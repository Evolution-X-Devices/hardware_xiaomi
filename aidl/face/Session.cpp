/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.biometrics.face-service.xiaomi"

#include "Session.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android/native_window_aidl.h>
#include <unistd.h>
#include <util/CancellationSignal.h>

#include <cerrno>
#include <random>

namespace aidl::android::hardware::biometrics::face {

using namespace std::chrono_literals;
using ::aidl::android::hardware::NativeWindow;

namespace {

constexpr const char* kEnrollmentsFile = "/data/vendor/face/enrollments";

constexpr auto kAuthTimeout = 5s;
constexpr auto kEnrollTimeout = 60s;

constexpr int kLockoutTimedFailures = 5;
constexpr int kLockoutPermanentFailures = 20;
constexpr auto kLockoutDuration = 30s;

int64_t randomId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<int64_t> dist(1, INT64_MAX);
    return dist(rng);
}

AuthenticationFrame authFrame(AcquiredInfo info) {
    AuthenticationFrame frame;
    frame.data.acquiredInfo = info;
    return frame;
}

EnrollmentFrame enrollFrame(AcquiredInfo info, EnrollmentStage stage) {
    EnrollmentFrame frame;
    frame.stage = stage;
    frame.data.acquiredInfo = info;
    return frame;
}

// One face per user. Every user's face lives in the engine's single template under its own id.
struct Enrollment {
    int32_t enrollmentId = 0;
    int32_t faceId = -1;
    int64_t authenticatorId = 0;
    bool requireAttention = true;
};

// Keyed by user id.
using Enrollments = std::map<int32_t, Enrollment>;

Enrollments loadEnrollments() {
    Enrollments enrollments;
    std::string content;
    if (!::android::base::ReadFileToString(kEnrollmentsFile, &content)) return enrollments;

    for (const std::string& line : ::android::base::Split(content, "\n")) {
        if (line.empty()) continue;
        auto fields = ::android::base::Split(line, " ");
        int32_t userId;
        Enrollment e;
        if (fields.size() != 5 || !::android::base::ParseInt(fields[0], &userId) ||
            !::android::base::ParseInt(fields[1], &e.enrollmentId) ||
            !::android::base::ParseInt(fields[2], &e.faceId) ||
            !::android::base::ParseInt(fields[3], &e.authenticatorId)) {
            LOG(ERROR) << "Ignoring malformed enrollment: " << line;
            continue;
        }
        e.requireAttention = fields[4] == "1";
        enrollments[userId] = e;
    }
    return enrollments;
}

bool saveEnrollments(const Enrollments& enrollments) {
    std::string content;
    for (const auto& [userId, e] : enrollments) {
        content += std::to_string(userId) + " " + std::to_string(e.enrollmentId) + " " +
                   std::to_string(e.faceId) + " " + std::to_string(e.authenticatorId) + " " +
                   (e.requireAttention ? "1" : "0") + "\n";
    }
    return ::android::base::WriteStringToFile(content, kEnrollmentsFile);
}

// vold deletes /data/vendor_de/<user> together with the user, e.g. a deleted private space.
bool userRemoved(int32_t userId) {
    std::string path = "/data/vendor_de/" + std::to_string(userId);
    return access(path.c_str(), F_OK) != 0 && errno == ENOENT;
}

// The framework never cleans up after removed users, so free their template slots here.
void pruneRemovedUsers(AuthEngine& auth, Enrollments* enrollments) {
    bool changed = false;
    for (auto it = enrollments->begin(); it != enrollments->end();) {
        if (!userRemoved(it->first) || !auth.removeFace(it->second.faceId)) {
            ++it;
            continue;
        }
        LOG(INFO) << "Removed the face of deleted user " << it->first;
        it = enrollments->erase(it);
        changed = true;
    }
    if (changed && !saveEnrollments(*enrollments)) PLOG(ERROR) << "Failed to save enrollments";
}

}  // namespace

std::map<int32_t, LockoutTracker> Session::sLockouts;

Session::Session(std::shared_ptr<Hardware> hw, int32_t userId, std::shared_ptr<ISessionCallback> cb)
    : mHw(std::move(hw)), mUserId(userId), mCb(std::move(cb)), mWorker(16) {}

std::shared_ptr<common::ICancellationSignal> Session::schedule(
        std::function<void(const std::future<void>&)> op) {
    std::promise<void> promise;
    std::future<void> future = promise.get_future();
    mWorker.schedule(Callable::from([this, op = std::move(op), future = std::move(future)] {
        std::lock_guard lock(mHw->lock);
        op(future);
    }));
    return ndk::SharedRefBase::make<CancellationSignal>(std::move(promise));
}

