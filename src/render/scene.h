// scene.h - builds all meshes/textures for a track (procedurally, see
// modeling.h), then draws the world for a given sim state and camera.
// Purely a consumer of SimState. Rebuilt whenever the track changes.
#pragma once

#include "render/math3d.h"
#include "render/raster.h"

namespace ss {

struct Arena;
struct Track;
struct SimState;

constexpr int kMaxChunks      = 64;
constexpr int kMaxGroundTiles = 64;
constexpr int kMaxProps       = 512;
constexpr int kNumCharacters  = 8;

struct TrackChunk {
    Mesh  road, curb, verge, wall;
    Vec3  center;
    float radius;
};

// A kind of trackside prop: an optional tinted mesh (per-instance colour), an
// optional fixed-colour mesh, and an optional cheap stand-in for distance.
enum PropKind : u8 {
    Prop_Pine0, Prop_Pine1, Prop_Pine2,
    Prop_Round0, Prop_Round1,
    Prop_Rock0, Prop_Rock1,
    Prop_Cactus0, Prop_Cactus1, Prop_Mesa,
    Prop_SnowPine0, Prop_SnowPine1, Prop_Snowman,
    Prop_Palm0, Prop_Palm1, Prop_Column,
    Prop_TyreStack,
    Prop_ChevronLeft, Prop_ChevronRight,
    Prop_Flag,
    Prop_Grandstand,
    Prop_Gantry,
    Prop_KindCount
};

struct PropModel {
    Mesh  tinted, fixed, lod;
    float radius;   // bounding sphere radius at scale 1, centred `centerY` up
    float centerY;
    float lodDist;  // switch to `lod` beyond this distance (0 = never)
};

struct PropInstance {
    u8    kind;
    Vec3  pos;
    float yaw;
    float scale;
    u32   tint;
};

// Per-track look: sky, fog, ground, scenery.
struct SceneTheme {
    u32   skyTop, skyHorizon, mountainNear, mountainFar, fog;
    float fogStart, fogEnd;
    bool  sunset;  // striped synthwave sun on the horizon
};

struct Scene {
    SceneTheme theme;
    Texture    texAsphalt, texGround, texWall, texChecker, texItemBox;

    TrackChunk chunks[kMaxChunks];
    int        chunkCount;
    Mesh       groundTiles[kMaxGroundTiles];
    Vec3       groundCenters[kMaxGroundTiles];
    float      groundRadius;
    int        groundCount;
    Mesh       startLine;

    PropModel    props[Prop_KindCount];
    PropInstance instances[kMaxProps];
    int          instanceCount;
    Vec3         gantryPos;
    float        gantryYaw;

    // Karts: tinted chassis, fixed-colour parts, spinning wheels, one driver
    // model per character, and a low-detail stand-in for distant karts.
    Mesh kartBody, kartParts, wheelFront, wheelRear, kartLowTinted, kartLowFixed;
    Mesh characters[kNumCharacters];
    Mesh itemBox, shellDome, shellRim, banana, shadow, spark, lamp, podium;
};

// Presentation-only per-kart animation state (wheel spin, steering angle).
// Owned by the caller so the scene itself stays immutable.
struct KartVisuals {
    float wheelAngle[8];
    float steer[8];
    i32   lastHeading[8];
    i32   lastTick;
    bool  valid;
};

struct Camera {
    Vec3  eye;
    Vec3  target;
    float fovY;
    float farDist;
};

// Builds everything for `track` into `persistent` (use a per-level arena and
// reset it to switch tracks). `scratch` is working memory.
void BuildScene(Scene& scene, const Track& track, Arena& persistent, Arena& scratch);

// Draws sky, terrain, track, props, items and karts.
void DrawWorld(RenderContext& rc, const Scene& scene, const SimState& s, const Camera& cam, float timeSeconds,
               KartVisuals* visuals);

// Character select: one kart + driver on a turntable podium.
void DrawCharacterPreview(RenderContext& rc, const Scene& scene, int character, float timeSeconds);

// Model viewer: every generated model lined up in front of a fixed camera.
void DrawGallery(RenderContext& rc, const Scene& scene, float timeSeconds);

Mat4 CameraViewProj(const Camera& cam, float aspect, Mat4* outView = nullptr);
inline float AngleToRadians(i32 a) { return (float)a * (2.0f * kPi / 65536.0f); }

}  // namespace ss
