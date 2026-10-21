/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/biometrics/face/BnFace.h>

#include "Session.h"

namespace aidl::android::hardware::biometrics::face {

class Face : public BnFace {
  public:
    ndk::ScopedAStatus getSensorProps(std::vector<SensorProps>* return_val) override;
    ndk::ScopedAStatus createSession(int32_t sensorId, int32_t userId,
                                     const std::shared_ptr<ISessionCallback>& cb,
                                     std::shared_ptr<ISession>* return_val) override;

  private:
    std::shared_ptr<Hardware> mHw = std::make_shared<Hardware>();
};

}  // namespace aidl::android::hardware::biometrics::face
