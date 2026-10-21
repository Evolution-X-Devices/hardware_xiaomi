/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.biometrics.face-service.xiaomi"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cstring>

#include "Face.h"

using aidl::android::hardware::biometrics::face::AuthEngine;
using aidl::android::hardware::biometrics::face::Face;

int main(int argc, char** argv) {
    // argv strings are contiguous, together they make up /proc/self/cmdline.
    AuthEngine::setCmdline(argv[0], argv[argc - 1] + strlen(argv[argc - 1]) - argv[0]);

    ABinderProcess_setThreadPoolMaxThreadCount(0);

    std::shared_ptr<Face> face = ndk::SharedRefBase::make<Face>();
    const std::string instance = std::string(Face::descriptor) + "/default";
    binder_status_t status = AServiceManager_addService(face->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK);

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should not reach
}
