/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.biometrics.face-service.xiaomi"

#include "AuthEngine.h"

#include <android-base/logging.h>
#include <dlfcn.h>
#include <unistd.h>
#include <ziparchive/zip_archive.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace aidl::android::hardware::biometrics::face {

using namespace st;

namespace {

constexpr const char* kCmdline = "com.miui.face";
constexpr const char* kResources = "/vendor/etc/faceunlock/resources.zip";
constexpr const char* kVendorLibraries[] = {
        "/vendor/lib64/libstfaceunlockocl.so",
        "/vendor/lib64/libstfaceunlockppl.so",
};
constexpr const char* kResourcesLibrary =
        "/vendor/etc/faceunlock/resources.zip!/lib/arm64-v8a/libstfaceunlockocl.so";
constexpr const char* kDexEntry = "classes.dex";
constexpr std::string_view kLicenseMarker = "License Product: FaceAuth";
constexpr const char* kTemplateDir = "/data/vendor/face/st_auth";

bool readEntry(const char* name, std::string* out) {
    ZipArchiveHandle archive = nullptr;
    ZipEntry64 entry;
    bool ok = OpenArchive(kResources, &archive) == 0 && FindEntry(archive, name, &entry) == 0;
    if (ok) {
        out->resize(entry.uncompressed_length);
        ok = ExtractToMemory(archive, &entry, reinterpret_cast<uint8_t*>(out->data()),
                             out->size()) == 0;
    }
    CloseArchive(archive);
    return ok;
}

// The stock app passes the license to the SDK from a string constant in its dex.
std::optional<std::string> findLicense(const std::string& dex) {
    auto u32 = [&](size_t off) -> std::optional<uint32_t> {
        if (off > dex.size() || dex.size() - off < sizeof(uint32_t)) return std::nullopt;
        uint32_t value;
        memcpy(&value, dex.data() + off, sizeof(value));
        return value;
    };

    auto count = u32(0x38), table = u32(0x3c);
    if (dex.compare(0, 4, "dex\n") != 0 || !count || !table) return std::nullopt;

    for (uint32_t i = 0; i < *count; i++) {
        auto offset = u32(*table + static_cast<size_t>(i) * sizeof(uint32_t));
        if (!offset) return std::nullopt;

        // string_data_item: uleb128 UTF-16 length, then NUL terminated MUTF-8.
        size_t start = *offset;
        while (start < dex.size() && (dex[start] & 0x80)) start++;
        size_t end = dex.find('\0', ++start);
        if (end == std::string::npos) return std::nullopt;

        std::string_view string(dex.data() + start, end - start);
        if (string.find(kLicenseMarker) != std::string_view::npos) return std::string(string);
    }
    return std::nullopt;
}

st_image_t toImage(const Frame& frame) {
    return {frame.nv21, ST_PIX_FMT_NV21, frame.width, frame.height, frame.width};
}

int faceOrientation(int sensorOrientation) {
    switch (sensorOrientation) {
        case 0:
            return ST_FACE_UP;
        case 90:
            return ST_FACE_LEFT;
        case 180:
            return ST_FACE_DOWN;
        default:
            return ST_FACE_RIGHT;
    }
}

AcquiredInfo toAcquiredInfo(int status) {
    switch (status) {
        case ST_AUTH_OK:
        case ST_AUTH_ENROLL_KEEP:
        case ST_AUTH_ENROLL_OK:
        case ST_AUTH_UNLOCK_OK:
        case ST_AUTH_UNLOCK_FAILED:
            return AcquiredInfo::GOOD;
        case ST_AUTH_NO_FACE:
        case ST_AUTH_QUALITY_FACE_MISS:
            return AcquiredInfo::NOT_DETECTED;
        case ST_AUTH_EYE_CLOSE:
            return AcquiredInfo::POOR_GAZE;
        case ST_AUTH_OVER_EXPOSE:
        case ST_AUTH_QUALITY_BRIGHT_HIGH:
            return AcquiredInfo::TOO_BRIGHT;
        case ST_AUTH_QUALITY_BRIGHT_LOW:
            return AcquiredInfo::TOO_DARK;
        case ST_AUTH_EYE_BLOCKED:
        case ST_AUTH_MOUTH_BLOCKED:
        case ST_AUTH_QUALITY_MOUTH:
        case ST_AUTH_QUALITY_OCCLUSION:
            return AcquiredInfo::FACE_OBSCURED;
        case ST_AUTH_FACE_UP:
        case ST_AUTH_FACE_DOWN:
            return AcquiredInfo::TILT_TOO_EXTREME;
        case ST_AUTH_FACE_LEFT:
        case ST_AUTH_FACE_RIGHT:
            return AcquiredInfo::PAN_TOO_EXTREME;
        case ST_AUTH_QUALITY_SHARP:
            return AcquiredInfo::TOO_MUCH_MOTION;
        case ST_AUTH_QUALITY_DISTANCE:
        case ST_AUTH_QUALITY_FACE_SMALL:
            return AcquiredInfo::TOO_FAR;
        case ST_AUTH_QUALITY_FACE_BIG:
            return AcquiredInfo::TOO_CLOSE;
        default:
            return AcquiredInfo::INSUFFICIENT;
    }
}

}  // namespace

