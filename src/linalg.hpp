// linalg.hpp — 依存ライブラリなしの最小線形代数ユーティリティ。
// OpenGL 準拠の列優先 (column-major) 4x4 行列。glm 等に頼らず自前実装することで
// 3D 数学の基礎理解を示す意図。
#pragma once
#include <cmath>
#include <array>

namespace fl {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s)       { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, const Vec3& a)       { return a * s; }

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3  cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3  normalize(const Vec3& a) {
    float len = length(a);
    return len > 1e-8f ? a * (1.0f / len) : a;
}

// 列優先 4x4 行列。m[col*4 + row]。
struct Mat4 {
    std::array<float, 16> m{};
    static Mat4 identity() {
        Mat4 r;
        r.m = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        return r;
    }
    const float* data() const { return m.data(); }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            r.m[col * 4 + row] = sum;
        }
    return r;
}

inline Mat4 translate(const Vec3& t) {
    Mat4 r = Mat4::identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

inline Mat4 scale(const Vec3& s) {
    Mat4 r = Mat4::identity();
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}

// 右手系・OpenGL 準拠の透視投影。fovy はラジアン。
inline Mat4 perspective(float fovy, float aspect, float znear, float zfar) {
    Mat4 r;
    float f = 1.0f / std::tan(fovy * 0.5f);
    r.m.fill(0.0f);
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}

// 右手系の視点変換行列。
inline Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
    Vec3 f = normalize(center - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r = Mat4::identity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8]  = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9]  = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] =  dot(f, eye);
    return r;
}

// カメラパラメータからスクリーン座標 (NDC) を通るレイ方向を計算する。
inline Vec3 screenRay(const Vec3& eye, const Vec3& target,
                      float fovY, float aspect, float ndcX, float ndcY) {
    Vec3 f = normalize(target - eye);
    Vec3 r = normalize(cross(f, {0.0f, 1.0f, 0.0f}));
    Vec3 u = cross(r, f);
    float th = std::tan(fovY * 0.5f);
    return normalize(f + r * (ndcX * aspect * th) + u * (ndcY * th));
}

// レイ-AABB 交差判定（スラブ法）。ヒットしたtを返す。ミスは -1.0f。
inline float rayAABB(const Vec3& orig, const Vec3& dir,
                     const Vec3& lo, const Vec3& hi) {
    float tmin = -1e9f, tmax = 1e9f;
    const float* o = &orig.x;
    const float* d = &dir.x;
    const float* mn = &lo.x;
    const float* mx = &hi.x;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < mn[i] || o[i] > mx[i]) return -1.0f;
        } else {
            float t1 = (mn[i] - o[i]) / d[i];
            float t2 = (mx[i] - o[i]) / d[i];
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            tmin = tmin > t1 ? tmin : t1;
            tmax = tmax < t2 ? tmax : t2;
        }
    }
    return (tmin <= tmax && tmax >= 0.0f) ? (tmin >= 0.0f ? tmin : 0.0f) : -1.0f;
}

} // namespace fl
