// modeling.h - a small procedural modelling kit.
//
// Every model in the game is generated at startup from these primitives. No
// files, no importer. Builders work in a scratch arena and copy the finished
// mesh into the persistent arena at exactly its final size.
//
//   Lathe   spin a 2D profile (radius, height) around the Y axis
//   Extrude push a 2D outline (z, y) across X; caps are ear-clipped
//   Sweep   a tube along a 3D path with per-ring radius and colour
//   Box / Tri / Quad / Cone  for hard-edged details
//
// Lighting is baked into vertex colours from smooth or faceted normals. All
// geometry passes through the builder's current transform `xf`, so parts can
// be authored in their own local space and placed with a matrix.
#pragma once

#include "render/math3d.h"
#include "render/raster.h"

namespace ss {

struct Arena;

struct Vec2 {
    float x, y;
};

// A profile point for Lathe: radius, height and colour. Repeat a point to make
// a hard crease (normals on either side are computed independently).
struct ProfilePoint {
    float r, y;
    u32   color;
};

struct MeshBuilder {
    Vertex* v;
    u16*    idx;
    Vec3*   nrm;  // world-space vertex normals, used to choose triangle winding
    int     vc, ic, vcap, icap;
    Mat4    xf;
    bool    lit;

    void Init(Arena& work, int maxVerts = 16384, int maxIndices = 49152);
    Mesh Finish(Arena& dest, const Texture* tex, u8 flags = 0) const;  // exact-size copy into dest

    u16  AddVertex(Vec3 p, Vec3 n, float u, float v, u32 albedo);  // applies xf and lighting
    void AddTri(u16 a, u16 b, u16 c);
    // Adds the triangle wound so its geometric normal agrees with `facing`.
    void AddTriFacing(u16 a, u16 b, u16 c, Vec3 facing);

    // Hard-edged primitives (faceted normals).
    void Quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, u32 color, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1);
    void Tri(Vec3 a, Vec3 b, Vec3 c, u32 color);
    void Box(Vec3 mn, Vec3 mx, u32 color);
    void Cone(Vec3 base, float radius, float height, int sides, u32 color);

    // Smooth-shaded surfaces.
    void Lathe(const ProfilePoint* profile, int count, int segments, float jitter = 0.0f, u32 seed = 0);
    void Extrude(const Vec2* outline, int count, float x0, float x1, u32 color);
    void Sweep(const Vec3* path, const float* radii, const u32* colors, int count, int sides, bool capEnds);
    void Torus(float majorRadius, float minorRadius, int segments, int sides, u32 color);
};

extern const Vec3 kSunDir;
extern float      g_lightLevel;  // scales baked lighting (e.g. dimmer for night tracks)
u32 ShadeColor(u32 albedo, Vec3 normal);

}  // namespace ss
