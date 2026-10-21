/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.biometrics.face-service.xiaomi"

#include "CameraSource.h"

#include <android-base/logging.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraMetadata.h>

#include <algorithm>
#include <cstring>

namespace aidl::android::hardware::biometrics::face {

namespace {

constexpr int kMaxImages = 4;
constexpr int kPreviewSize = std::min(CameraSource::kWidth, CameraSource::kHeight);

uint8_t clamp8(int v) {
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

}  // namespace

CameraSource::~CameraSource() {
    stop();
    if (mManager) ACameraManager_delete(mManager);
}

bool CameraSource::findFrontCamera() {
    if (!mCameraId.empty()) return true;
    if (!mManager) mManager = ACameraManager_create();

    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(mManager, &ids) != ACAMERA_OK) {
        LOG(ERROR) << "Failed to list cameras";
        return false;
    }

    for (int i = 0; i < ids->numCameras && mCameraId.empty(); i++) {
        ACameraMetadata* chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(mManager, ids->cameraIds[i], &chars) !=
            ACAMERA_OK) {
            continue;
        }

        ACameraMetadata_const_entry entry;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK &&
            entry.data.u8[0] == ACAMERA_LENS_FACING_FRONT) {
            mCameraId = ids->cameraIds[i];
            if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_ORIENTATION, &entry) ==
                ACAMERA_OK) {
                mSensorOrientation = entry.data.i32[0];
            }
        }
        ACameraMetadata_free(chars);
    }
    ACameraManager_deleteCameraIdList(ids);

    if (mCameraId.empty()) {
        LOG(ERROR) << "No front camera found";
        return false;
    }
    return true;
}

void CameraSource::onImageAvailable(void* context, AImageReader* reader) {
    auto* self = static_cast<CameraSource*>(context);
    AImage* image = nullptr;
    if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || !image) return;

    std::vector<uint8_t> nv21;
    bool ok = self->copyImage(image, &nv21);
    AImage_delete(image);
    if (!ok) return;

    if (self->mPreview) self->renderPreview(nv21);

    std::lock_guard lock(self->mMutex);
    self->mFrame = std::move(nv21);
    self->mFrameAvailable = true;
    self->mCond.notify_all();
}

// Draws a mirrored, upright, center cropped square, like a selfie camera preview.
void CameraSource::renderPreview(const std::vector<uint8_t>& nv21) {
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(mPreview, &buffer, nullptr) != 0) return;

    const int rotation = (360 - mSensorOrientation % 360) % 360;
    const int rotatedWidth = rotation % 180 ? kHeight : kWidth;
    const int rotatedHeight = rotation % 180 ? kWidth : kHeight;
    const int offsetX = (rotatedWidth - kPreviewSize) / 2;
    const int offsetY = (rotatedHeight - kPreviewSize) / 2;
    const uint8_t* vu = nv21.data() + kWidth * kHeight;

    for (int v = 0; v < kPreviewSize; v++) {
        auto* row = static_cast<uint32_t*>(buffer.bits) + v * buffer.stride;
        for (int u = 0; u < kPreviewSize; u++) {
            int rx = u + offsetX, ry = v + offsetY;
            int mx, my;
            switch (rotation) {
                case 90:
                    mx = ry, my = kHeight - 1 - rx;
                    break;
                case 180:
                    mx = kWidth - 1 - rx, my = kHeight - 1 - ry;
                    break;
                case 270:
                    mx = kWidth - 1 - ry, my = rx;
                    break;
                default:
                    mx = rx, my = ry;
                    break;
            }
            int x = kWidth - 1 - mx, y = my;

            int luma = nv21[y * kWidth + x];
            const uint8_t* c = vu + (y / 2) * kWidth + (x & ~1);
            int cr = c[0] - 128, cb = c[1] - 128;
            row[u] = 0xff000000u | clamp8(luma + ((116130 * cb) >> 16)) << 16 |
                     clamp8(luma - ((22554 * cb + 46802 * cr) >> 16)) << 8 |
                     clamp8(luma + ((91881 * cr) >> 16));
        }
    }
    ANativeWindow_unlockAndPost(mPreview);
}

void CameraSource::onDisconnected(void* context, ACameraDevice*) {
    auto* self = static_cast<CameraSource*>(context);
    LOG(WARNING) << "Camera disconnected";
    std::lock_guard lock(self->mMutex);
    self->mFailed = true;
    self->mCond.notify_all();
}

void CameraSource::onError(void* context, ACameraDevice*, int error) {
    auto* self = static_cast<CameraSource*>(context);
    LOG(ERROR) << "Camera error " << error;
    std::lock_guard lock(self->mMutex);
    self->mFailed = true;
    self->mCond.notify_all();
}

