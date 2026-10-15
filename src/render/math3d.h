// math3d.h - float vector/matrix math for presentation only.
// Floats are fine here: nothing the renderer computes feeds back into the sim.
//
// Conventions: left-handed, y up. Camera looks down +z, +x is right.
// Matrices are row-major and multiply column vectors: v' = M * v.
#pragma once

#include <cmath>

namespace ss {

struct Vec3 {
    float x, y, z;
};
inline Vec3  operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3  operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3  operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3  Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3  Normalize(Vec3 a) {
    float l = Length(a);
    return l > 1e-8f ? a * (1.0f / l) : Vec3{0, 0, 0};
}
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

struct Vec4 {
    float x, y, z, w;
};

struct Mat4 {
    float m[4][4];

    static Mat4 Identity() {
        Mat4 r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }
    static Mat4 Translate(Vec3 t) {
        Mat4 r    = Identity();
        r.m[0][3] = t.x;
        r.m[1][3] = t.y;
        r.m[2][3] = t.z;
        return r;
    }
    static Mat4 Scale(Vec3 s) {
        Mat4 r    = Identity();
        r.m[0][0] = s.x;
        r.m[1][1] = s.y;
        r.m[2][2] = s.z;
        return r;
    }
    // Yaw: positive turns +z toward +x (matches the sim's heading convention).
    static Mat4 RotateY(float a) {
        Mat4  r = Identity();
        float c = std::cos(a), s = std::sin(a);
        r.m[0][0] = c;
        r.m[0][2] = s;
        r.m[2][0] = -s;
        r.m[2][2] = c;
        return r;
    }
    static Mat4 RotateX(float a) {
        Mat4  r = Identity();
        float c = std::cos(a), s = std::sin(a);
        r.m[1][1] = c;
        r.m[1][2] = -s;
        r.m[2][1] = s;
        r.m[2][2] = c;
        return r;
    }
    static Mat4 RotateZ(float a) {
        Mat4  r = Identity();
        float c = std::cos(a), s = std::sin(a);
        r.m[0][0] = c;
        r.m[0][1] = -s;
        r.m[1][0] = s;
        r.m[1][1] = c;
        return r;
    }
    static Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up) {
        Vec3 f = Normalize(target - eye);
        Vec3 r = Normalize(Cross(up, f));
        Vec3 u = Cross(f, r);
        Mat4 v = Identity();
        v.m[0][0] = r.x; v.m[0][1] = r.y; v.m[0][2] = r.z; v.m[0][3] = -Dot(r, eye);
        v.m[1][0] = u.x; v.m[1][1] = u.y; v.m[1][2] = u.z; v.m[1][3] = -Dot(u, eye);
        v.m[2][0] = f.x; v.m[2][1] = f.y; v.m[2][2] = f.z; v.m[2][3] = -Dot(f, eye);
        return v;
    }
    // Output: x,y scaled by the focal length, z = view depth, w = view depth.
    // The rasterizer clips against z >= near and |x|,|y| <= w.
    static Mat4 Perspective(float fovY, float aspect) {
        float f = 1.0f / std::tan(fovY * 0.5f);
        Mat4  p{};
        p.m[0][0] = f / aspect;
        p.m[1][1] = f;
        p.m[2][2] = 1.0f;
        p.m[3][2] = 1.0f;
        return p;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

inline Vec4 Transform(const Mat4& a, Vec3 v) {
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z + a.m[0][3],
            a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z + a.m[1][3],
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z + a.m[2][3],
            a.m[3][0] * v.x + a.m[3][1] * v.y + a.m[3][2] * v.z + a.m[3][3]};
}

inline Vec3 TransformPoint(const Mat4& a, Vec3 v) {
    Vec4 r = Transform(a, v);
    return {r.x, r.y, r.z};
}

constexpr float kPi = 3.14159265358979f;

}  // namespace ss
