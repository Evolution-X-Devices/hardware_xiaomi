/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/biometrics/face/BnSession.h>
#include <aidl/android/hardware/biometrics/face/ISessionCallback.h>
#include <thread/WorkerThread.h>

#include <future>
#include <map>
#include <mutex>
#include <set>

#include "AuthEngine.h"
#include "CameraSource.h"

namespace aidl::android::hardware::biometrics::face {

namespace common = aidl::android::hardware::biometrics::common;
namespace keymaster = aidl::android::hardware::keymaster;

using aidl::android::hardware::common::NativeHandle;

struct Hardware {
    std::mutex lock;
    AuthEngine auth;
    CameraSource camera;
};

struct LockoutTracker {
    int failures = 0;
    std::chrono::steady_clock::time_point lockedUntil;
};

class Session : public BnSession {
  public:
    Session(std::shared_ptr<Hardware> hw, int32_t userId, std::shared_ptr<ISessionCallback> cb);

    ndk::ScopedAStatus generateChallenge() override;
    ndk::ScopedAStatus revokeChallenge(int64_t challenge) override;
    ndk::ScopedAStatus getEnrollmentConfig(EnrollmentType enrollmentType,
                                           std::vector<EnrollmentStageConfig>* return_val) override;
    ndk::ScopedAStatus enroll(const keymaster::HardwareAuthToken& hat,
                              EnrollmentType enrollmentType, const std::vector<Feature>& features,
                              const std::optional<NativeHandle>& previewSurface,
                              std::shared_ptr<common::ICancellationSignal>* return_val) override;
    ndk::ScopedAStatus authenticate(
            int64_t operationId, std::shared_ptr<common::ICancellationSignal>* return_val) override;
    ndk::ScopedAStatus detectInteraction(
            std::shared_ptr<common::ICancellationSignal>* return_val) override;
    ndk::ScopedAStatus enumerateEnrollments() override;
    ndk::ScopedAStatus removeEnrollments(const std::vector<int32_t>& enrollmentIds) override;
    ndk::ScopedAStatus getFeatures() override;
    ndk::ScopedAStatus setFeature(const keymaster::HardwareAuthToken& hat, Feature feature,
                                  bool enabled) override;
    ndk::ScopedAStatus getAuthenticatorId() override;
    ndk::ScopedAStatus invalidateAuthenticatorId() override;
    ndk::ScopedAStatus resetLockout(const keymaster::HardwareAuthToken& hat) override;
    ndk::ScopedAStatus close() override;
    ndk::ScopedAStatus authenticateWithContext(
            int64_t operationId, const common::OperationContext& context,
            std::shared_ptr<common::ICancellationSignal>* return_val) override;
    ndk::ScopedAStatus enrollWithContext(
            const keymaster::HardwareAuthToken& hat, EnrollmentType enrollmentType,
            const std::vector<Feature>& features, const std::optional<NativeHandle>& previewSurface,
            const common::OperationContext& context,
            std::shared_ptr<common::ICancellationSignal>* return_val) override;
    ndk::ScopedAStatus detectInteractionWithContext(
            const common::OperationContext& context,
            std::shared_ptr<common::ICancellationSignal>* return_val) override;
    ndk::ScopedAStatus onContextChanged(const common::OperationContext& context) override;
    ndk::ScopedAStatus enrollWithOptions(
            const FaceEnrollOptions& options,
            std::shared_ptr<common::ICancellationSignal>* return_val) override;

  private:
    std::shared_ptr<common::ICancellationSignal> schedule(
            std::function<void(const std::future<void>&)> op);

    std::shared_ptr<common::ICancellationSignal> scheduleEnroll(
            const keymaster::HardwareAuthToken& hat, ANativeWindow* preview);
    void doEnroll(const keymaster::HardwareAuthToken& hat, ANativeWindow* preview,
                  const std::future<void>& cancel);
    void doAuthenticate(const std::future<void>& cancel);
    bool prepare();
    void onAuthenticationFailedLocked();
    bool checkLockoutLocked();

    std::shared_ptr<Hardware> mHw;
    int32_t mUserId;
    std::shared_ptr<ISessionCallback> mCb;
    WorkerThread mWorker;

    std::mutex mChallengeLock;
    std::set<int64_t> mChallenges;

    static std::map<int32_t, LockoutTracker> sLockouts;
};

}  // namespace aidl::android::hardware::biometrics::face