bool CameraSource::start(ANativeWindow* preview) {
    if (!findFrontCamera()) return false;

    if (preview) {
        ANativeWindow_acquire(preview);
        ANativeWindow_setBuffersGeometry(preview, kPreviewSize, kPreviewSize,
                                         AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        mPreview = preview;
    }

    {
        std::lock_guard lock(mMutex);
        mFrameAvailable = false;
        mFailed = false;
    }

    ACameraDevice_StateCallbacks deviceCallbacks = {
            .context = this,
            .onDisconnected = onDisconnected,
            .onError = onError,
    };
    camera_status_t ret =
            ACameraManager_openCamera(mManager, mCameraId.c_str(), &deviceCallbacks, &mDevice);
    if (ret != ACAMERA_OK) {
        LOG(ERROR) << "Failed to open camera " << mCameraId << ": " << ret;
        mDevice = nullptr;
        stop();
        return false;
    }

    if (AImageReader_new(kWidth, kHeight, AIMAGE_FORMAT_YUV_420_888, kMaxImages, &mReader) !=
        AMEDIA_OK) {
        LOG(ERROR) << "Failed to create image reader";
        stop();
        return false;
    }
    AImageReader_ImageListener listener = {.context = this, .onImageAvailable = onImageAvailable};
    AImageReader_setImageListener(mReader, &listener);

    ANativeWindow* readerWindow = nullptr;
    AImageReader_getWindow(mReader, &readerWindow);
    ACaptureSessionOutputContainer_create(&mOutputs);
    ACaptureSessionOutput_create(readerWindow, &mSessionOutput);
    ACaptureSessionOutputContainer_add(mOutputs, mSessionOutput);
    ACameraDevice_createCaptureRequest(mDevice, TEMPLATE_PREVIEW, &mRequest);
    ACameraOutputTarget_create(readerWindow, &mTarget);
    ACaptureRequest_addTarget(mRequest, mTarget);

    ACameraCaptureSession_stateCallbacks sessionCallbacks = {
            .onClosed = [](void*, ACameraCaptureSession*) {},
            .onReady = [](void*, ACameraCaptureSession*) {},
            .onActive = [](void*, ACameraCaptureSession*) {},
    };
    ret = ACameraDevice_createCaptureSession(mDevice, mOutputs, &sessionCallbacks, &mSession);
    if (ret != ACAMERA_OK) {
        LOG(ERROR) << "Failed to create capture session: " << ret;
        mSession = nullptr;
        stop();
        return false;
    }

    ret = ACameraCaptureSession_setRepeatingRequest(mSession, nullptr, 1, &mRequest, nullptr);
    if (ret != ACAMERA_OK) {
        LOG(ERROR) << "Failed to start repeating request: " << ret;
        stop();
        return false;
    }
    return true;
}

void CameraSource::stop() {
    if (mSession) {
        ACameraCaptureSession_stopRepeating(mSession);
        ACameraCaptureSession_close(mSession);
        mSession = nullptr;
    }
    if (mDevice) {
        ACameraDevice_close(mDevice);
        mDevice = nullptr;
    }
    if (mRequest) {
        ACaptureRequest_removeTarget(mRequest, mTarget);
        ACameraOutputTarget_free(mTarget);
        ACaptureRequest_free(mRequest);
        mTarget = nullptr;
        mRequest = nullptr;
    }
    if (mOutputs) {
        ACaptureSessionOutputContainer_remove(mOutputs, mSessionOutput);
        ACaptureSessionOutput_free(mSessionOutput);
        ACaptureSessionOutputContainer_free(mOutputs);
        mSessionOutput = nullptr;
        mOutputs = nullptr;
    }
    if (mReader) {
        AImageReader_delete(mReader);
        mReader = nullptr;
    }
    if (mPreview) {
        ANativeWindow_release(mPreview);
        mPreview = nullptr;
    }
}

bool CameraSource::nextFrame(std::vector<uint8_t>* nv21,
                             std::chrono::steady_clock::time_point deadline,
                             const std::function<bool()>& shouldStop) {
    std::unique_lock lock(mMutex);
    while (!mFrameAvailable && !mFailed) {
        // Wake up periodically to check shouldStop.
        auto wakeup = std::min(deadline,
                               std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
        mCond.wait_until(lock, wakeup);
        if (shouldStop() || std::chrono::steady_clock::now() >= deadline) return false;
    }
    if (mFailed) return false;

    mFrameAvailable = false;
    *nv21 = std::move(mFrame);
    return true;
}

bool CameraSource::copyImage(AImage* image, std::vector<uint8_t>* nv21) {
    int32_t width = 0, height = 0;
    AImage_getWidth(image, &width);
    AImage_getHeight(image, &height);
    if (width != kWidth || height != kHeight) return false;

    uint8_t *y, *u, *v;
    int yLen, uLen, vLen;
    int32_t yStride, uvStride, uvPixelStride;
    if (AImage_getPlaneData(image, 0, &y, &yLen) != AMEDIA_OK ||
        AImage_getPlaneData(image, 1, &u, &uLen) != AMEDIA_OK ||
        AImage_getPlaneData(image, 2, &v, &vLen) != AMEDIA_OK) {
        return false;
    }
    AImage_getPlaneRowStride(image, 0, &yStride);
    AImage_getPlaneRowStride(image, 1, &uvStride);
    AImage_getPlanePixelStride(image, 1, &uvPixelStride);

    nv21->resize(width * height * 3 / 2);
    uint8_t* dst = nv21->data();
    for (int row = 0; row < height; row++) {
        memcpy(dst + row * width, y + row * yStride, width);
    }

    uint8_t* vu = dst + width * height;
    for (int row = 0; row < height / 2; row++) {
        const uint8_t* uRow = u + row * uvStride;
        const uint8_t* vRow = v + row * uvStride;
        for (int col = 0; col < width / 2; col++) {
            *vu++ = vRow[col * uvPixelStride];
            *vu++ = uRow[col * uvPixelStride];
        }
    }
    return true;
}

}  // namespace aidl::android::hardware::biometrics::face
