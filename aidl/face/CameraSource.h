/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <camera/NdkCameraManager.h>
#include <media/NdkImageReader.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace aidl::android::hardware::biometrics::face {

// Streams NV21 frames from the front camera, optionally rendering a preview into a surface.
class CameraSource {
  public:
    static constexpr int kWidth = 640;
    static constexpr int kHeight = 480;

    ~CameraSource();

    bool start(ANativeWindow* preview);
    void stop();

    // Waits for the next frame until the deadline or until shouldStop returns true.
    bool nextFrame(std::vector<uint8_t>* nv21, std::chrono::steady_clock::time_point deadline,
                   const std::function<bool()>& shouldStop);

    int sensorOrientation() const { return mSensorOrientation; }

  private:
    static void onImageAvailable(void* context, AImageReader* reader);
    static void onDisconnected(void* context, ACameraDevice* device);
    static void onError(void* context, ACameraDevice* device, int error);

    bool findFrontCamera();
    bool copyImage(AImage* image, std::vector<uint8_t>* nv21);
    void renderPreview(const std::vector<uint8_t>& nv21);

    ACameraManager* mManager = nullptr;
    std::string mCameraId;
    int mSensorOrientation = 270;

    ANativeWindow* mPreview = nullptr;
    ACameraDevice* mDevice = nullptr;
    AImageReader* mReader = nullptr;
    ACaptureSessionOutputContainer* mOutputs = nullptr;
    ACaptureSessionOutput* mSessionOutput = nullptr;
    ACameraOutputTarget* mTarget = nullptr;
    ACaptureRequest* mRequest = nullptr;
    ACameraCaptureSession* mSession = nullptr;

    std::mutex mMutex;
    std::condition_variable mCond;
    std::vector<uint8_t> mFrame;
    bool mFrameAvailable = false;
    bool mFailed = false;
};

}  // namespace aidl::android::hardware::biometrics::face
