/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/biometrics/face/AcquiredInfo.h>
#include <aidl/android/hardware/biometrics/face/Error.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "ST.h"

namespace aidl::android::hardware::biometrics::face {

struct Frame {
    const uint8_t* nv21;
    int width;
    int height;
};

// Drives the SenseTime st_auth SDK shipped in the stock MiuiBiometric app, which runs the whole
// pipeline and keeps the template in a file.
class AuthEngine {
  public:
    static constexpr int kEnrollFrames = 1;

    struct Result {
        AcquiredInfo info = AcquiredInfo::UNKNOWN;
        int remaining = -1;
        bool done = false;
        bool compared = false;
        int faceId = -1;
        std::optional<Error> error;
    };

    ~AuthEngine();

    // The license is bound to the stock app, which the SDK identifies by the process cmdline.
    static void setCmdline(char* cmdline, size_t size);

    bool init();

    std::optional<Error> beginEnroll(int sensorOrientation);
    Result enroll(const Frame& frame);
    std::optional<Error> beginAuthenticate(int sensorOrientation, bool requireAttention,
                                           int faceId);
    Result authenticate(const Frame& frame);
    bool removeFace(int faceId);

  private:
    static bool withStockCmdline(const std::function<bool()>& fn);

    bool loadSymbols();
    bool initLocked();
    void deinit();

    static char* sCmdline;
    static size_t sCmdlineSize;

    bool mInitialized = false;
    void* mLib = nullptr;
    int mConfig = 0;
    int mAccepted = 0;
    int mFaceId = -1;

#define ST_FN(ret, name, ...) ret (*name)(__VA_ARGS__) = nullptr
    ST_FN(int, initLicense, const char*);
    ST_FN(int, authInit, const char*, int);
    ST_FN(void, authDeinit);
    ST_FN(int, authReset);
    ST_FN(int, setFaceOrientation, int);
    ST_FN(int, setImageNumber, int);
    ST_FN(int, authEnroll, const st::st_image_t*, int, int*);
    ST_FN(int, authUnlock, const st::st_image_t*, int, const int*, int);
    ST_FN(int, deleteFace, int);
#undef ST_FN
};

}  // namespace aidl::android::hardware::biometrics::face