char* AuthEngine::sCmdline = nullptr;
size_t AuthEngine::sCmdlineSize = 0;

void AuthEngine::setCmdline(char* cmdline, size_t size) {
    sCmdline = cmdline;
    sCmdlineSize = size;
}

bool AuthEngine::withStockCmdline(const std::function<bool()>& fn) {
    if (!sCmdline || sCmdlineSize <= strlen(kCmdline)) {
        LOG(ERROR) << "Unable to present cmdline";
        return false;
    }
    std::string saved(sCmdline, sCmdlineSize);
    memset(sCmdline, 0, sCmdlineSize);
    strcpy(sCmdline, kCmdline);
    bool ret = fn();
    memcpy(sCmdline, saved.data(), sCmdlineSize);
    return ret;
}

AuthEngine::~AuthEngine() {
    deinit();
}

bool AuthEngine::loadSymbols() {
    if (mLib) return true;

    // Some stock apps bundle the engine in their APK, others install it as a system library.
    auto vendor = std::find_if(std::begin(kVendorLibraries), std::end(kVendorLibraries),
                               [](const char* path) { return access(path, F_OK) == 0; });
    const char* path = vendor != std::end(kVendorLibraries) ? *vendor : kResourcesLibrary;
    void* lib = dlopen(path, RTLD_NOW);
    if (!lib) {
        LOG(ERROR) << "Failed to load " << path << ": " << dlerror();
        return false;
    }

    bool ok = true;
    auto load = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
        if (!fn) {
            LOG(ERROR) << "Missing symbol " << name;
            ok = false;
        }
    };
    load(initLicense, "st_face_init_license");
    load(authInit, "st_auth_init");
    load(authDeinit, "st_auth_deinit");
    load(authReset, "st_auth_reset");
    load(setFaceOrientation, "st_auth_set_face_orientation");
    load(setImageNumber, "st_auth_set_faceid_image_number");
    load(authEnroll, "st_auth_enroll");
    load(authUnlock, "st_auth_unlock");
    load(deleteFace, "st_auth_delete_face");
    if (ok) mLib = lib;
    return ok;
}

bool AuthEngine::init() {
    if (mInitialized) return true;
    if (!loadSymbols()) return false;

    mInitialized = withStockCmdline([this] { return initLocked(); });
    if (!mInitialized) deinit();
    return mInitialized;
}

bool AuthEngine::initLocked() {
    std::string dex;
    if (!readEntry(kDexEntry, &dex)) {
        LOG(ERROR) << "Failed to read " << kDexEntry << " from " << kResources;
        return false;
    }
    std::optional<std::string> license = findLicense(dex);
    if (!license) {
        LOG(ERROR) << "No st_auth license in " << kDexEntry;
        return false;
    }

    int ret = initLicense(license->c_str());
    if (ret != 0) {
        LOG(ERROR) << "License rejected: " << ret;
        return false;
    }

    ret = authInit(kTemplateDir, 0);
    if (ret != 0) {
        LOG(ERROR) << "Failed to initialize st_auth: " << ret;
        return false;
    }
    return true;
}

