/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

namespace aidl::android::hardware::biometrics::face::st {

enum st_pixel_format { ST_PIX_FMT_NV21 = 3 };

enum st_face_orientation {
    ST_FACE_UP = 1,
    ST_FACE_LEFT = 2,
    ST_FACE_DOWN = 4,
    ST_FACE_RIGHT = 8,
};

struct st_image_t {
    const uint8_t* data;
    int format;
    int width;
    int height;
    int stride;
};
static_assert(sizeof(st_image_t) == 24);

enum st_auth_config {
    ST_AUTH_CLOSE_EYE_STATUS = 1,
};

enum st_auth_status {
    ST_E_HANDLE = -2,
    ST_E_TEMPLATE_DAMAGED = -1001,
    ST_E_DELETE_ID_NOT_EXIST = -1004,
    ST_E_TEMPLATE_UPPER_LIMIT = -1008,
    ST_AUTH_OK = 0,
    ST_AUTH_NO_FACE = 1,
    ST_AUTH_HACKNESS_FAILED = 2,
    ST_AUTH_EYE_CLOSE = 3,
    ST_AUTH_ENROLL_KEEP = 5,
    ST_AUTH_OVER_EXPOSE = 7,
    ST_AUTH_ENROLL_OK = 101,
    ST_AUTH_EYE_BLOCKED = 103,
    ST_AUTH_MOUTH_BLOCKED = 104,
    ST_AUTH_FACE_UP = 113,
    ST_AUTH_FACE_DOWN = 114,
    ST_AUTH_FACE_LEFT = 115,
    ST_AUTH_FACE_RIGHT = 116,
    ST_AUTH_QUALITY_SHARP = 117,
    ST_AUTH_QUALITY_MOUTH = 118,
    ST_AUTH_QUALITY_DISTANCE = 119,
    ST_AUTH_QUALITY_FACE_SMALL = 120,
    ST_AUTH_QUALITY_FACE_BIG = 121,
    ST_AUTH_QUALITY_OCCLUSION = 122,
    ST_AUTH_QUALITY_BRIGHT_LOW = 123,
    ST_AUTH_QUALITY_BRIGHT_HIGH = 124,
    ST_AUTH_QUALITY_FACE_MISS = 125,
    ST_AUTH_UNLOCK_OK = 201,
    ST_AUTH_UNLOCK_FAILED = 202,
    ST_AUTH_TEMPLATE_NOT_EXIST = 203,
    ST_AUTH_TEMPLATE_NO_FACE = 204,
};

}  // namespace aidl::android::hardware::biometrics::face::st
