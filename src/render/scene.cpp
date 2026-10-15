#include "render/scene.h"
#include "core/memory.h"
#include "game/characters.h"
#include "game/sim.h"
#include "game/tracks.h"
#include "render/modeling.h"

#include <cstdlib>
#include <initializer_list>

namespace ss {

namespace {

constexpr float kKartLodDist = 30.0f;
constexpr float kFrontWheelR = 0.21f;
constexpr float kRearWheelR  = 0.25f;
const Vec3      kWheelPos[4] = {{-0.66f, kFrontWheelR, 0.80f}, {0.66f, kFrontWheelR, 0.80f},
                                {-0.66f, kRearWheelR, -0.72f}, {0.66f, kRearWheelR, -0.72f}};

// ---------------------------------------------------------------------------
// Procedural textures
// ---------------------------------------------------------------------------
u32 Noise(int x, int y, u32 seed) { return HashU32((u32)x * 73856093u ^ (u32)y * 19349663u ^ seed) & 0xFF; }

Texture MakeTexture(Arena& arena, int sizeLog2, u32 (*fn)(int x, int y, int size)) {
    int  size = 1 << sizeLog2;
    u32* px   = arena.Push<u32>((size_t)size * size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) px[y * size + x] = fn(x, y, size);
    return {px, sizeLog2};
}

u32 AsphaltTexel(int x, int y, int size) {
    int n = 78 + (int)(Noise(x, y, 1) % 22) - 11;
    if ((x == size / 2 - 1 || x == size / 2) && y < size / 2) return Rgb(235, 235, 225);  // dashed centre line
    if (abs(x - size / 4) < 3 || abs(x - 3 * size / 4) < 3) n -= 8;                         // tyre-worn lanes
    return Rgb(n, n, n + 4);
}

u32 GrassTexel(int x, int y, int) {
    int n      = (int)(Noise(x, y, 2) % 40);
    int stripe = ((y / 8) & 1) ? 10 : 0;  // mowing stripes
    return Rgb(52 + n / 3 + stripe, 128 + n + stripe, 44 + n / 4);
}

u32 SandTexel(int x, int y, int) {
    int n      = (int)(Noise(x, y, 5) % 24) - 12;
    int ripple = (((y + x / 3) / 5) & 1) ? -10 : 0;  // wind ripples
    return Rgb(Clamp(222 + n + ripple, 0, 255), Clamp(188 + n + ripple, 0, 255), Clamp(136 + n / 2 + ripple, 0, 255));
}

u32 SnowTexel(int x, int y, int) {
    int n = (int)(Noise(x, y, 6) % 18) - 9;
    if (Noise(x, y, 7) > 250) return Rgb(255, 255, 255);  // sparkle
    return Rgb(226 + n, 234 + n, 244 + n / 2);
}

u32 NeonGridTexel(int x, int y, int) {
    if (x % 16 == 0 || y % 16 == 0) return ((x / 16 + y / 16) & 1) ? Rgb(40, 230, 255) : Rgb(255, 70, 200);
    int n = (int)(Noise(x, y, 8) % 10);
    return Rgb(30 + n, 8, 48 + n);
}

u32 WallTexel(int x, int y, int size) {
    if (y < size / 6) return ((x / 8) & 1) ? Rgb(220, 50, 40) : Rgb(240, 240, 240);
    int band = (y - size / 6) % 12;
    int n    = 30 + (int)(Noise(x, y, 3) % 14) + (band < 2 ? -12 : (band > 8 ? -6 : 8));
    return Rgb(n, n, n + 2);
}

u32 CheckerTexel(int x, int y, int) { return (((x / 8) + (y / 8)) & 1) ? Rgb(20, 20, 20) : Rgb(240, 240, 240); }

u32 ItemBoxTexel(int x, int y, int size) {
    static const char* kQ[7] = {"01110", "10001", "00001", "00110", "00100", "00000", "00100"};
    if (x < 3 || y < 3 || x >= size - 3 || y >= size - 3) return Rgb(255, 255, 255);
    int gx = (x - 12) / 8, gy = (y - 4) / 8;
    if (gx >= 0 && gx < 5 && gy >= 0 && gy < 7 && x >= 12 && y >= 4 && kQ[gy][gx] == '1') return Rgb(255, 255, 255);
    int g = 150 + (x + y) % 32;
    return Rgb(g, g, 255);
}

u32 Hsv(float h, float s, float v) {
    h        = h - std::floor(h);
    float r  = std::fabs(h * 6 - 3) - 1, g = 2 - std::fabs(h * 6 - 2), b = 2 - std::fabs(h * 6 - 4);
    auto  ch = [&](float c) { return (int)((1 - s + s * Clamp(c, 0.0f, 1.0f)) * v * 255.0f); };
    return Rgb(ch(r), ch(g), ch(b));
}

// ---------------------------------------------------------------------------
// Themes
// ---------------------------------------------------------------------------
struct ScatterEntry {
    PropKind kind;
    float    weight, minScale, maxScale;
    u32      tint;
};

struct ThemeDef {
    SceneTheme   look;
    u32 (*ground)(int, int, int);
    u32          embankment;
    float        lightLevel;
    ScatterEntry scatter[6];
    int          scatterCount;
};

const ThemeDef kThemes[Theme_Count] = {
    // Meadow
    {{0x2F6FD0, 0xBFE0F2, 0x5F7E96, 0x88A6BC, 0xBFE0F2, 70, 230, false}, GrassTexel, 0x7A5C3E, 1.0f,
     {{Prop_Pine0, 0.2f, 0.85f, 1.5f, 0xFFFFFF}, {Prop_Pine1, 0.2f, 0.85f, 1.5f, 0xFFFFFF},
      {Prop_Pine2, 0.15f, 0.85f, 1.5f, 0xFFFFFF}, {Prop_Round0, 0.14f, 0.85f, 1.4f, 0xFFFFFF},
      {Prop_Round1, 0.13f, 0.85f, 1.4f, 0xFFFFFF}, {Prop_Rock0, 0.18f, 0.6f, 1.8f, 0xFFFFFF}},
     6},
    // Desert
    {{0x4A86C8, 0xF2D7A6, 0xB8754A, 0xD9A06B, 0xEBCB98, 80, 240, false}, SandTexel, 0xB07848, 1.05f,
     {{Prop_Cactus0, 0.3f, 0.8f, 1.4f, 0xFFFFFF}, {Prop_Cactus1, 0.25f, 0.8f, 1.4f, 0xFFFFFF},
      {Prop_Rock0, 0.15f, 0.6f, 2.2f, 0xE0A070}, {Prop_Rock1, 0.15f, 0.6f, 2.2f, 0xD89060},
      {Prop_Mesa, 0.15f, 0.7f, 1.6f, 0xFFFFFF}},
     5},
    // Snow
    {{0x7FA4CC, 0xE4EEF6, 0xA8B9CC, 0xC9D6E3, 0xDDE8F0, 55, 210, false}, SnowTexel, 0x9AA8B8, 1.05f,
     {{Prop_SnowPine0, 0.35f, 0.85f, 1.6f, 0xFFFFFF}, {Prop_SnowPine1, 0.3f, 0.85f, 1.6f, 0xFFFFFF},
      {Prop_Rock1, 0.15f, 0.6f, 1.8f, 0xE6EEF6}, {Prop_Snowman, 0.2f, 0.9f, 1.3f, 0xFFFFFF}},
     4},
    // Vaporwave
    {{0x1A0533, 0xFF5FA2, 0x2D0B4F, 0x6A1B9A, 0x3A1458, 60, 210, true}, NeonGridTexel, 0x2A0E40, 0.8f,
     {{Prop_Palm0, 0.35f, 0.9f, 1.4f, 0xFFFFFF}, {Prop_Palm1, 0.3f, 0.9f, 1.4f, 0xFFFFFF},
      {Prop_Column, 0.35f, 0.8f, 1.5f, 0xFFFFFF}},
     3},
};

// ---------------------------------------------------------------------------
// Modelling helpers
// ---------------------------------------------------------------------------
Vec3 ToVec3(V2 p, Fx y) { return {p.x.ToFloat(), y.ToFloat(), p.z.ToFloat()}; }
Vec3 Flat(V2 d) { return {d.x.ToFloat(), 0, d.z.ToFloat()}; }

// Quad wound so it faces `facing` (local space), whatever order the corners
// were listed in.
void QuadFacing(MeshBuilder& b, Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, u32 color, Vec3 facing) {
    if (Dot(Cross(p1 - p0, p3 - p0), facing) < 0) {
        Vec3 t = p1;
        p1     = p3;
        p3     = t;
    }
    b.Quad(p0, p1, p2, p3, color);
}

// Rotation taking +Y onto `dir`.
Mat4 AlignY(Vec3 dir) {
    Vec3 y = Normalize(dir);
    Vec3 x = Normalize(Cross(y, std::fabs(y.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0}));
    Vec3 z = Cross(x, y);
    Mat4 m = Mat4::Identity();
    m.m[0][0] = x.x; m.m[1][0] = x.y; m.m[2][0] = x.z;
    m.m[0][1] = y.x; m.m[1][1] = y.y; m.m[2][1] = y.z;
    m.m[0][2] = z.x; m.m[1][2] = z.y; m.m[2][2] = z.z;
    return m;
}

void Sphere(MeshBuilder& b, Vec3 c, float r, u32 color, int segments = 10, float squashY = 1.0f, float jitter = 0.0f,
            u32 seed = 0) {
    ProfilePoint p[7];
    for (int i = 0; i < 7; ++i) {
        float a = -kPi * 0.5f + kPi * (float)i / 6.0f;
        p[i]    = {r * std::cos(a), r * std::sin(a) * squashY, color};
    }
    p[0].r = p[6].r = 0;
    Mat4 keep = b.xf;
    b.xf      = keep * Mat4::Translate(c);
    b.Lathe(p, 7, segments, jitter, seed);
    b.xf = keep;
}

void ConeAlong(MeshBuilder& b, Vec3 base, Vec3 dir, float radius, float length, u32 color, int segments = 8) {
    const ProfilePoint p[] = {{0, 0, color}, {radius, 0, color}, {0, length, color}};
    Mat4 keep              = b.xf;
    b.xf                   = keep * Mat4::Translate(base) * AlignY(dir);
    b.Lathe(p, 3, segments);
    b.xf = keep;
}

void Tube(MeshBuilder& b, std::initializer_list<Vec3> pts, float r0, float r1, u32 color, int sides = 6, bool caps = false) {
    Vec3  path[16];
    float radii[16];
    u32   cols[16];
    int   n = 0;
    for (Vec3 p : pts) path[n++] = p;
    for (int i = 0; i < n; ++i) {
        radii[i] = r0 + (r1 - r0) * (float)i / (float)Max(n - 1, 1);
        cols[i]  = color;
    }
    b.Sweep(path, radii, cols, n, sides, caps);
}

// The seated driver's shared pieces: torso and arms reaching for the wheel.
void Torso(MeshBuilder& b, u32 color, float width = 1.0f) {
    const ProfilePoint p[] = {{0.0f, 0.0f, color},          {0.20f * width, 0.02f, color}, {0.24f * width, 0.20f, color},
                              {0.22f * width, 0.42f, color}, {0.12f, 0.50f, color},         {0.0f, 0.52f, color}};
    Mat4 keep = b.xf;
    b.xf      = keep * Mat4::Translate({0, 0.34f, -0.40f});
    b.Lathe(p, 6, 10);
    b.xf = keep;
}

void Arms(MeshBuilder& b, u32 sleeve, u32 hand, float radius = 0.06f) {
    for (float s : {-1.0f, 1.0f}) {
        Tube(b, {{0.20f * s, 0.80f, -0.38f}, {0.28f * s, 0.62f, -0.12f}, {0.13f * s, 0.66f, 0.12f}}, radius, radius * 0.85f,
             sleeve);
        Sphere(b, {0.13f * s, 0.66f, 0.13f}, radius * 0.95f, hand, 6);
    }
}

void Eyes(MeshBuilder& b, float y, float z, float spread, float r, u32 color = 0x101010) {
    Sphere(b, {-spread, y, z}, r, color, 6);
    Sphere(b, {spread, y, z}, r, color, 6);
}

// ---------------------------------------------------------------------------
// Track
// ---------------------------------------------------------------------------
void BuildTrackChunks(Scene& sc, const Track& t, const ThemeDef& theme, Arena& dest, Arena& work) {
    constexpr int kSegsPerChunk = 10;
    const float   road = t.roadHalf.ToFloat(), wall = t.wallHalf.ToFloat(), curb = road + 1.0f;
    const float   wallH = 1.0f, groundY = -0.6f;
    float         dist = 0;

    sc.chunkCount = (t.count + kSegsPerChunk - 1) / kSegsPerChunk;
    SS_ASSERT(sc.chunkCount <= kMaxChunks, "too many track chunks");
    for (int c = 0; c < sc.chunkCount; ++c) {
        ScratchScope scope(work);
        MeshBuilder  road_, curb_, verge_, wall_;
        road_.Init(work, 512, 1024);
        curb_.Init(work, 512, 1024);
        verge_.Init(work, 512, 1024);
        wall_.Init(work, 512, 1024);
        Vec3 sum{0, 0, 0};
        int  n = 0;

        for (int i = c * kSegsPerChunk; i < Min((c + 1) * kSegsPerChunk, t.count); ++i) {
            const TrackSample& a  = t.s[i];
            const TrackSample& b  = t.s[(i + 1) % t.count];
            Vec3               pa = ToVec3(a.pos, a.y), pb = ToVec3(b.pos, b.y);
            Vec3               ra = Flat(a.right), rb = Flat(b.right);
            float              len = a.length.ToFloat();
            float              v0 = dist / 14.0f, v1 = (dist + len) / 14.0f;

            road_.Quad(pa - ra * road, pb - rb * road, pb + rb * road, pa + ra * road, 0xFFFFFF, 0, v0, 1, v1);

            u32  curbColor = (i & 1) ? 0xD83A30 : 0xF0F0F0;
            Vec3 up{0, 0.02f, 0};
            curb_.Quad(pa - ra * curb + up, pb - rb * curb + up, pb - rb * road + up, pa - ra * road + up, curbColor);
            curb_.Quad(pa + ra * road + up, pb + rb * road + up, pb + rb * curb + up, pa + ra * curb + up, curbColor);

            float g0 = dist / 4.0f, g1 = (dist + len) / 4.0f;
            verge_.Quad(pa - ra * wall, pb - rb * wall, pb - rb * curb, pa - ra * curb, 0xFFFFFF, 0, g0, 0.75f, g1);
            verge_.Quad(pa + ra * curb, pb + rb * curb, pb + rb * wall, pa + ra * wall, 0xFFFFFF, 0, g0, 0.75f, g1);

            Vec3  h{0, wallH, 0};
            Vec3  la = pa - ra * wall, lb = pb - rb * wall, rA = pa + ra * wall, rB = pb + rb * wall;
            float w0 = dist / 3.0f, w1 = (dist + len) / 3.0f;
            wall_.Quad(la, la + h, lb + h, lb, 0xFFFFFF, w0, 1, w1, 0);
            wall_.Quad(rA + h, rA, rB, rB + h, 0xFFFFFF, w0, 0, w1, 1);
            auto ground = [&](Vec3 p) { return Vec3{p.x, groundY, p.z}; };  // embankment outside
            curb_.Quad(ground(la), ground(lb), lb + h, la + h, theme.embankment);
            curb_.Quad(ground(rA), rA + h, rB + h, ground(rB), theme.embankment);

            sum = sum + pa;
            ++n;
            dist += len;
        }

        TrackChunk& ch = sc.chunks[c];
        ch.road        = road_.Finish(dest, &sc.texAsphalt);
        ch.curb        = curb_.Finish(dest, nullptr);
        ch.verge       = verge_.Finish(dest, &sc.texGround);
        ch.wall        = wall_.Finish(dest, &sc.texWall);
        ch.center      = sum * (1.0f / (float)n);
        ch.radius      = 0;
        for (int i = c * kSegsPerChunk; i < Min((c + 1) * kSegsPerChunk + 1, t.count + 1); ++i) {
            const TrackSample& s = t.s[i % t.count];
            ch.radius            = Max(ch.radius, Length(ToVec3(s.pos, s.y) - ch.center) + wall + 2.0f);
        }
    }

    ScratchScope       scope(work);
    MeshBuilder        line;
    line.Init(work, 8, 12);
    const TrackSample& s0 = t.s[0];
    Vec3 p = ToVec3(s0.pos, s0.y) + Vec3{0, 0.03f, 0}, r = Flat(s0.right), f = Flat(s0.tangent) * 0.9f;
    line.Quad(p - r * road - f, p - r * road + f, p + r * road + f, p + r * road - f, 0xFFFFFF, 0, 0, 7, 1);
    sc.startLine = line.Finish(dest, &sc.texChecker);
}

void BuildGround(Scene& sc, Arena& dest, Arena& work) {
    constexpr int   kTiles = 8;
    constexpr float kHalf = 700.0f, kTile = 2 * kHalf / kTiles, kY = -0.6f;
    sc.groundCount  = 0;
    sc.groundRadius = kTile * 0.75f;
    for (int ty = 0; ty < kTiles; ++ty)
        for (int tx = 0; tx < kTiles; ++tx) {
            ScratchScope scope(work);
            float        x0 = -kHalf + tx * kTile, z0 = -kHalf + ty * kTile, x1 = x0 + kTile, z1 = z0 + kTile;
            MeshBuilder  b;
            b.Init(work, 8, 12);
            b.Quad({x0, kY, z0}, {x0, kY, z1}, {x1, kY, z1}, {x1, kY, z0}, 0xE0E0E0, x0 / 6, z0 / 6, x1 / 6, z1 / 6);
            sc.groundCenters[sc.groundCount] = {(x0 + x1) / 2, kY, (z0 + z1) / 2};
            sc.groundTiles[sc.groundCount++] = b.Finish(dest, &sc.texGround);
        }
}

// ---------------------------------------------------------------------------
// Karts
// ---------------------------------------------------------------------------
void BuildKart(Scene& sc, Arena& dest, Arena& work) {
    // Chassis, authored near-white so the character's paint multiplies through.
    {
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        const Vec2 tub[] = {{-1.0f, 0.16f}, {0.98f, 0.16f}, {1.25f, 0.26f}, {1.18f, 0.34f}, {0.62f, 0.40f},
                            {0.32f, 0.50f},  {0.14f, 0.50f}, {0.06f, 0.34f}, {-0.62f, 0.34f}, {-0.70f, 0.58f},
                            {-1.02f, 0.56f}};
        b.Extrude(tub, (int)SS_COUNTOF(tub), -0.46f, 0.46f, 0xFFFFFF);  // concave: cockpit cut-out
        const Vec2 pod[] = {{-0.75f, 0.18f}, {0.55f, 0.18f}, {0.40f, 0.38f}, {-0.75f, 0.42f}};
        b.Extrude(pod, 4, 0.46f, 0.68f, 0xE6E6E6);
        b.Extrude(pod, 4, -0.68f, -0.46f, 0xE6E6E6);
        const Vec2 frontWing[] = {{1.0f, 0.10f}, {1.42f, 0.10f}, {1.42f, 0.15f}, {1.1f, 0.19f}};
        b.Extrude(frontWing, 4, -0.74f, 0.74f, 0xF4F4F4);
        const Vec2 rearWing[] = {{-1.32f, 0.86f}, {-0.95f, 0.88f}, {-0.98f, 0.95f}, {-1.32f, 0.94f}};
        b.Extrude(rearWing, 4, -0.72f, 0.72f, 0xF4F4F4);
        b.Box({-0.74f, 0.70f, -1.34f}, {-0.70f, 1.00f, -0.94f}, 0xD8D8D8);
        b.Box({0.70f, 0.70f, -1.34f}, {0.74f, 1.00f, -0.94f}, 0xD8D8D8);
        sc.kartBody = b.Finish(dest, nullptr);
    }
    {
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        b.xf = Mat4::Translate({0, 0.66f, 0.15f}) * Mat4::RotateX(-1.05f);  // steering wheel, tilted at the driver
        b.Torus(0.14f, 0.022f, 12, 5, 0x262626);
        b.xf = Mat4::Identity();
        Tube(b, {{0, 0.40f, 0.42f}, {0, 0.64f, 0.18f}}, 0.025f, 0.025f, 0x3A3A3A, 5);
        for (float side : {-1.0f, 1.0f}) {
            const Vec3  pipe[] = {{0.26f * side, 0.40f, -0.85f}, {0.27f * side, 0.44f, -1.18f}};
            const float r[]    = {0.06f, 0.07f};
            const u32   c[]    = {0xB8BCC0, 0xD0D4D8};
            b.Sweep(pipe, r, c, 2, 8, true);
        }
        for (float z : {0.80f, -0.72f}) {
            float y = z > 0 ? kFrontWheelR : kRearWheelR;
            Tube(b, {{-0.62f, y, z}, {0.62f, y, z}}, 0.03f, 0.03f, 0x303030, 5);
        }
        b.Box({-0.52f, 0.58f, -0.97f}, {-0.46f, 0.88f, -0.92f}, 0x404040);
        b.Box({0.46f, 0.58f, -0.97f}, {0.52f, 0.88f, -0.92f}, 0x404040);
        b.Box({-0.26f, 0.34f, -0.66f}, {0.26f, 0.70f, -0.56f}, 0x2C2C2C);  // seat back
        sc.kartParts = b.Finish(dest, nullptr);
    }
    // Wheels: lathed with a dished rim and five spokes so the spin shows.
    for (int front = 0; front < 2; ++front) {
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        float     R = front ? kFrontWheelR : kRearWheelR, w = front ? 0.11f : 0.13f;
        const u32 tyre = 0x1C1C1C, wall = 0x2E2E2E, rim = 0xC4C8CC, hub = 0x8A8E92;
        const ProfilePoint prof[] = {
            {0.0f, -w, 0x303030},        {0.6f * R, -w, 0x303030},    {0.6f * R, -w, tyre},        {0.86f * R, -w - 0.01f, wall},
            {R, -w * 0.6f, tyre},        {R, w * 0.6f, tyre},         {0.86f * R, w + 0.01f, wall}, {0.62f * R, w, tyre},
            {0.62f * R, w, rim},         {0.55f * R, w - 0.04f, rim}, {0.18f * R, w - 0.04f, rim}, {0.18f * R, w - 0.04f, hub},
            {0.15f * R, w - 0.01f, hub}, {0.0f, w, hub},
        };
        Mat4 axle = Mat4::RotateZ(kPi * 0.5f);
        b.xf      = axle;
        b.Lathe(prof, (int)SS_COUNTOF(prof), 14);
        for (int k = 0; k < 5; ++k) {
            b.xf = axle * Mat4::RotateY((float)k * 2 * kPi / 5);
            b.Box({-0.018f, w - 0.045f, 0.16f * R}, {0.018f, w - 0.02f, 0.56f * R}, rim);
        }
        (front ? sc.wheelFront : sc.wheelRear) = b.Finish(dest, nullptr);
    }
    // Low-detail stand-in for distant karts.
    {
        ScratchScope scope(work);
        MeshBuilder  t;
        t.Init(work);
        t.Box({-0.6f, 0.15f, -0.9f}, {0.6f, 0.45f, 0.9f}, 0xFFFFFF);
        t.Box({-0.45f, 0.2f, 0.9f}, {0.45f, 0.38f, 1.2f}, 0xE8E8E8);
        t.Box({-0.68f, 0.72f, -1.08f}, {0.68f, 0.8f, -0.82f}, 0xD0D0D0);
        sc.kartLowTinted = t.Finish(dest, nullptr);
        MeshBuilder f;
        f.Init(work);
        const float wx[2] = {-0.86f, 0.56f}, wz[2] = {-0.85f, 0.45f};
        for (float x : wx)
            for (float z : wz) f.Box({x, 0.0f, z}, {x + 0.3f, 0.42f, z + 0.42f}, 0x262626);
        f.Box({-0.3f, 0.45f, -0.55f}, {0.3f, 1.2f, -0.05f}, 0x9A9AA0);
        sc.kartLowFixed = f.Finish(dest, nullptr);
    }
}

// The roster. Each driver sits in the cockpit: seat base y 0.34 at z -0.40,
// head around y 1.0, steering wheel at (0, 0.66, 0.15).
void BuildCharacters(Scene& sc, Arena& dest, Arena& work) {
    for (int c = 0; c < kNumCharacters; ++c) {
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        switch (c) {
            case Char_Doggo: {  // shiba: tan with a cream muzzle and pointy ears
                const u32 fur = 0xD9924A, cream = 0xF3E3C8;
                Torso(b, fur);
                Arms(b, fur, cream);
                Sphere(b, {0, 1.03f, -0.36f}, 0.24f, fur, 12);
                Sphere(b, {0, 0.97f, -0.17f}, 0.12f, cream, 10, 0.8f);
                Sphere(b, {0, 1.00f, -0.06f}, 0.035f, 0x151515, 6);
                Eyes(b, 1.09f, -0.16f, 0.09f, 0.03f);
                for (float s : {-1.0f, 1.0f}) ConeAlong(b, {0.13f * s, 1.18f, -0.38f}, {0.25f * s, 1, 0}, 0.08f, 0.17f, fur, 5);
                Sphere(b, {0, 0.62f, -0.18f}, 0.12f, cream, 8, 0.9f);  // chest
                break;
            }
            case Char_Stonks: {  // suit, smooth bald head, and the line that only goes up
                const u32 suit = 0x2A3550, skin = 0xF1C7A5;
                Torso(b, suit);
                Arms(b, suit, skin);
                b.Box({-0.07f, 0.60f, -0.21f}, {0.07f, 0.84f, -0.17f}, 0xF2F2F2);
                b.Box({-0.025f, 0.62f, -0.225f}, {0.025f, 0.82f, -0.2f}, 0xC0282A);
                Sphere(b, {0, 1.05f, -0.36f}, 0.21f, skin, 12, 1.15f);
                Eyes(b, 1.08f, -0.17f, 0.075f, 0.025f);
                b.Box({-0.06f, 0.96f, -0.165f}, {0.06f, 0.975f, -0.15f}, 0x7A4A3A);  // smirk
                Tube(b, {{0.3f, 0.45f, -0.75f}, {0.3f, 1.55f, -0.75f}}, 0.03f, 0.03f, 0x8A9098, 5);
                Tube(b, {{-0.15f, 1.25f, -0.75f}, {0.0f, 1.42f, -0.75f}, {0.1f, 1.34f, -0.75f}, {0.32f, 1.66f, -0.75f}},
                     0.04f, 0.04f, 0x2FD35A, 6);
                ConeAlong(b, {0.32f, 1.66f, -0.75f}, {0.22f, 0.32f, 0}, 0.09f, 0.16f, 0x2FD35A, 6);
                break;
            }
            case Char_Moai: {  // a head. That is all it is.
                const u32 stone = 0x9A9488, dark = 0x6E695F;
                Torso(b, 0x8A8478, 1.1f);
                b.Box({-0.21f, 0.84f, -0.56f}, {0.21f, 1.52f, -0.22f}, stone);
                b.Box({-0.23f, 1.24f, -0.25f}, {0.23f, 1.33f, -0.17f}, dark);  // brow
                b.Box({-0.065f, 1.0f, -0.22f}, {0.065f, 1.27f, -0.11f}, stone);  // nose
                b.Box({-0.16f, 0.86f, -0.24f}, {0.16f, 0.98f, -0.16f}, dark);  // pout
                for (float s : {-1.0f, 1.0f}) {
                    b.Box({0.06f * s - 0.06f, 1.14f, -0.225f}, {0.06f * s + 0.06f, 1.23f, -0.215f}, 0x3A3630);  // eye shadow
                    b.Box({0.21f * s - 0.03f, 0.95f, -0.45f}, {0.21f * s + 0.03f, 1.35f, -0.32f}, dark);  // long ears
                }
                break;
            }
            case Char_GalaxyBrain: {  // a modest face under an enormous, glowing brain
                const u32 robe = 0x5B3A8C, skin = 0xE8C8B0;
                Torso(b, robe);
                Arms(b, robe, skin);
                Sphere(b, {0, 0.96f, -0.36f}, 0.17f, skin, 10);
                Eyes(b, 0.99f, -0.21f, 0.06f, 0.025f, 0x2A1A40);
                b.lit = false;  // the brain glows
                Sphere(b, {0, 1.24f, -0.40f}, 0.32f, 0xFF8AE2, 12, 0.85f, 0.18f, 77);
                Sphere(b, {0, 1.27f, -0.40f}, 0.27f, 0xB86BFF, 10, 0.85f, 0.2f, 78);
                for (int k = 0; k < 6; ++k) {
                    float a = (float)k * 1.05f;
                    b.Box({std::sin(a) * 0.5f - 0.025f, 1.25f + 0.2f * std::cos(a * 2.0f), -0.40f + std::cos(a) * 0.5f - 0.025f},
                          {std::sin(a) * 0.5f + 0.025f, 1.30f + 0.2f * std::cos(a * 2.0f), -0.40f + std::cos(a) * 0.5f + 0.025f},
                          k & 1 ? 0x7FF7FF : 0xFFFFFF);
                }
                b.lit = true;
                break;
            }
            case Char_VibeCat: {  // eyes closed, headphones on, vibing
                const u32 fur = 0xE6E6EE, pink = 0xF4A0B8;
                Torso(b, 0xCFCFD8);
                Arms(b, 0xCFCFD8, 0xFFFFFF);
                Sphere(b, {0, 1.04f, -0.36f}, 0.23f, fur, 12);
                for (float s : {-1.0f, 1.0f}) {
                    ConeAlong(b, {0.14f * s, 1.18f, -0.36f}, {0.35f * s, 1, 0}, 0.09f, 0.18f, fur, 4);
                    b.Box({0.08f * s - 0.045f, 1.07f, -0.155f}, {0.08f * s + 0.045f, 1.085f, -0.14f}, 0x151515);  // ^ ^
                    Sphere(b, {0.24f * s, 1.04f, -0.36f}, 0.08f, 0x202028, 8);  // headphone cups
                    b.Box({Min(0.1f * s, 0.22f * s), 0.98f, -0.15f}, {Max(0.1f * s, 0.22f * s), 0.99f, -0.14f}, 0x404040);  // whiskers
                }
                Sphere(b, {0, 1.01f, -0.13f}, 0.025f, pink, 6);
                b.xf = Mat4::Translate({0, 1.04f, -0.36f}) * Mat4::RotateX(kPi * 0.5f);  // ring axis along z: ear to ear
                b.Torus(0.25f, 0.025f, 12, 4, 0xFF5FA2);  // headband
                b.xf = Mat4::Identity();
                break;
            }
            case Char_Honk: {  // a goose. Untrustworthy. Wings on the wheel.
                const u32 white = 0xF4F4F0, orange = 0xF29A2E;
                Sphere(b, {0, 0.58f, -0.42f}, 0.27f, white, 10, 1.1f);
                Arms(b, white, white, 0.07f);
                Tube(b, {{0, 0.75f, -0.38f}, {0, 0.98f, -0.33f}, {0, 1.18f, -0.24f}, {0, 1.28f, -0.16f}}, 0.085f, 0.065f, white, 8);
                Sphere(b, {0, 1.31f, -0.14f}, 0.1f, white, 10);
                ConeAlong(b, {0, 1.29f, -0.06f}, {0, -0.15f, 1}, 0.05f, 0.17f, orange, 6);
                Eyes(b, 1.35f, -0.09f, 0.06f, 0.018f);
                break;
            }
            case Char_RaveCrab: {  // claws up, eyes on stalks, ready to rave
                const u32 shell = 0xD8382A, light = 0xF07A5A;
                Sphere(b, {0, 0.62f, -0.38f}, 0.34f, shell, 12, 0.6f);
                Sphere(b, {0, 0.55f, -0.30f}, 0.22f, light, 10, 0.5f);
                for (float s : {-1.0f, 1.0f}) {
                    Tube(b, {{0.09f * s, 0.78f, -0.3f}, {0.11f * s, 1.02f, -0.28f}}, 0.025f, 0.02f, shell, 5);
                    Sphere(b, {0.11f * s, 1.05f, -0.28f}, 0.055f, 0xFFFFFF, 8);
                    Sphere(b, {0.11f * s, 1.06f, -0.235f}, 0.025f, 0x101010, 6);
                    Tube(b, {{0.28f * s, 0.62f, -0.38f}, {0.42f * s, 0.85f, -0.3f}, {0.40f * s, 1.12f, -0.22f}}, 0.06f, 0.055f, shell, 6);
                    ConeAlong(b, {0.38f * s, 1.12f, -0.22f}, {-0.3f * s, 1, 0.2f}, 0.07f, 0.18f, shell, 5);
                    ConeAlong(b, {0.43f * s, 1.12f, -0.22f}, {0.4f * s, 1, 0.2f}, 0.05f, 0.15f, light, 5);
                }
                break;
            }
            case Char_Banana: {  // a banana. Also: a ruler, for scale.
                const u32 yellow = 0xF7D63E, tip = 0x5A3E1E;
                Vec3  path[7];
                float radii[7];
                u32   cols[7];
                for (int i = 0; i < 7; ++i) {
                    float t  = (float)i / 6.0f;
                    path[i]  = {0, 0.36f + 1.05f * t, -0.44f + 0.18f * std::sin(t * kPi * 0.9f)};
                    radii[i] = 0.06f + 0.16f * std::sin(t * kPi * 0.95f + 0.05f);
                    cols[i]  = i == 6 ? tip : (i == 5 ? 0xE6C030 : yellow);
                }
                b.Sweep(path, radii, cols, 7, 10, true);
                for (float s : {-1.0f, 1.0f}) {
                    Sphere(b, {0.075f * s, 1.02f, -0.19f}, 0.05f, 0xFFFFFF, 8);
                    Sphere(b, {0.075f * s, 1.02f, -0.15f}, 0.024f, 0x101010, 6);
                }
                Arms(b, 0x1A1A1A, 0xFFFFFF, 0.028f);
                b.Box({0.69f, 0.42f, -0.62f}, {0.75f, 0.47f, 0.62f}, 0xF0E6C8);  // the ruler
                for (int m = 0; m < 13; ++m)
                    b.Box({0.748f, 0.44f, -0.6f + 0.1f * (float)m}, {0.756f, 0.47f, -0.59f + 0.1f * (float)m}, 0x202020);
                break;
            }
        }
        sc.characters[c] = b.Finish(dest, nullptr);
    }
}

void BuildItems(Scene& sc, Arena& dest, Arena& work) {
    {
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        b.lit = false;  // item boxes glow: no shading, just a light rainbow tint
        b.Box({-0.55f, -0.55f, -0.55f}, {0.55f, 0.55f, 0.55f}, 0xFFFFFF);
        sc.itemBox = b.Finish(dest, &sc.texItemBox);
    }
    {
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        const ProfilePoint dome[] = {{0.40f, 0.16f, 0xFFFFFF}, {0.38f, 0.26f, 0xFFFFFF}, {0.31f, 0.36f, 0xF4F4F4},
                                     {0.19f, 0.43f, 0xFFFFFF}, {0.0f, 0.46f, 0xFFFFFF}};
        b.Lathe(dome, 5, 12);
        sc.shellDome = b.Finish(dest, nullptr);
        MeshBuilder r;
        r.Init(work);
        r.xf = Mat4::Translate({0, 0.13f, 0});
        r.Torus(0.40f, 0.065f, 14, 6, 0xF4F4F4);
        r.xf = Mat4::Identity();
        const ProfilePoint under[] = {{0.0f, 0.03f, 0xE8DCB8}, {0.36f, 0.05f, 0xE8DCB8}, {0.40f, 0.10f, 0xE8DCB8}};
        r.Lathe(under, 3, 12);
        sc.shellRim = r.Finish(dest, nullptr);
    }
    {
        ScratchScope  scope(work);
        MeshBuilder   b;
        b.Init(work);
        constexpr int kN = 9;
        Vec3          path[kN];
        float         radii[kN];
        u32           cols[kN];
        for (int i = 0; i < kN; ++i) {
            float t  = (float)i / (kN - 1);
            float a  = (t - 0.5f) * 2.3f;
            path[i]  = {std::sin(a) * 0.42f, 0.12f + 0.10f * (1.0f - std::cos(a)), std::cos(a) * 0.42f - 0.36f};
            radii[i] = 0.035f + 0.085f * std::sin(t * kPi);
            cols[i]  = (i == 0 || i == kN - 1) ? 0x5A3E1E : (i == 1 || i == kN - 2 ? 0xE6C030 : 0xF7D63E);
        }
        b.Sweep(path, radii, cols, kN, 7, true);
        sc.banana = b.Finish(dest, nullptr);
    }
    {
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        b.lit = false;
        b.Quad({-0.75f, 0, -1.15f}, {-0.75f, 0, 1.3f}, {0.75f, 0, 1.3f}, {0.75f, 0, -1.15f}, 0);
        sc.shadow = b.Finish(dest, nullptr, Mesh_Darken);
        MeshBuilder s;
        s.Init(work);
        s.lit = false;
        s.Box({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, 0xFFFFFF);
        sc.spark = s.Finish(dest, nullptr, Mesh_NoFog);
        MeshBuilder l;
        l.Init(work);
        l.lit = false;
        Sphere(l, {0, 0, 0}, 0.2f, 0xFFFFFF, 8);
        sc.lamp = l.Finish(dest, nullptr, Mesh_NoFog);
        MeshBuilder p;
        p.Init(work);
        const ProfilePoint podium[] = {{0, 0, 0x2A2E38}, {2.4f, 0, 0x2A2E38}, {2.4f, 0.25f, 0xF2C230}, {2.3f, 0.3f, 0x3A3F4C}, {0, 0.3f, 0x3A3F4C}};
        p.Lathe(podium, 5, 24);
        sc.podium = p.Finish(dest, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Scenery models
// ---------------------------------------------------------------------------
void Pine(MeshBuilder& b, int v, bool snowy) {
    const ProfilePoint trunk[] = {{0.20f, 0.0f, 0x5E4026}, {0.16f, 1.4f, 0x6B4A2B}, {0.0f, 1.45f, 0x6B4A2B}};
    b.Lathe(trunk, 3, 7);
    int   tiers = 3 + (v == 2);
    float y = 1.0f, radius = 1.9f + 0.2f * (float)v;
    for (int t = 0; t < tiers; ++t) {
        u32   g    = LerpColor(snowy ? 0x1E4A2C : 0x24602A, snowy ? 0x2F6E44 : 0x3E9A46, (float)t / (float)tiers);
        u32   rim  = snowy ? 0xEEF4FA : g;
        u32   top  = snowy ? 0xFFFFFF : LerpColor(g, 0xFFFFFF, 0.1f);
        float h    = 2.2f - 0.25f * (float)t;
        const ProfilePoint tier[] = {{0.0f, y + 0.35f, LerpColor(g, 0x000000, 0.3f)}, {radius, y, rim},
                                     {radius * 0.5f, y + h * 0.5f, g}, {0.0f, y + h, top}};
        b.Lathe(tier, 4, 9, 0.3f, (u32)(v * 31 + t + (snowy ? 100 : 0)));
        y += h * 0.55f;
        radius *= 0.72f;
    }
}

void BuildPropModels(Scene& sc, Arena& dest, Arena& work, float wallHalf) {
    auto finish = [&](MeshBuilder& b, const Texture* tex = nullptr, u8 flags = 0) { return b.Finish(dest, tex, flags); };
    auto simple = [&](PropKind k, float radius, float centerY) {
        sc.props[k].radius  = radius;
        sc.props[k].centerY = centerY;
    };

    for (int v = 0; v < 3; ++v) {  // pines
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        Pine(b, v, false);
        PropModel& m = sc.props[Prop_Pine0 + v];
        m.tinted     = finish(b);
        MeshBuilder lo;
        lo.Init(work);
        lo.Cone({0, 0.8f, 0}, 2.0f + 0.2f * (float)v, 5.0f, 5, 0x2F7D32);
        m.lod     = finish(lo, nullptr, Mesh_DoubleSided);
        m.lodDist = 70.0f;
        simple((PropKind)(Prop_Pine0 + v), 4.0f, 3.0f);
    }
    for (int v = 0; v < 2; ++v) {  // snowy pines
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        Pine(b, v + 1, true);
        PropModel& m = sc.props[Prop_SnowPine0 + v];
        m.tinted     = finish(b);
        MeshBuilder lo;
        lo.Init(work);
        lo.Cone({0, 0.8f, 0}, 2.1f, 5.0f, 5, 0xD8E4EE);
        m.lod     = finish(lo, nullptr, Mesh_DoubleSided);
        m.lodDist = 70.0f;
        simple((PropKind)(Prop_SnowPine0 + v), 4.0f, 3.0f);
    }
    for (int v = 0; v < 2; ++v) {  // broadleaf
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        const ProfilePoint trunk[] = {{0.26f, 0.0f, 0x5E4026}, {0.2f, 2.0f, 0x6B4A2B}, {0.0f, 2.1f, 0x6B4A2B}};
        b.Lathe(trunk, 3, 7);
        u32                c0 = v ? 0x4C9A3A : 0x3E8A3A, c1 = v ? 0x7CC24E : 0x5FAE4A;
        const ProfilePoint canopy[] = {{0.0f, 1.6f, LerpColor(c0, 0x000000, 0.3f)}, {1.1f, 1.8f, c0}, {1.7f, 2.6f, c0},
                                       {1.6f, 3.4f, c1}, {1.0f, 4.1f, c1}, {0.0f, 4.35f, c1}};
        b.Lathe(canopy, 6, 10, 0.28f, 77u + (u32)v);
        PropModel& m = sc.props[Prop_Round0 + v];
        m.tinted     = finish(b);
        MeshBuilder lo;
        lo.Init(work);
        const ProfilePoint blob[] = {{0.0f, 1.4f, c0}, {1.6f, 2.6f, c0}, {0.0f, 4.2f, c1}};
        lo.Lathe(blob, 3, 5);
        m.lod     = finish(lo);
        m.lodDist = 70.0f;
        simple((PropKind)(Prop_Round0 + v), 3.5f, 2.5f);
    }
    for (int v = 0; v < 2; ++v) {  // rocks (tintable per theme)
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        u32                c = v ? 0xA0A098 : 0x8E939A;
        const ProfilePoint rock[] = {{0.0f, -0.2f, c}, {1.0f, 0.0f, c}, {1.05f, 0.5f, c},
                                     {0.7f, 0.95f, LerpColor(c, 0xFFFFFF, 0.15f)}, {0.0f, 1.05f, LerpColor(c, 0xFFFFFF, 0.2f)}};
        b.Lathe(rock, 5, 7, 0.45f, 501u + (u32)v);
        sc.props[Prop_Rock0 + v].tinted = finish(b);
        simple((PropKind)(Prop_Rock0 + v), 1.4f, 0.5f);
    }
    for (int v = 0; v < 2; ++v) {  // saguaro cacti
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        const u32 g = v ? 0x5C9A48 : 0x4F8A3C;
        float     h = v ? 3.2f : 4.2f;
        Tube(b, {{0, 0, 0}, {0, h * 0.5f, 0}, {0, h, 0}}, 0.38f, 0.32f, g, 8, true);
        Tube(b, {{0.3f, 1.6f, 0}, {0.85f, 1.7f, 0}, {0.95f, 2.2f, 0}, {0.95f, 3.0f, 0}}, 0.2f, 0.17f, g, 6, true);
        if (!v) Tube(b, {{-0.3f, 2.2f, 0}, {-0.75f, 2.3f, 0}, {-0.8f, 2.8f, 0}, {-0.8f, 3.4f, 0}}, 0.18f, 0.15f, g, 6, true);
        sc.props[Prop_Cactus0 + v].tinted = finish(b);
        simple((PropKind)(Prop_Cactus0 + v), 2.5f, 2.0f);
    }
    {  // mesa: banded, flat-topped
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        const ProfilePoint mesa[] = {{0, -0.5f, 0xB0603A}, {12, -0.5f, 0xB0603A}, {11.5f, 3, 0xC8744A}, {10.8f, 3.4f, 0xE0A070},
                                     {10.5f, 7, 0xC8744A}, {9, 8, 0xD89060},     {0, 8.2f, 0xE0A878}};
        b.Lathe(mesa, 7, 12, 0.15f, 9001);
        PropModel& m = sc.props[Prop_Mesa];
        m.tinted     = finish(b);
        simple(Prop_Mesa, 13.0f, 4.0f);
    }
    {  // snowman with a carrot nose and a top hat
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        Sphere(b, {0, 0.7f, 0}, 0.8f, 0xF4F8FC, 10);
        Sphere(b, {0, 1.75f, 0}, 0.58f, 0xF4F8FC, 10);
        Sphere(b, {0, 2.55f, 0}, 0.4f, 0xF4F8FC, 10);
        ConeAlong(b, {0, 2.55f, -0.36f}, {0, 0, -1}, 0.07f, 0.35f, 0xF28A2E, 6);
        Eyes(b, 2.68f, -0.33f, 0.13f, 0.045f);
        const ProfilePoint hat[] = {{0, 2.86f, 0x151515}, {0.42f, 2.86f, 0x151515}, {0.42f, 2.92f, 0x151515}, {0.27f, 2.92f, 0x151515},
                                    {0.27f, 3.35f, 0x151515}, {0, 3.35f, 0x151515}};
        b.Lathe(hat, 6, 10);
        for (int k = 0; k < 3; ++k) Sphere(b, {0, 1.55f + 0.25f * (float)k, -0.56f + 0.05f * (float)k}, 0.05f, 0x202020, 6);
        sc.props[Prop_Snowman].fixed = finish(b);
        simple(Prop_Snowman, 2.0f, 1.6f);
    }
    for (int v = 0; v < 2; ++v) {  // palms: banded curved trunk and drooping fronds
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        float lean = v ? 1.4f : -0.9f, h = v ? 6.5f : 5.5f;
        Vec3  path[8];
        float radii[8];
        u32   cols[8];
        for (int i = 0; i < 8; ++i) {
            float t  = (float)i / 7.0f;
            path[i]  = {lean * t * t, h * t, 0};
            radii[i] = 0.3f - 0.12f * t;
            cols[i]  = (i & 1) ? 0x6B3B5E : 0x8A4E78;
        }
        b.Sweep(path, radii, cols, 8, 7, true);
        Vec3 top = path[7];
        for (int f = 0; f < 7; ++f) {
            float a   = (float)f * 2 * kPi / 7 + 0.3f * (float)v;
            Vec3  dir = {std::sin(a), 0, std::cos(a)};
            Vec3  side = Normalize(Cross(dir, {0, 1, 0})) * 0.32f;
            Vec3  p0 = top, p1 = top + dir * 1.3f + Vec3{0, 0.35f, 0}, p2 = top + dir * 2.4f + Vec3{0, -0.5f, 0};
            QuadFacing(b, p0 - side * 0.3f, p1 - side, p1 + side, p0 + side * 0.3f, 0x1FA39A, {0, 1, 0});
            QuadFacing(b, p1 - side, p2 - side * 0.2f, p2 + side * 0.2f, p1 + side, 0x18887F, {0, 1, 0});
        }
        PropModel& m = sc.props[Prop_Palm0 + v];
        m.tinted     = finish(b, nullptr, Mesh_DoubleSided);
        simple((PropKind)(Prop_Palm0 + v), 4.5f, 3.5f);
    }
    {  // marble column
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        const u32          m0 = 0xF2E6F0, m1 = 0xFFF4FC;
        const ProfilePoint col[] = {{0, 0, m0},       {0.9f, 0, m0},    {0.9f, 0.3f, m1}, {0.6f, 0.32f, m0}, {0.52f, 0.5f, m1},
                                    {0.46f, 5.0f, m1}, {0.6f, 5.15f, m0}, {0.9f, 5.3f, m1}, {0.9f, 5.6f, m0}, {0, 5.6f, m0}};
        b.Lathe(col, 10, 12);
        sc.props[Prop_Column].fixed = finish(b);
        simple(Prop_Column, 3.2f, 2.8f);
    }
    {  // tyre stack
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        for (int t = 0; t < 3; ++t) {
            b.xf = Mat4::Translate({0, 0.17f + 0.31f * (float)t, 0});
            b.Torus(0.36f, 0.15f, 10, 6, t == 1 ? 0xFFFFFF : 0x1E1E1E);  // middle tyre takes the tint
        }
        sc.props[Prop_TyreStack].tinted = finish(b);
        simple(Prop_TyreStack, 1.0f, 0.5f);
    }
    for (int dir = 0; dir < 2; ++dir) {  // chevron boards facing oncoming traffic (-z)
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        float s = dir ? 1.0f : -1.0f;
        b.Box({-1.6f, -8.0f, 0.0f}, {-1.45f, 1.9f, 0.15f}, 0x808890);  // posts reach the ground beside raised track
        b.Box({1.45f, -8.0f, 0.0f}, {1.6f, 1.9f, 0.15f}, 0x808890);
        b.Box({-1.7f, 1.0f, -0.05f}, {1.7f, 2.0f, 0.05f}, 0x15151A);
        Vec3 face{0, 0, -1};
        for (int c = 0; c < 3; ++c) {
            float cx = (-0.9f + 0.9f * (float)c) * s, cy = 1.5f, z = -0.07f;
            QuadFacing(b, {cx - 0.20f * s, cy + 0.36f, z}, {cx - 0.02f * s, cy + 0.36f, z}, {cx + 0.18f * s, cy, z}, {cx, cy, z},
                       0xFFD020, face);
            QuadFacing(b, {cx, cy, z}, {cx + 0.18f * s, cy, z}, {cx - 0.02f * s, cy - 0.36f, z}, {cx - 0.20f * s, cy - 0.36f, z},
                       0xFFD020, face);
        }
        PropKind k        = dir ? Prop_ChevronRight : Prop_ChevronLeft;
        sc.props[k].fixed = finish(b);
        simple(k, 2.2f, 1.2f);
    }
    {  // flag
        ScratchScope scope(work);
        MeshBuilder  pole;
        pole.Init(work);
        Tube(pole, {{0, 0, 0}, {0, 5.2f, 0}}, 0.06f, 0.05f, 0xC8CCD0, 6, true);
        MeshBuilder cloth;
        cloth.Init(work);
        for (int k = 0; k < 4; ++k) {
            float x0 = 0.05f + 0.45f * (float)k, x1 = x0 + 0.45f;
            float z0 = std::sin((float)k * 1.3f) * 0.15f, z1 = std::sin((float)(k + 1) * 1.3f) * 0.15f;
            cloth.Quad({x0, 3.9f, z0}, {x0, 5.1f, z0}, {x1, 5.1f, z1}, {x1, 3.9f, z1}, 0xFFFFFF);
        }
        sc.props[Prop_Flag].fixed  = finish(pole);
        sc.props[Prop_Flag].tinted = finish(cloth, nullptr, Mesh_DoubleSided);
        simple(Prop_Flag, 3.0f, 2.6f);
    }
    {  // grandstand. Local: x along the track, +z away from it, y up.
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        const Vec2 steps[] = {{0, 0}, {0, 0.8f}, {1.2f, 0.8f}, {1.2f, 1.6f}, {2.4f, 1.6f}, {2.4f, 2.4f},
                              {3.6f, 2.4f}, {3.6f, 3.2f}, {4.8f, 3.2f}, {4.8f, 0}};
        b.Extrude(steps, (int)SS_COUNTOF(steps), -18.0f, 18.0f, 0xB8B8B0);
        b.Box({-18.5f, 4.6f, -0.6f}, {18.5f, 4.85f, 5.1f}, 0xDDE6EE);
        for (float x = -18.0f; x <= 18.01f; x += 6.0f) b.Box({x - 0.12f, 3.2f, 4.7f}, {x + 0.12f, 4.6f, 4.95f}, 0x707880);
        for (int seg = 0; seg < 12; ++seg)
            b.Box({-18.0f + 3.0f * (float)seg, 0.1f, -0.06f}, {-15.0f + 3.0f * (float)seg, 0.75f, 0.0f}, (seg & 1) ? 0xF2F2F2 : 0xD8342C);
        u32       rnd      = 4242;
        auto      next     = [&]() { rnd = HashU32(rnd + 0x9E3779B9u); return (float)(rnd % 10000) / 10000.0f; };
        const u32 shirts[] = {0xE8433A, 0x3A7BE8, 0x3CC25A, 0xF2C230, 0x9B59D9, 0xF0F0F0, 0x303438, 0xF28A2E};
        const u32 skins[]  = {0xF1C7A5, 0xD9A27E, 0xA8714D, 0x6E4A33, 0xF5D7BE};
        for (int row = 0; row < 4; ++row) {
            float top = 0.8f * (float)(row + 1), z = 1.2f * (float)row + 0.45f;
            for (float x = -17.6f; x < 17.6f; x += 0.55f) {
                if (next() < 0.15f) continue;
                float px    = x + (next() - 0.5f) * 0.2f;
                u32   shirt = shirts[(int)(next() * 8) % 8], skin = skins[(int)(next() * 5) % 5];
                QuadFacing(b, {px - 0.17f, top, z}, {px - 0.17f, top + 0.5f, z}, {px + 0.17f, top + 0.5f, z}, {px + 0.17f, top, z},
                           shirt, {0, 0, -1});
                QuadFacing(b, {px - 0.1f, top + 0.52f, z}, {px - 0.1f, top + 0.74f, z}, {px + 0.1f, top + 0.74f, z},
                           {px + 0.1f, top + 0.52f, z}, skin, {0, 0, -1});
            }
        }
        sc.props[Prop_Grandstand].fixed = finish(b);
        simple(Prop_Grandstand, 19.5f, 2.5f);
    }
    {  // start gantry. Local: x across the track, z along it.
        ScratchScope scope(work);
        MeshBuilder  b;
        b.Init(work);
        float px = wallHalf + 0.5f;
        b.Box({-px - 0.3f, 0, -0.3f}, {-px + 0.3f, 6.2f, 0.3f}, 0x5A6068);
        b.Box({px - 0.3f, 0, -0.3f}, {px + 0.3f, 6.2f, 0.3f}, 0x5A6068);
        b.Box({-px - 0.3f, 4.95f, -0.35f}, {px + 0.3f, 6.25f, 0.35f}, 0x2A2E34);
        b.Box({-2.0f, 4.35f, -0.3f}, {2.0f, 4.95f, 0.2f}, 0x1A1C20);
        MeshBuilder banner;
        banner.Init(work);
        float w = px - 0.6f;
        banner.Quad({-w, 5.05f, -0.37f}, {-w, 6.15f, -0.37f}, {w, 6.15f, -0.37f}, {w, 5.05f, -0.37f}, 0xFFFFFF, 0, 0, w, 1);
        banner.Quad({w, 5.05f, 0.37f}, {w, 6.15f, 0.37f}, {-w, 6.15f, 0.37f}, {-w, 5.05f, 0.37f}, 0xFFFFFF, 0, 0, w, 1);
        sc.props[Prop_Gantry].fixed  = finish(b);
        sc.props[Prop_Gantry].tinted = finish(banner, &sc.texChecker);
        simple(Prop_Gantry, px + 1.0f, 3.0f);
    }
}

bool ClearOfTrack(const Track& t, float x, float z, float clearance) {
    for (int i = 0; i < t.count; i += 2) {
        float dx = t.s[i].pos.x.ToFloat() - x, dz = t.s[i].pos.z.ToFloat() - z;
        if (dx * dx + dz * dz < clearance * clearance) return false;
    }
    return true;
}

void PlaceProps(Scene& sc, const Track& t, const ThemeDef& theme) {
    float wall = t.wallHalf.ToFloat();
    auto  add  = [&](PropKind k, Vec3 p, float yaw, float scale, u32 tint) {
        if (sc.instanceCount < kMaxProps) sc.instances[sc.instanceCount++] = {(u8)k, p, yaw, scale, tint};
    };

    const TrackSample& s0 = t.s[0];
    sc.gantryPos          = ToVec3(s0.pos, s0.y);
    sc.gantryYaw          = AngleToRadians(AngleOf(s0.tangent));
    add(Prop_Gantry, sc.gantryPos, sc.gantryYaw, 1.0f, 0xFFFFFF);

    // Grandstand beside the run to the line, if there's room for it.
    Vec3 standPos{1e9f, 0, 1e9f};
    for (int back : {8, 14, 20}) {
        const TrackSample& sg = t.s[t.count - back];
        Vec3 p = ToVec3(sg.pos, Fx{0}) - Flat(sg.right) * (wall + 2.5f) + Vec3{0, -0.6f, 0};
        Vec3 c = p - Flat(sg.right) * 2.4f;
        Vec3 along = Flat(sg.tangent) * 18.0f;
        if (ClearOfTrack(t, (c + along).x, (c + along).z, wall + 4.0f) && ClearOfTrack(t, (c - along).x, (c - along).z, wall + 4.0f)) {
            standPos = p;
            add(Prop_Grandstand, p, AngleToRadians(AngleOf(sg.tangent)) - kPi * 0.5f, 1.0f, 0xFFFFFF);
            break;
        }
    }

    int flagColor = 0;
    for (int i = 0; i < t.count; ++i) {
        const TrackSample& s     = t.s[i];
        float              curv  = s.curvature.ToFloat();
        float              yaw   = AngleToRadians(AngleOf(s.tangent));
        Vec3               base  = ToVec3(s.pos, Fx{0}) + Vec3{0, -0.6f, 0};
        Vec3               right = Flat(s.right);
        if (std::fabs(curv) > 0.22f && i % 9 == 0) {
            float outside = curv > 0 ? -1.0f : 1.0f;
            Vec3  p       = base + right * (outside * (wall + 1.6f));
            if (ClearOfTrack(t, p.x, p.z, wall + 1.0f)) {
                p.y = s.y.ToFloat();
                add(curv > 0 ? Prop_ChevronRight : Prop_ChevronLeft, p, yaw, 1.0f, 0xFFFFFF);
                for (float along : {-3.0f, 3.0f}) {
                    Vec3 tp = p + Flat(s.tangent) * along + right * (outside * 0.4f);
                    tp.y    = -0.6f;
                    add(Prop_TyreStack, tp, yaw, 1.0f, (flagColor++ & 1) ? 0xE8433A : 0xF2F2F2);
                }
            }
        }
        if (std::fabs(curv) < 0.04f && i % 16 == 5) {
            for (float side : {-1.0f, 1.0f}) {
                Vec3 p = base + right * (side * (wall + 2.2f));
                if (!ClearOfTrack(t, p.x, p.z, wall + 1.5f)) continue;
                add(Prop_Flag, p, yaw + (side > 0 ? kPi : 0.0f), 1.0f, kCharacters[flagColor++ % Char_Count].color);
            }
        }
    }

    // Scatter the theme's scenery, clear of the track and the grandstand.
    float totalWeight = 0;
    for (int e = 0; e < theme.scatterCount; ++e) totalWeight += theme.scatter[e].weight;
    u32  rnd  = 12345u + (u32)t.id * 7919u;
    auto next = [&]() { rnd = HashU32(rnd + 0x9E3779B9u); return (float)(rnd % 100000) / 100000.0f; };
    for (int attempt = 0; attempt < 6000 && sc.instanceCount < kMaxProps && sc.instanceCount < 380; ++attempt) {
        float x = (next() - 0.5f) * 680.0f, z = (next() - 0.5f) * 680.0f;
        float pick = next() * totalWeight;
        int   e    = 0;
        while (e < theme.scatterCount - 1 && pick > theme.scatter[e].weight) pick -= theme.scatter[e++].weight;
        const ScatterEntry& se    = theme.scatter[e];
        float               scale = se.minScale + (se.maxScale - se.minScale) * next();
        float               clear = wall + 6.0f + (se.kind == Prop_Mesa ? 18.0f * scale : 0.0f);
        if (!ClearOfTrack(t, x, z, clear)) continue;
        float dx = x - standPos.x, dz = z - standPos.z;
        if (dx * dx + dz * dz < 30.0f * 30.0f) continue;
        add(se.kind, {x, -0.6f, z}, next() * 2 * kPi, scale, se.tint);
    }
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
struct Frustum {
    Mat4  view;
    float kx, ky, far;
    float nx, ny;

    bool SphereVisible(Vec3 c, float r) const {
        Vec3 p = TransformPoint(view, c);
        if (p.z < -r || p.z > far + r) return false;
        if ((kx * p.x - p.z) * nx > r || (-kx * p.x - p.z) * nx > r) return false;
        if ((ky * p.y - p.z) * ny > r || (-ky * p.y - p.z) * ny > r) return false;
        return true;
    }
};

Frustum MakeFrustum(RenderContext& rc, const Camera& cam) {
    float   aspect = (float)rc.target.width / (float)rc.target.height;
    Frustum fr;
    rc.viewProj = CameraViewProj(cam, aspect, &fr.view);
    float f     = 1.0f / std::tan(cam.fovY * 0.5f);
    fr.kx       = f / aspect;
    fr.ky       = f;
    fr.far      = cam.farDist;
    fr.nx       = 1.0f / std::sqrt(fr.kx * fr.kx + 1.0f);
    fr.ny       = 1.0f / std::sqrt(fr.ky * fr.ky + 1.0f);
    return fr;
}

void DrawSky(RenderContext& rc, const SceneTheme& th, const Camera& cam) {
    RenderTarget& rt   = rc.target;
    Vec3          fwd  = cam.target - cam.eye;
    Vec3          flat = Normalize(Vec3{fwd.x, 0, fwd.z});
    Vec4          hp   = Transform(rc.viewProj, cam.eye + flat * 1000.0f);
    float horizon = hp.w > 0 ? (0.5f - hp.y / hp.w * 0.5f) * (float)rt.height : (float)rt.height * 0.5f;
    int   hy      = Clamp((int)horizon, 0, rt.height);

    for (int y = 0; y < rt.height; ++y) {
        float t   = y < hy ? Clamp((float)y / Max(horizon, 1.0f), 0.0f, 1.0f) : 1.0f;
        u32   col = LerpColor(th.skyTop, th.skyHorizon, th.sunset ? t : t * t);
        u32*  row = rt.color + y * rt.width;
        for (int x = 0; x < rt.width; ++x) row[x] = col;
    }

    float yaw    = std::atan2(fwd.x, fwd.z);
    float f      = 1.0f / std::tan(cam.fovY * 0.5f);
    float aspect = (float)rt.width / (float)rt.height;

    // Synthwave sun: a gradient disc fixed in the sky, sliced by horizontal gaps.
    if (th.sunset) {
        float rel = 1.75f - yaw;  // fixed world bearing: dead ahead down Boulevard's start straight
        while (rel > kPi) rel -= 2 * kPi;
        while (rel < -kPi) rel += 2 * kPi;
        if (std::fabs(rel) < 1.4f) {
            float cx = ((std::tan(rel) * f / aspect) * 0.5f + 0.5f) * (float)rt.width;
            float r  = (float)rt.height * 0.2f, cy = horizon - r * 1.05f;  // high enough that the sliced half shows
            for (int y = Max((int)(cy - r), 0); y < Min((int)(cy + r), hy); ++y) {
                float dy = (float)y - cy, t = (dy + r) / (2 * r);
                if (t > 0.5f && std::fmod((float)y - (cy - r), r * 0.16f) < r * 0.16f * (t - 0.45f) * 1.6f) continue;  // slices
                float half = std::sqrt(Max(r * r - dy * dy, 0.0f));
                u32   col  = LerpColor(0xFFE45C, 0xFF3E9A, t);
                for (int x = Max((int)(cx - half), 0); x < Min((int)(cx + half), rt.width); ++x) rt.color[y * rt.width + x] = col;
            }
        }
    }

    for (int range = 0; range < 2; ++range) {
        u32   col    = range == 0 ? LerpColor(th.mountainFar, th.skyHorizon, 0.3f) : th.mountainNear;
        float amp    = range == 0 ? 0.16f : 0.10f;
        float offset = range == 0 ? 1.7f : 0.0f;
        for (int x = 0; x < rt.width; ++x) {
            float ndc = ((float)x + 0.5f) / (float)rt.width * 2.0f - 1.0f;
            float a   = yaw + std::atan(ndc * aspect / f) + offset;
            float h   = 0.55f + 0.22f * std::sin(a * 3.0f + 0.5f) + 0.14f * std::sin(a * 7.0f + 1.3f) + 0.07f * std::sin(a * 17.0f);
            int top = (int)(horizon - h * amp * (float)rt.height);
            for (int y = Max(top, 0); y < Min(hy + 1, rt.height); ++y) rt.color[y * rt.width + x] = col;
        }
    }
}

Mat4 KartModel(const Kart& k, float timeSeconds) {
    float hop  = k.hopTicks > 0 ? std::sin((float)k.hopTicks / 8.0f * kPi) * 0.35f : 0.0f;
    float bump = k.bumpTicks > 0 ? std::sin(timeSeconds * 60.0f) * 0.05f * (float)k.bumpTicks / 10.0f : 0.0f;
    float lean = (float)k.driftDir * -0.12f;
    // Spin-out: exactly two full turns, fast then easing out, ending where it began.
    float spin = 0.0f;
    if (k.spinTicks > 0) {
        float t = 1.0f - (float)k.spinTicks / (float)kSpinTicks;
        spin    = 4.0f * kPi * (1.0f - (1.0f - t) * (1.0f - t));
    }
    Vec3 pos{k.pos.x.ToFloat(), k.y.ToFloat() + hop, k.pos.z.ToFloat()};
    return Mat4::Translate(pos) * Mat4::RotateY(AngleToRadians(k.heading) + spin) * Mat4::RotateZ(lean + bump);
}

// Chassis + driver + wheels. `lowDetail` swaps in the distant stand-in.
void DrawKart(RenderContext& rc, const Scene& sc, const Mat4& model, int character, u32 paint, float wheelSpin, float steer,
              bool lowDetail, float time) {
    if (lowDetail) {
        DrawMesh(rc, sc.kartLowTinted, model, paint);
        DrawMesh(rc, sc.kartLowFixed, model);
        return;
    }
    DrawMesh(rc, sc.kartBody, model, paint);
    DrawMesh(rc, sc.kartParts, model);
    Mat4 driver = model;
    if (character == Char_VibeCat)  // vibing: a steady head-bob
        driver = model * Mat4::Translate({0, 0.035f * std::sin(time * 9.0f), -0.36f}) * Mat4::RotateZ(0.12f * std::sin(time * 4.5f)) *
                 Mat4::Translate({0, 0, 0.36f});
    if (character == Char_RaveCrab)  // raving: bounce
        driver = model * Mat4::Translate({0, 0.05f * std::fabs(std::sin(time * 8.0f)), 0});
    DrawMesh(rc, sc.characters[Clamp(character, 0, kNumCharacters - 1)], driver);
    const Mat4 identity = Mat4::Identity();
    for (int w = 0; w < 4; ++w) {
        bool  front = w < 2, rightSide = (w & 1) != 0;
        float a     = wheelSpin * (front ? kRearWheelR / kFrontWheelR : 1.0f);
        Mat4  m = model * Mat4::Translate(kWheelPos[w]) * (front ? Mat4::RotateY(steer) : identity) *
                 (rightSide ? Mat4::RotateY(kPi) : identity) * Mat4::RotateX(rightSide ? -a : a);
        DrawMesh(rc, front ? sc.wheelFront : sc.wheelRear, m);
    }
}

void DrawKartEffects(RenderContext& rc, const Scene& sc, const Kart& k, int index, float time) {
    Mat4 model = KartModel(k, time);
    auto puff  = [&](Vec3 local, float size, u32 color) {
        DrawMesh(rc, sc.spark, model * Mat4::Translate(local) * Mat4::Scale({size, size, size}), color);
    };
    u32  jitterSeed = HashU32((u32)(time * 60.0f) * 31u + (u32)index);
    auto jitter     = [&](int n) { return ((float)(HashU32(jitterSeed + (u32)n) % 100) / 100.0f - 0.5f); };

    if (k.driftDir != 0) {
        u32   col  = k.driftCharge >= kUltraTurbo ? 0xC060FF
                   : k.driftCharge >= kSuperTurbo ? 0xFF8A1C
                   : k.driftCharge >= kMiniTurbo  ? 0x3FA9FF
                                                  : 0xE8E8E8;
        float size = k.driftCharge >= kUltraTurbo ? 0.26f : (k.driftCharge >= kMiniTurbo ? 0.2f : 0.11f);
        for (int w = 0; w < 2; ++w)
            for (int s = 0; s < 3; ++s)
                puff({(w ? 0.72f : -0.72f) + jitter(w * 7 + s) * 0.3f, 0.1f + jitter(s + 3) * 0.2f, -0.95f - (float)s * 0.18f},
                     size * (1.0f - (float)s * 0.2f), col);
    }
    if (k.draftTicks > 8) {  // slipstream: wind streaks rushing past
        int streaks = 2 + k.draftTicks * 6 / kDraftChargeTicks;
        for (int s = 0; s < streaks; ++s) {
            float side  = (s & 1) ? 1.0f : -1.0f;
            float phase = std::fmod(time * 3.0f + (float)s * 0.37f, 1.0f);
            Vec3  p{side * (0.85f + 0.25f * (float)((s / 2) % 2)), 0.35f + 0.25f * (float)((s / 2) % 3), 1.6f - 4.0f * phase};
            DrawMesh(rc, sc.spark, model * Mat4::Translate(p) * Mat4::Scale({0.025f, 0.025f, 0.9f}), 0xDDEEFF);
        }
    }
    if (k.boostTicks > 0)
        for (int s = 0; s < 3; ++s)
            for (float side : {-0.27f, 0.27f})
                puff({side + jitter(s) * 0.1f, 0.44f + jitter(s + 9) * 0.06f, -1.25f - (float)s * 0.22f}, 0.24f - (float)s * 0.05f,
                     s == 0 ? 0xFFE45C : 0xFF7A1A);
}

void UpdateVisuals(KartVisuals& v, const SimState& s) {
    int dt = v.valid ? Clamp(s.tick - v.lastTick, 0, 10) : 0;
    for (int i = 0; i < s.numKarts && i < 8; ++i) {
        const Kart& k = s.karts[i];
        if (!v.valid) {
            v.lastHeading[i] = k.heading;
            v.wheelAngle[i] = v.steer[i] = 0;
        }
        float turn   = dt ? (float)AngleDelta(v.lastHeading[i], k.heading) / (float)dt : 0.0f;
        float target = k.driftDir ? -0.35f * (float)k.driftDir : Clamp(turn / 380.0f, -1.0f, 1.0f) * 0.45f;
        if (k.spinTicks) target = 0;
        v.steer[i] += (target - v.steer[i]) * 0.35f;
        v.wheelAngle[i] += k.speed.ToFloat() * (float)dt / 0.23f;
        v.lastHeading[i] = k.heading;
    }
    v.lastTick = s.tick;
    v.valid    = true;
}

void DrawGround(RenderContext& rc, const Scene& sc, const Frustum& fr) {
    for (int gi = 0; gi < sc.groundCount; ++gi)
        if (fr.SphereVisible(sc.groundCenters[gi], sc.groundRadius)) DrawMesh(rc, sc.groundTiles[gi], Mat4::Identity());
}

void ApplyFog(RenderContext& rc, const SceneTheme& th) {
    rc.fogColor = th.fog;
    rc.fogStart = th.fogStart;
    rc.fogEnd   = th.fogEnd;
}

}  // namespace

// ---------------------------------------------------------------------------

void BuildScene(Scene& sc, const Track& track, Arena& dest, Arena& work) {
    sc                    = {};
    const ThemeDef& theme = kThemes[Clamp(track.theme, 0, Theme_Count - 1)];
    sc.theme              = theme.look;
    g_lightLevel          = theme.lightLevel;
    sc.texAsphalt = MakeTexture(dest, 6, AsphaltTexel);
    sc.texGround  = MakeTexture(dest, 6, theme.ground);
    sc.texWall    = MakeTexture(dest, 6, WallTexel);
    sc.texChecker = MakeTexture(dest, 4, CheckerTexel);
    sc.texItemBox = MakeTexture(dest, 6, ItemBoxTexel);
    BuildTrackChunks(sc, track, theme, dest, work);
    BuildGround(sc, dest, work);
    BuildKart(sc, dest, work);
    BuildCharacters(sc, dest, work);
    BuildItems(sc, dest, work);
    BuildPropModels(sc, dest, work, track.wallHalf.ToFloat());
    PlaceProps(sc, track, theme);
    g_lightLevel = 1.0f;
}

Mat4 CameraViewProj(const Camera& cam, float aspect, Mat4* outView) {
    Mat4 view = Mat4::LookAt(cam.eye, cam.target, {0, 1, 0});
    if (outView) *outView = view;
    return Mat4::Perspective(cam.fovY, aspect) * view;
}

void DrawWorld(RenderContext& rc, const Scene& sc, const SimState& s, const Camera& cam, float time, KartVisuals* visuals) {
    Frustum fr = MakeFrustum(rc, cam);
    ApplyFog(rc, sc.theme);
    if (visuals) UpdateVisuals(*visuals, s);

    DrawSky(rc, sc.theme, cam);
    ClearDepth(rc.target);
    const Mat4 identity = Mat4::Identity();

    // Track chunks front-to-back so hidden pixels are rejected before shading.
    int   order[kMaxChunks];
    float dist[kMaxChunks];
    int   visible = 0;
    for (int c = 0; c < sc.chunkCount; ++c) {
        const TrackChunk& ch = sc.chunks[c];
        if (!fr.SphereVisible(ch.center, ch.radius)) continue;
        float d = Length(ch.center - cam.eye);
        int   j = visible++;
        while (j > 0 && dist[j - 1] > d) {
            order[j] = order[j - 1];
            dist[j]  = dist[j - 1];
            --j;
        }
        order[j] = c;
        dist[j]  = d;
    }
    for (int v = 0; v < visible; ++v) {
        const TrackChunk& ch = sc.chunks[order[v]];
        DrawMesh(rc, ch.road, identity);
        DrawMesh(rc, ch.curb, identity);
        DrawMesh(rc, ch.wall, identity);
        DrawMesh(rc, ch.verge, identity);
    }
    DrawMesh(rc, sc.startLine, identity);

    for (int i = 0; i < s.numKarts; ++i) {
        const Kart& k = s.karts[i];
        Vec3        p{k.pos.x.ToFloat(), k.y.ToFloat(), k.pos.z.ToFloat()};
        if (!fr.SphereVisible(p + Vec3{0, 0.6f, 0}, 1.8f)) continue;
        u32 paint = k.starTicks > 0 ? Hsv(time * 3.0f + (float)i * 0.2f, 0.8f, 1.0f) : kCharacters[k.character].color;
        DrawKart(rc, sc, KartModel(k, time), k.character, paint, visuals ? visuals->wheelAngle[i] : 0.0f,
                 visuals ? visuals->steer[i] : 0.0f, Length(p - cam.eye) > kKartLodDist, time);
        DrawMesh(rc, sc.shadow, Mat4::Translate(p + Vec3{0, 0.05f, 0}) * Mat4::RotateY(AngleToRadians(k.heading)));
        DrawKartEffects(rc, sc, k, i, time);
    }

    for (int i = 0; i < s.numBoxes; ++i) {
        const ItemBox& b = s.boxes[i];
        if (b.respawnTicks > 20) continue;
        float grow = b.respawnTicks > 0 ? 1.0f - (float)b.respawnTicks / 20.0f : 1.0f;
        Vec3  p{b.pos.x.ToFloat(), b.y.ToFloat() + 1.0f + std::sin(time * 2.0f + (float)i) * 0.12f, b.pos.z.ToFloat()};
        if (!fr.SphereVisible(p, 1.0f)) continue;
        Mat4 m = Mat4::Translate(p) * Mat4::RotateY(time * 1.5f + (float)i) * Mat4::RotateX(0.6f) * Mat4::Scale({grow, grow, grow});
        DrawMesh(rc, sc.itemBox, m, Hsv(time * 0.3f + (float)i * 0.07f, 0.22f, 1.0f));
    }
    for (const Hazard& h : s.hazards) {
        if (!h.active) continue;
        Vec3 p{h.pos.x.ToFloat(), h.y.ToFloat(), h.pos.z.ToFloat()};
        if (fr.SphereVisible(p, 1.0f))
            DrawMesh(rc, sc.banana, Mat4::Translate(p) * Mat4::RotateY((float)h.owner * 1.7f) * Mat4::Scale({1.4f, 1.4f, 1.4f}));
    }
    for (const Projectile& p : s.projectiles) {
        if (!p.kind) continue;
        Vec3 pos{p.pos.x.ToFloat(), p.y.ToFloat(), p.pos.z.ToFloat()};
        if (!fr.SphereVisible(pos, 1.0f)) continue;
        Mat4 m = Mat4::Translate(pos) * Mat4::RotateY(time * 12.0f);
        DrawMesh(rc, sc.shellDome, m, p.kind == Item_GreenShell ? 0x34C94E : 0xE53B2E);
        DrawMesh(rc, sc.shellRim, m);
    }

    for (int i = 0; i < sc.instanceCount; ++i) {
        const PropInstance& in = sc.instances[i];
        const PropModel&    pm = sc.props[in.kind];
        Vec3                c  = in.pos + Vec3{0, pm.centerY * in.scale, 0};
        if (!fr.SphereVisible(c, pm.radius * in.scale)) continue;
        Mat4 m = Mat4::Translate(in.pos) * Mat4::RotateY(in.yaw) * Mat4::Scale({in.scale, in.scale, in.scale});
        if (pm.lodDist > 0 && pm.lod.vertexCount && Length(c - cam.eye) > pm.lodDist) {
            DrawMesh(rc, pm.lod, m, in.tint);
            continue;
        }
        if (pm.tinted.vertexCount) DrawMesh(rc, pm.tinted, m, in.tint);
        if (pm.fixed.vertexCount) DrawMesh(rc, pm.fixed, m);
    }

    // Start lights follow the countdown: three reds, then green.
    Mat4 g = Mat4::Translate(sc.gantryPos) * Mat4::RotateY(sc.gantryYaw);
    for (int l = 0; l < 3; ++l) {
        u32 col = 0x2A2A2A;
        if (s.phase == Phase_Countdown && s.phaseTicks / kTickRate >= l) col = 0xFF3020;
        if (s.phase == Phase_Racing && s.phaseTicks < 2 * kTickRate) col = 0x30FF60;
        for (float face : {-0.36f, 0.26f}) DrawMesh(rc, sc.lamp, g * Mat4::Translate({-1.2f + 1.2f * (float)l, 4.65f, face}), col);
    }

    DrawGround(rc, sc, fr);  // last: most of it is hidden behind everything above
}

void DrawCharacterPreview(RenderContext& rc, const Scene& sc, int character, float time) {
    Camera  cam{{0, 1.9f, -4.6f}, {0.6f, 0.75f, 0}, 0.85f, 240.0f};
    Frustum fr = MakeFrustum(rc, cam);
    ApplyFog(rc, sc.theme);
    DrawSky(rc, sc.theme, cam);
    ClearDepth(rc.target);
    Mat4 base = Mat4::Translate({0.6f, -0.6f, 0}) * Mat4::RotateY(time * 0.7f + 2.6f);
    DrawMesh(rc, sc.podium, base);
    Mat4 kart = base * Mat4::Translate({0, 0.3f, 0});
    DrawKart(rc, sc, kart, character, kCharacters[Clamp(character, 0, kNumCharacters - 1)].color, time * 2.0f, 0.0f, false, time);
    DrawMesh(rc, sc.shadow, kart * Mat4::Translate({0, 0.02f, 0}));
    DrawGround(rc, sc, fr);
}

void DrawGallery(RenderContext& rc, const Scene& sc, float time) {
    Camera  cam{{0, 3.6f, -8.5f}, {0, 1.4f, 5.0f}, 1.05f, 240.0f};
    Frustum fr = MakeFrustum(rc, cam);
    ApplyFog(rc, sc.theme);
    DrawSky(rc, sc.theme, cam);
    ClearDepth(rc.target);
    const float g = -0.6f;

    // Front row: the whole roster.
    for (int c = 0; c < kNumCharacters; ++c) {
        Mat4 kart = Mat4::Translate({-5.6f + 1.6f * (float)c, g, (c & 1) ? 0.9f : -0.4f}) * Mat4::RotateY(kPi + 0.5f);
        DrawKart(rc, sc, kart, c, kCharacters[c].color, time, 0.0f, false, time);
    }
    // Items.
    for (int s = 0; s < 2; ++s) {
        Mat4 m = Mat4::Translate({-2.2f + 1.3f * (float)s, g, 3.0f}) * Mat4::Scale({1.6f, 1.6f, 1.6f});
        DrawMesh(rc, sc.shellDome, m, s ? 0xE53B2E : 0x34C94E);
        DrawMesh(rc, sc.shellRim, m);
    }
    DrawMesh(rc, sc.banana, Mat4::Translate({0.6f, g, 3.2f}) * Mat4::RotateY(0.6f) * Mat4::Scale({1.6f, 1.6f, 1.6f}));
    DrawMesh(rc, sc.itemBox, Mat4::Translate({2.4f, g + 1.2f, 3.3f}) * Mat4::RotateY(time * 0.6f) * Mat4::RotateX(0.6f),
             Hsv(time * 0.3f, 0.22f, 1.0f));
    // Back row: every scenery model across all themes.
    const struct {
        PropKind kind;
        float    x, z, scale;
        u32      tint;
    } row[] = {
        {Prop_Pine0, -16, 16, 1, 0xFFFFFF},    {Prop_SnowPine0, -11, 17, 1, 0xFFFFFF}, {Prop_Round0, -6, 18, 1, 0xFFFFFF},
        {Prop_Palm1, -1, 18, 1, 0xFFFFFF},     {Prop_Column, 4, 18, 1, 0xFFFFFF},      {Prop_Cactus0, 8.5f, 16, 1, 0xFFFFFF},
        {Prop_Snowman, 12.5f, 14, 1, 0xFFFFFF}, {Prop_Rock0, 15.5f, 12, 1.1f, 0xE0A070}, {Prop_TyreStack, 6, 7, 1, 0xE8433A},
        {Prop_Flag, -6, 9, 1, 0x3A7BE8},
    };
    for (const auto& r : row) {
        const PropModel& pm = sc.props[r.kind];
        Mat4 m = Mat4::Translate({r.x, g, r.z}) * Mat4::RotateY(kPi) * Mat4::Scale({r.scale, r.scale, r.scale});
        if (pm.tinted.vertexCount) DrawMesh(rc, pm.tinted, m, r.tint);
        if (pm.fixed.vertexCount) DrawMesh(rc, pm.fixed, m);
    }
    DrawGround(rc, sc, fr);
}

}  // namespace ss
