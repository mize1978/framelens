// camera.hpp — 注視点を中心に回るオービットカメラ。
#pragma once
#include "linalg.hpp"
#include <algorithm>
#include <cmath>

namespace fl {

struct OrbitCamera {
    Vec3  target{0, 0, 0};
    float yaw   = 0.9f;    // 方位角（ラジアン）
    float pitch = 0.35f;   // 仰角（ラジアン）
    float dist  = 7000.0f; // 注視点からの距離（mm）

    Vec3 eye() const {
        float cp = std::cos(pitch), sp = std::sin(pitch);
        float cy = std::cos(yaw),   sy = std::sin(yaw);
        return target + Vec3{dist * cp * cy, dist * sp, dist * cp * sy};
    }

    Mat4 view() const { return lookAt(eye(), target, {0, 1, 0}); }

    void orbit(float dYaw, float dPitch) {
        yaw   += dYaw;
        pitch += dPitch;
        const float lim = 1.55f; // ±~89°
        pitch = std::clamp(pitch, -lim, lim);
    }

    void zoom(float factor) {
        dist = std::clamp(dist * factor, 1500.0f, 30000.0f);
    }
};

} // namespace fl
