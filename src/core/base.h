// base.h - fundamental types, asserts, deterministic fixed-point math, hashing.
//
// Everything the simulation touches is integer-only, so every peer in a
// networked race computes bit-identical results regardless of compiler, CPU or
// platform. Rollback netcode depends on that completely.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

#define SS_CONCAT_(a, b) a##b
#define SS_CONCAT(a, b) SS_CONCAT_(a, b)
#define SS_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

#define SS_ASSERT(cond, msg)                                           \
    do {                                                               \
        if (!(cond)) ::ss::AssertFail(#cond, msg, __FILE__, __LINE__); \
    } while (0)

namespace ss {

[[noreturn]] void AssertFail(const char* expr, const char* msg, const char* file, int line);

template <class T> constexpr T Min(T a, T b) { return a < b ? a : b; }
template <class T> constexpr T Max(T a, T b) { return a > b ? a : b; }
template <class T> constexpr T Clamp(T v, T lo, T hi) { return Min(Max(v, lo), hi); }

// ---------------------------------------------------------------------------
// 16.16 fixed point
// ---------------------------------------------------------------------------
struct Fx {
    i32 raw;

    static constexpr int kShift = 16;
    static constexpr i32 kOne   = 1 << kShift;

    static constexpr Fx FromRaw(i32 r) { return Fx{r}; }
    static constexpr Fx FromInt(i32 i) { return Fx{i * kOne}; }
    static constexpr Fx Ratio(i32 num, i32 den) { return Fx{(i32)(((i64)num * kOne) / den)}; }

    constexpr i32 ToInt() const { return raw >> kShift; }
    float ToFloat() const { return (float)raw * (1.0f / kOne); }  // presentation only
};

constexpr Fx operator+(Fx a, Fx b) { return Fx{a.raw + b.raw}; }
constexpr Fx operator-(Fx a, Fx b) { return Fx{a.raw - b.raw}; }
constexpr Fx operator-(Fx a) { return Fx{-a.raw}; }
constexpr Fx operator*(Fx a, Fx b) { return Fx{(i32)(((i64)a.raw * b.raw) >> Fx::kShift)}; }
constexpr Fx operator/(Fx a, Fx b) { return Fx{(i32)(((i64)a.raw * Fx::kOne) / b.raw)}; }
constexpr Fx& operator+=(Fx& a, Fx b) { a.raw += b.raw; return a; }
constexpr Fx& operator-=(Fx& a, Fx b) { a.raw -= b.raw; return a; }
constexpr Fx& operator*=(Fx& a, Fx b) { a = a * b; return a; }
constexpr bool operator<(Fx a, Fx b) { return a.raw < b.raw; }
constexpr bool operator>(Fx a, Fx b) { return a.raw > b.raw; }
constexpr bool operator<=(Fx a, Fx b) { return a.raw <= b.raw; }
constexpr bool operator>=(Fx a, Fx b) { return a.raw >= b.raw; }
constexpr bool operator==(Fx a, Fx b) { return a.raw == b.raw; }
constexpr bool operator!=(Fx a, Fx b) { return a.raw != b.raw; }

constexpr Fx FxAbs(Fx a) { return a.raw < 0 ? -a : a; }
constexpr Fx FxLerp(Fx a, Fx b, Fx t) { return a + (b - a) * t; }

// 2D vector on the ground plane: x = east, z = north. y (height) is separate.
struct V2 {
    Fx x, z;
};
constexpr V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.z + b.z}; }
constexpr V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.z - b.z}; }
constexpr V2 operator-(V2 a) { return {-a.x, -a.z}; }
constexpr V2 operator*(V2 a, Fx s) { return {a.x * s, a.z * s}; }
constexpr V2& operator+=(V2& a, V2 b) { a = a + b; return a; }
constexpr V2& operator-=(V2& a, V2 b) { a = a - b; return a; }
constexpr Fx Dot(V2 a, V2 b) { return a.x * b.x + a.z * b.z; }
// > 0 when b is clockwise (to the right) of a, looking down from +y.
constexpr Fx Cross(V2 a, V2 b) { return a.z * b.x - a.x * b.z; }
constexpr V2 RightOf(V2 fwd) { return {fwd.z, -fwd.x}; }