bool Session::prepare() {
    if (!mHw->auth.init()) {
        mCb->onError(Error::HW_UNAVAILABLE, 0);
        return false;
    }
    return true;
}

ndk::ScopedAStatus Session::generateChallenge() {
    int64_t challenge = randomId();
    {
        std::lock_guard lock(mChallengeLock);
        mChallenges.insert(challenge);
    }
    mCb->onChallengeGenerated(challenge);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    {
        std::lock_guard lock(mChallengeLock);
        mChallenges.erase(challenge);
    }
    mCb->onChallengeRevoked(challenge);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getEnrollmentConfig(EnrollmentType,
                                                std::vector<EnrollmentStageConfig>* return_val) {
    *return_val = {};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enroll(const keymaster::HardwareAuthToken& hat, EnrollmentType,
                                   const std::vector<Feature>&,
                                   const std::optional<NativeHandle>& /*previewSurface*/,
                                   std::shared_ptr<common::ICancellationSignal>* return_val) {
    *return_val = scheduleEnroll(hat, nullptr);
    return ndk::ScopedAStatus::ok();
}

std::shared_ptr<common::ICancellationSignal> Session::scheduleEnroll(
        const keymaster::HardwareAuthToken& hat, ANativeWindow* preview) {
    auto window = std::make_shared<NativeWindow>(preview);
    return schedule([this, hat, window](const std::future<void>& cancel) {
        doEnroll(hat, window->get(), cancel);
    });
}

void Session::doEnroll(const keymaster::HardwareAuthToken& hat, ANativeWindow* preview,
                       const std::future<void>& cancel) {
    {
        std::lock_guard lock(mChallengeLock);
        if (!mChallenges.count(hat.challenge)) {
            LOG(ERROR) << "Enrollment requested with unknown challenge";
            mCb->onError(Error::UNABLE_TO_PROCESS, 0);
            return;
        }
    }

    Enrollments enrollments = loadEnrollments();
    if (enrollments.count(mUserId)) {
        LOG(ERROR) << "A face is already enrolled for user " << mUserId;
        mCb->onError(Error::NO_SPACE, 0);
        return;
    }
    if (!prepare()) return;

    AuthEngine& auth = mHw->auth;
    pruneRemovedUsers(auth, &enrollments);

    CameraSource& camera = mHw->camera;
    if (!camera.start(preview)) {
        mCb->onError(Error::HW_UNAVAILABLE, 0);
        return;
    }
    if (auto error = auth.beginEnroll(camera.sensorOrientation())) {
        camera.stop();
        mCb->onError(*error, 0);
        return;
    }

    int32_t enrollmentId = static_cast<int32_t>(randomId() & INT32_MAX);
    auto deadline = std::chrono::steady_clock::now() + kEnrollTimeout;
    auto cancelled = [&] { return shouldCancel(cancel); };

    std::vector<uint8_t> nv21;
    std::optional<Error> error;
    bool done = false;
    int faceId = -1;
    int lastRemaining = AuthEngine::kEnrollFrames;
    AcquiredInfo lastInfo = AcquiredInfo::UNKNOWN;
    bool first = true;

    while (camera.nextFrame(&nv21, deadline, cancelled)) {
        if (first) {
            mCb->onEnrollmentFrame(enrollFrame(AcquiredInfo::FIRST_FRAME_RECEIVED,
                                               EnrollmentStage::FIRST_FRAME_RECEIVED));
            first = false;
        }

        Frame frame = {nv21.data(), CameraSource::kWidth, CameraSource::kHeight};
        AuthEngine::Result result = auth.enroll(frame);
        if (result.error) {
            error = result.error;
            break;
        }
        if (result.info != lastInfo) {
            mCb->onEnrollmentFrame(enrollFrame(result.info, EnrollmentStage::HOLD_STILL_IN_CENTER));
            lastInfo = result.info;
        }
        if (result.done) {
            done = true;
            faceId = result.faceId;
            break;
        }
        if (result.remaining > 0 && result.remaining != lastRemaining) {
            mCb->onEnrollmentProgress(enrollmentId, result.remaining);
            lastRemaining = result.remaining;
        }
    }
    camera.stop();

    if (!done) {
        if (!error) error = cancelled() ? Error::CANCELED : Error::TIMEOUT;
        mCb->onError(*error, 0);
        return;
    }

    enrollments[mUserId] = {
            .enrollmentId = enrollmentId, .faceId = faceId, .authenticatorId = randomId()};
    if (!saveEnrollments(enrollments)) {
        PLOG(ERROR) << "Failed to save enrollment";
        auth.removeFace(faceId);
        mCb->onError(Error::UNABLE_TO_PROCESS, 0);
        return;
    }

    mCb->onEnrollmentFrame(enrollFrame(AcquiredInfo::GOOD, EnrollmentStage::ENROLLMENT_FINISHED));
    mCb->onEnrollmentProgress(enrollmentId, 0);
}

ndk::ScopedAStatus Session::authenticate(int64_t,
                                         std::shared_ptr<common::ICancellationSignal>* return_val) {
    *return_val = schedule([this](const std::future<void>& cancel) { doAuthenticate(cancel); });
    return ndk::ScopedAStatus::ok();
}

bool Session::checkLockoutLocked() {
    LockoutTracker& lockout = sLockouts[mUserId];
    if (lockout.failures >= kLockoutPermanentFailures) {
        mCb->onLockoutPermanent();
        return true;
    }
    auto now = std::chrono::steady_clock::now();
    if (lockout.lockedUntil > now) {
        mCb->onLockoutTimed(
                std::chrono::duration_cast<std::chrono::milliseconds>(lockout.lockedUntil - now)
                        .count());
        return true;
    }
    return false;
}

void Session::onAuthenticationFailedLocked() {
    mCb->onAuthenticationFailed();

    LockoutTracker& lockout = sLockouts[mUserId];
    lockout.failures++;
    if (lockout.failures >= kLockoutPermanentFailures) {
        mCb->onLockoutPermanent();
    } else if (lockout.failures % kLockoutTimedFailures == 0) {
        lockout.lockedUntil = std::chrono::steady_clock::now() + kLockoutDuration;
        mCb->onLockoutTimed(std::chrono::milliseconds(kLockoutDuration).count());
    }
}

void Session::doAuthenticate(const std::future<void>& cancel) {
    Enrollments enrollments = loadEnrollments();
    auto it = enrollments.find(mUserId);
    if (it == enrollments.end()) {
        mCb->onError(Error::UNABLE_TO_PROCESS, 0);
        return;
    }
    const Enrollment& enrollment = it->second;
    if (checkLockoutLocked() || !prepare()) return;

    AuthEngine& auth = mHw->auth;
    CameraSource& camera = mHw->camera;
    if (!camera.start(nullptr)) {
        mCb->onError(Error::HW_UNAVAILABLE, 0);
        return;
    }
    if (auto error = auth.beginAuthenticate(camera.sensorOrientation(),
                                            enrollment.requireAttention, enrollment.faceId)) {
        camera.stop();
        mCb->onError(*error, 0);
        return;
    }

    auto deadline = std::chrono::steady_clock::now() + kAuthTimeout;
    auto cancelled = [&] { return shouldCancel(cancel); };

    std::vector<uint8_t> nv21;
    std::optional<Error> error;
    bool compared = false;
    bool matched = false;
    AcquiredInfo lastInfo = AcquiredInfo::UNKNOWN;

    while (camera.nextFrame(&nv21, deadline, cancelled)) {
        Frame frame = {nv21.data(), CameraSource::kWidth, CameraSource::kHeight};
        AuthEngine::Result result = auth.authenticate(frame);
        if (result.error) {
            error = result.error;
            break;
        }
        if (result.info != lastInfo) {
            mCb->onAuthenticationFrame(authFrame(result.info));
            lastInfo = result.info;
        }
        compared |= result.compared;
        if (result.done) {
            matched = true;
            break;
        }
    }
    camera.stop();

    if (matched) {
        sLockouts.erase(mUserId);
        // Only strong sensors mint auth tokens.
        mCb->onAuthenticationSucceeded(enrollment.enrollmentId, keymaster::HardwareAuthToken());
    } else if (error) {
        mCb->onError(*error, 0);
    } else if (cancelled()) {
        mCb->onError(Error::CANCELED, 0);
    } else if (compared) {
        onAuthenticationFailedLocked();
    } else {
        mCb->onError(Error::TIMEOUT, 0);
    }
}

ndk::ScopedAStatus Session::detectInteraction(
        std::shared_ptr<common::ICancellationSignal>* return_val) {
    *return_val = schedule(
            [this](const std::future<void>&) { mCb->onError(Error::UNABLE_TO_PROCESS, 0); });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    schedule([this](const std::future<void>&) {
        Enrollments enrollments = loadEnrollments();
        auto it = enrollments.find(mUserId);
        if (it != enrollments.end()) {
            mCb->onEnrollmentsEnumerated({it->second.enrollmentId});
        } else {
            mCb->onEnrollmentsEnumerated({});
        }
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& enrollmentIds) {
    schedule([this, enrollmentIds](const std::future<void>&) {
        Enrollments enrollments = loadEnrollments();
        auto it = enrollments.find(mUserId);
        if (it == enrollments.end() ||
            std::find(enrollmentIds.begin(), enrollmentIds.end(), it->second.enrollmentId) ==
                    enrollmentIds.end()) {
            mCb->onEnrollmentsRemoved(enrollmentIds);
            return;
        }

        if (!mHw->auth.removeFace(it->second.faceId)) {
            mCb->onError(Error::UNABLE_TO_REMOVE, 0);
            return;
        }
        enrollments.erase(it);
        if (!saveEnrollments(enrollments)) {
            PLOG(ERROR) << "Failed to save enrollments";
            mCb->onError(Error::UNABLE_TO_REMOVE, 0);
            return;
        }
        mCb->onEnrollmentsRemoved(enrollmentIds);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getFeatures() {
    schedule([this](const std::future<void>&) {
        Enrollments enrollments = loadEnrollments();
        auto it = enrollments.find(mUserId);
        if (it == enrollments.end()) {
            mCb->onError(Error::UNABLE_TO_PROCESS, 0);
            return;
        }
        std::vector<Feature> features;
        if (it->second.requireAttention) features.push_back(Feature::REQUIRE_ATTENTION);
        mCb->onFeaturesRetrieved(features);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setFeature(const keymaster::HardwareAuthToken&, Feature feature,
                                       bool enabled) {
    schedule([this, feature, enabled](const std::future<void>&) {
        Enrollments enrollments = loadEnrollments();
        auto it = enrollments.find(mUserId);
        if (it == enrollments.end() || feature != Feature::REQUIRE_ATTENTION) {
            mCb->onError(Error::UNABLE_TO_PROCESS, 0);
            return;
        }
        it->second.requireAttention = enabled;
        if (!saveEnrollments(enrollments)) {
            mCb->onError(Error::UNABLE_TO_PROCESS, 0);
            return;
        }
        mCb->onFeatureSet(feature);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    schedule([this](const std::future<void>&) {
        Enrollments enrollments = loadEnrollments();
        auto it = enrollments.find(mUserId);
        mCb->onAuthenticatorIdRetrieved(it != enrollments.end() ? it->second.authenticatorId : 0);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    schedule([this](const std::future<void>&) {
        Enrollments enrollments = loadEnrollments();
        auto it = enrollments.find(mUserId);
        if (it == enrollments.end()) {
            mCb->onAuthenticatorIdInvalidated(0);
            return;
        }
        it->second.authenticatorId = randomId();
        if (!saveEnrollments(enrollments)) PLOG(ERROR) << "Failed to save enrollments";
        mCb->onAuthenticatorIdInvalidated(it->second.authenticatorId);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::resetLockout(const keymaster::HardwareAuthToken&) {
    schedule([this](const std::future<void>&) {
        sLockouts.erase(mUserId);
        mCb->onLockoutCleared();
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::close() {
    mCb->onSessionClosed();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(
        int64_t operationId, const common::OperationContext&,
        std::shared_ptr<common::ICancellationSignal>* return_val) {
    return authenticate(operationId, return_val);
}

ndk::ScopedAStatus Session::enrollWithContext(
        const keymaster::HardwareAuthToken& hat, EnrollmentType enrollmentType,
        const std::vector<Feature>& features, const std::optional<NativeHandle>& previewSurface,
        const common::OperationContext&, std::shared_ptr<common::ICancellationSignal>* return_val) {
    return enroll(hat, enrollmentType, features, previewSurface, return_val);
}

ndk::ScopedAStatus Session::detectInteractionWithContext(
        const common::OperationContext&, std::shared_ptr<common::ICancellationSignal>* return_val) {
    return detectInteraction(return_val);
}

ndk::ScopedAStatus Session::onContextChanged(const common::OperationContext&) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enrollWithOptions(
        const FaceEnrollOptions& options,
        std::shared_ptr<common::ICancellationSignal>* return_val) {
    *return_val = scheduleEnroll(options.hardwareAuthToken,
                                 options.surfacePreview ? options.surfacePreview->get() : nullptr);
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