void AuthEngine::deinit() {
    if (mLib) authDeinit();
    mInitialized = false;
}

std::optional<Error> AuthEngine::beginEnroll(int sensorOrientation) {
    mAccepted = 0;
    authReset();

    int ret;
    if ((ret = setImageNumber(kEnrollFrames)) != 0 ||
        (ret = setFaceOrientation(faceOrientation(sensorOrientation))) != 0) {
        LOG(ERROR) << "Failed to configure enrollment: " << ret;
        return Error::UNABLE_TO_PROCESS;
    }
    return std::nullopt;
}

AuthEngine::Result AuthEngine::enroll(const Frame& frame) {
    st_image_t image = toImage(frame);
    int faceId = -1;
    int status = authEnroll(&image, 0, &faceId);

    Result result{.info = toAcquiredInfo(status)};
    switch (status) {
        case ST_AUTH_OK:
        case ST_AUTH_ENROLL_OK:
            if (faceId < 0) {
                LOG(ERROR) << "Enrollment finished without a face id";
                result.error = Error::UNABLE_TO_PROCESS;
                break;
            }
            result.remaining = 0;
            result.done = true;
            result.faceId = faceId;
            break;
        case ST_AUTH_ENROLL_KEEP:
            result.remaining = std::max(kEnrollFrames - ++mAccepted, 1);
            break;
        case ST_E_TEMPLATE_UPPER_LIMIT:
            result.error = Error::NO_SPACE;
            break;
        case ST_E_HANDLE:
        case ST_E_TEMPLATE_DAMAGED:
            LOG(ERROR) << "Enrollment failed: " << status;
            result.error = Error::UNABLE_TO_PROCESS;
            break;
    }
    return result;
}

std::optional<Error> AuthEngine::beginAuthenticate(int sensorOrientation, bool requireAttention,
                                                  int faceId) {
    mConfig = requireAttention ? 0 : ST_AUTH_CLOSE_EYE_STATUS;
    mFaceId = faceId;
    authReset();
    if (int ret = setFaceOrientation(faceOrientation(sensorOrientation)); ret != 0) {
        LOG(ERROR) << "Failed to configure authentication: " << ret;
        return Error::UNABLE_TO_PROCESS;
    }
    return std::nullopt;
}

AuthEngine::Result AuthEngine::authenticate(const Frame& frame) {
    st_image_t image = toImage(frame);
    // The engine only compares against the listed face ids, so other users' faces never match.
    int status = authUnlock(&image, mConfig, &mFaceId, 1);

    Result result{.info = toAcquiredInfo(status)};
    switch (status) {
        case ST_AUTH_UNLOCK_OK:
            result.done = true;
            [[fallthrough]];
        case ST_AUTH_UNLOCK_FAILED:
        case ST_AUTH_HACKNESS_FAILED:
            result.compared = true;
            break;
        case ST_E_HANDLE:
            result.error = Error::HW_UNAVAILABLE;
            break;
        case ST_E_TEMPLATE_DAMAGED:
        case ST_AUTH_TEMPLATE_NOT_EXIST:
        case ST_AUTH_TEMPLATE_NO_FACE:
            LOG(ERROR) << "Template unusable: " << status;
            result.error = Error::REENROLL_REQUIRED;
            break;
    }
    return result;
}

bool AuthEngine::removeFace(int faceId) {
    if (!init()) return false;

    authReset();
    int ret = deleteFace(faceId);
    if (ret != 0 && ret != ST_E_DELETE_ID_NOT_EXIST && ret != ST_AUTH_TEMPLATE_NOT_EXIST &&
        ret != ST_AUTH_TEMPLATE_NO_FACE) {
        LOG(ERROR) << "Failed to delete face " << faceId << ": " << ret;
        return false;
    }
    return true;
}

}  // namespace aidl::android::hardware::biometrics::face