// Reference integer sqrt: bit-by-bit, usable at compile time.
constexpr u64 ISqrt64Bitwise(u64 n) {
    u64 res = 0, bit = 1ull << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

// floor(sqrt(n)), exact. The FPU sqrt is only an estimate; the integer fix-up
// makes the result bit-identical on every platform. Valid for n < 2^62.
constexpr u64 ISqrt64(u64 n) {
    if (std::is_constant_evaluated()) return ISqrt64Bitwise(n);
    u64 r = (u64)std::sqrt((double)n);
    while (r * r > n) --r;
    while ((r + 1) * (r + 1) <= n) ++r;
    return r;
}

constexpr u64 LengthSq64(V2 v) { return (u64)((i64)v.x.raw * v.x.raw) + (u64)((i64)v.z.raw * v.z.raw); }
constexpr u64 RadiusSq64(Fx r) { return (u64)((i64)r.raw * r.raw); }
constexpr Fx  Length(V2 v) { return Fx{(i32)ISqrt64(LengthSq64(v))}; }
constexpr V2  Normalize(V2 v) {
    Fx len = Length(v);
    if (len.raw == 0) return {Fx{0}, Fx{0}};
    return {v.x / len, v.z / len};
}

// ---------------------------------------------------------------------------
// Binary angles: 65536 units per turn, so wrap-around is free. Sine comes from
// a baked integer table (quarter wave, 256 steps, Q16) with linear
// interpolation: no libm, no platform-dependent rounding.
// ---------------------------------------------------------------------------
using Angle = i32;
constexpr Angle kAngleTurn    = 65536;
constexpr Angle kAngleQuarter = 16384;

inline constexpr i32 kSinQuarter[257] = {
    0, 402, 804, 1206, 1608, 2010, 2412, 2814, 3216, 3617, 4019, 4420,
    4821, 5222, 5623, 6023, 6424, 6824, 7224, 7623, 8022, 8421, 8820, 9218,
    9616, 10014, 10411, 10808, 11204, 11600, 11996, 12391, 12785, 13180, 13573, 13966,
    14359, 14751, 15143, 15534, 15924, 16314, 16703, 17091, 17479, 17867, 18253, 18639,
    19024, 19409, 19792, 20175, 20557, 20939, 21320, 21699, 22078, 22457, 22834, 23210,
    23586, 23961, 24335, 24708, 25080, 25451, 25821, 26190, 26558, 26925, 27291, 27656,
    28020, 28383, 28745, 29106, 29466, 29824, 30182, 30538, 30893, 31248, 31600, 31952,
    32303, 32652, 33000, 33347, 33692, 34037, 34380, 34721, 35062, 35401, 35738, 36075,
    36410, 36744, 37076, 37407, 37736, 38064, 38391, 38716, 39040, 39362, 39683, 40002,
    40320, 40636, 40951, 41264, 41576, 41886, 42194, 42501, 42806, 43110, 43412, 43713,
    44011, 44308, 44604, 44898, 45190, 45480, 45769, 46056, 46341, 46624, 46906, 47186,
    47464, 47741, 48015, 48288, 48559, 48828, 49095, 49361, 49624, 49886, 50146, 50404,
    50660, 50914, 51166, 51417, 51665, 51911, 52156, 52398, 52639, 52878, 53114, 53349,
    53581, 53812, 54040, 54267, 54491, 54714, 54934, 55152, 55368, 55582, 55794, 56004,
    56212, 56418, 56621, 56823, 57022, 57219, 57414, 57607, 57798, 57986, 58172, 58356,
    58538, 58718, 58896, 59071, 59244, 59415, 59583, 59750, 59914, 60075, 60235, 60392,
    60547, 60700, 60851, 60999, 61145, 61288, 61429, 61568, 61705, 61839, 61971, 62101,
    62228, 62353, 62476, 62596, 62714, 62830, 62943, 63054, 63162, 63268, 63372, 63473,
    63572, 63668, 63763, 63854, 63944, 64031, 64115, 64197, 64277, 64354, 64429, 64501,
    64571, 64639, 64704, 64766, 64827, 64884, 64940, 64993, 65043, 65091, 65137, 65180,
    65220, 65259, 65294, 65328, 65358, 65387, 65413, 65436, 65457, 65476, 65492, 65505,
    65516, 65525, 65531, 65535, 65536,
};

constexpr i32 SinStep(i32 idx) {  // idx: 1024 steps per turn
    idx &= 1023;
    i32 i = idx & 255;
    switch (idx >> 8) {
        case 0:  return kSinQuarter[i];
        case 1:  return kSinQuarter[256 - i];
        case 2:  return -kSinQuarter[i];
        default: return -kSinQuarter[256 - i];
    }
}
constexpr Fx Sin(Angle a) {
    i32 idx = (a >> 6) & 1023, frac = a & 63;
    i32 s0 = SinStep(idx), s1 = SinStep(idx + 1);
    return Fx{s0 + (((s1 - s0) * frac) >> 6)};
}
constexpr Fx Cos(Angle a) { return Sin(a + kAngleQuarter); }
// Unit vector for a heading; heading 0 faces +z, increasing turns toward +x.
constexpr V2 Forward(Angle a) { return {Sin(a), Cos(a)}; }
constexpr Angle WrapAngle(Angle a) { return a & (kAngleTurn - 1); }
// Smallest signed difference b - a, in [-32768, 32767].
constexpr Angle AngleDelta(Angle a, Angle b) { return (Angle)(i16)(u16)(b - a); }

// ---------------------------------------------------------------------------
// Hashing. Sim structs are padding-free (static_asserted with
// has_unique_object_representations), so hashing their bytes is well defined.
// ---------------------------------------------------------------------------
inline u64 HashBytes(const void* data, size_t size, u64 h = 0xcbf29ce484222325ull) {
    const u8* p = (const u8*)data;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

inline u32 HashU32(u32 x) {  // stateless integer hash (deterministic "randomness")
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

struct Rng {
    u32 s;
    u32 Next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    i32 Range(i32 lo, i32 hi) { return lo + (i32)(Next() % (u32)(hi - lo + 1)); }
};

}  // namespace ss
