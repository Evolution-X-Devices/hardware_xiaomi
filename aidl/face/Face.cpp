/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Face.h"

namespace aidl::android::hardware::biometrics::face {

namespace {

constexpr int kSensorId = 1;
constexpr int kMaxEnrollmentsPerUser = 1;

}  // namespace

ndk::ScopedAStatus Face::getSensorProps(std::vector<SensorProps>* return_val) {
    SensorProps props;
    props.commonProps.sensorId = kSensorId;
    props.commonProps.sensorStrength = common::SensorStrength::WEAK;
    props.commonProps.maxEnrollmentsPerUser = kMaxEnrollmentsPerUser;
    props.sensorType = FaceSensorType::RGB;
    props.halControlsPreview = true;
    props.supportsDetectInteraction = false;

    *return_val = {std::move(props)};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Face::createSession(int32_t /*sensorId*/, int32_t userId,
                                       const std::shared_ptr<ISessionCallback>& cb,
                                       std::shared_ptr<ISession>* return_val) {
    *return_val = SharedRefBase::make<Session>(mHw, userId, cb);
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
