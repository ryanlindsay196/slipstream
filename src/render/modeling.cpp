#include "render/modeling.h"
#include "core/memory.h"

namespace ss {

const Vec3 kSunDir     = Normalize(Vec3{0.45f, 0.8f, -0.35f});
float      g_lightLevel = 1.0f;

u32 ShadeColor(u32 albedo, Vec3 n) {
    // Sun (Lambert) plus a sky term that brightens upward-facing surfaces.
    float sun = Dot(n, kSunDir);
    float sky = 0.5f + 0.5f * n.y;
    return MulColor(albedo, g_lightLevel * (0.40f + 0.18f * sky + 0.45f * (sun > 0 ? sun : 0)));
}

namespace {

Vec3 RotateDir(const Mat4& m, Vec3 d) {
    return Normalize(Vec3{m.m[0][0] * d.x + m.m[0][1] * d.y + m.m[0][2] * d.z,
                          m.m[1][0] * d.x + m.m[1][1] * d.y + m.m[1][2] * d.z,
                          m.m[2][0] * d.x + m.m[2][1] * d.y + m.m[2][2] * d.z});
}

float Cross2(Vec2 o, Vec2 a, Vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

bool InTriangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c) {
    return Cross2(a, b, p) > 0 && Cross2(b, c, p) > 0 && Cross2(c, a, p) > 0;
}

}  // namespace

void MeshBuilder::Init(Arena& work, int maxVerts, int maxIndices) {
    SS_ASSERT(maxVerts <= 65536, "u16 indices: at most 65536 vertices per mesh");
    v    = work.Push<Vertex>((size_t)maxVerts);
    idx  = work.Push<u16>((size_t)maxIndices);
    nrm  = work.Push<Vec3>((size_t)maxVerts);
    vc = ic = 0;
    vcap    = maxVerts;
    icap    = maxIndices;
    xf      = Mat4::Identity();
    lit     = true;
}

Mesh MeshBuilder::Finish(Arena& dest, const Texture* tex, u8 flags) const {
    Vertex* verts = dest.Push<Vertex>((size_t)Max(vc, 1));
    u16*    ind   = dest.Push<u16>((size_t)Max(ic, 1));
    memcpy(verts, v, sizeof(Vertex) * (size_t)vc);
    memcpy(ind, idx, sizeof(u16) * (size_t)ic);
    return {verts, ind, vc, ic, tex, flags};
}

u16 MeshBuilder::AddVertex(Vec3 p, Vec3 n, float u, float vv, u32 albedo) {
    SS_ASSERT(vc < vcap, "mesh builder vertex capacity");
    Vec3 wn       = RotateDir(xf, n);
    nrm[vc] = wn;
    v[vc]         = {TransformPoint(xf, p), u, vv, lit ? ShadeColor(albedo, wn) : albedo};
    return (u16)vc++;
}

void MeshBuilder::AddTri(u16 a, u16 b, u16 c) {
    SS_ASSERT(ic + 3 <= icap, "mesh builder index capacity");
    idx[ic++] = a;
    idx[ic++] = b;
    idx[ic++] = c;
}

void MeshBuilder::AddTriFacing(u16 a, u16 b, u16 c, Vec3 facing) {
    Vec3 geo = Cross(v[b].pos - v[a].pos, v[c].pos - v[a].pos);
    if (Dot(geo, facing) >= 0) AddTri(a, b, c);
    else AddTri(a, c, b);
}

void MeshBuilder::Quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, u32 color, float u0, float v0, float u1, float v1) {
    // Faceted: the face normal comes from the (transformed) geometry.
    Vec3 wa = TransformPoint(xf, a), wb = TransformPoint(xf, b), wd = TransformPoint(xf, d);
    Vec3 n  = Normalize(Cross(wb - wa, wd - wa));
    Mat4 keep = xf;
    xf        = Mat4::Identity();
    Vec3 wc   = TransformPoint(keep, c);
    u16  i0 = AddVertex(wa, n, u0, v0, color), i1 = AddVertex(wb, n, u0, v1, color);
    u16  i2 = AddVertex(wc, n, u1, v1, color), i3 = AddVertex(wd, n, u1, v0, color);
    xf = keep;
    AddTri(i0, i1, i2);
    AddTri(i0, i2, i3);
}

void MeshBuilder::Tri(Vec3 a, Vec3 b, Vec3 c, u32 color) {
    Vec3 wa = TransformPoint(xf, a), wb = TransformPoint(xf, b), wc = TransformPoint(xf, c);
    Vec3 n  = Normalize(Cross(wb - wa, wc - wa));
    Mat4 keep = xf;
    xf        = Mat4::Identity();
    u16 i0 = AddVertex(wa, n, 0, 0, color), i1 = AddVertex(wb, n, 0, 1, color), i2 = AddVertex(wc, n, 1, 1, color);
    xf = keep;
    AddTri(i0, i1, i2);
}

void MeshBuilder::Box(Vec3 mn, Vec3 mx, u32 color) {
    float x0 = mn.x, y0 = mn.y, z0 = mn.z, x1 = mx.x, y1 = mx.y, z1 = mx.z;
    Quad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, color);  // +x
    Quad({x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0}, color);  // -x
    Quad({x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y0, z1}, color);  // +z
    Quad({x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}, color);  // -z
    Quad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, color);  // +y
    Quad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, color);  // -y
}

void MeshBuilder::Cone(Vec3 base, float radius, float height, int sides, u32 color) {
    Vec3 apex = base + Vec3{0, height, 0};
    for (int k = 0; k < sides; ++k) {
        float a0 = 2 * kPi * (float)k / (float)sides, a1 = 2 * kPi * (float)(k + 1) / (float)sides;
        Vec3  p0 = base + Vec3{std::sin(a0) * radius, 0, std::cos(a0) * radius};
        Vec3  p1 = base + Vec3{std::sin(a1) * radius, 0, std::cos(a1) * radius};
        Tri(p0, p1, apex, color);
    }
}

void MeshBuilder::Lathe(const ProfilePoint* p, int count, int segments, float jitter, u32 seed) {
    auto same = [&](int a, int b) { return p[a].r == p[b].r && p[a].y == p[b].y; };
    int  first = vc;
    for (int i = 0; i < count; ++i) {
        // Profile normal from the neighbours on *this* side of any crease.
        int   prev = (i > 0 && !same(i - 1, i)) ? i - 1 : i;
        int   next = (i < count - 1 && !same(i + 1, i)) ? i + 1 : i;
        float dr = p[next].r - p[prev].r, dy = p[next].y - p[prev].y;
        float len = std::sqrt(dr * dr + dy * dy);
        float nr = len > 0 ? dy / len : 1.0f, ny = len > 0 ? -dr / len : 0.0f;
        for (int k = 0; k < segments; ++k) {
            float a = 2 * kPi * (float)k / (float)segments;
            float r = p[i].r;
            if (jitter > 0) r *= 1.0f + jitter * ((float)(HashU32(seed ^ (u32)(i * 977 + k * 131)) % 1000) / 1000.0f - 0.5f);
            AddVertex({r * std::sin(a), p[i].y, r * std::cos(a)}, {nr * std::sin(a), ny, nr * std::cos(a)}, 0, 0,
                      p[i].color);
        }
    }
    for (int i = 0; i + 1 < count; ++i) {
        if (same(i, i + 1)) continue;  // crease: zero-area band
        for (int k = 0; k < segments; ++k) {
            u16  a = (u16)(first + i * segments + k), b = (u16)(first + (i + 1) * segments + k);
            u16  c = (u16)(first + (i + 1) * segments + (k + 1) % segments), d = (u16)(first + i * segments + (k + 1) % segments);
            Vec3 face = nrm[a] + nrm[b] + nrm[c] + nrm[d];
            AddTriFacing(a, b, c, face);
            AddTriFacing(a, c, d, face);
        }
    }
}

void MeshBuilder::Extrude(const Vec2* outlineIn, int count, float x0, float x1, u32 color) {
    SS_ASSERT(count >= 3 && count <= 64, "extrude outline size");
    Vec2  o[64];
    float area = 0;
    for (int i = 0; i < count; ++i) area += outlineIn[i].x * outlineIn[(i + 1) % count].y - outlineIn[(i + 1) % count].x * outlineIn[i].y;
    for (int i = 0; i < count; ++i) o[i] = area >= 0 ? outlineIn[i] : outlineIn[count - 1 - i];  // force CCW

    // Sides: one flat quad per edge.
    for (int i = 0; i < count; ++i) {
        Vec2 a = o[i], b = o[(i + 1) % count];
        Vec3 n = Normalize(Vec3{0, -(b.x - a.x), b.y - a.y});  // outward for a CCW outline in (z, y)
        u16  i0 = AddVertex({x0, a.y, a.x}, n, 0, 0, color), i1 = AddVertex({x1, a.y, a.x}, n, 0, 1, color);
        u16  i2 = AddVertex({x1, b.y, b.x}, n, 1, 1, color), i3 = AddVertex({x0, b.y, b.x}, n, 1, 0, color);
        Vec3 f  = nrm[i0];
        AddTriFacing(i0, i1, i2, f);
        AddTriFacing(i0, i2, i3, f);
    }

    // Caps: ear clipping (outline may be concave).
    int capBase[2];
    for (int side = 0; side < 2; ++side) {
        float x = side ? x1 : x0;
        Vec3  n{side ? 1.0f : -1.0f, 0, 0};
        capBase[side] = vc;
        for (int i = 0; i < count; ++i) AddVertex({x, o[i].y, o[i].x}, n, 0, 0, color);
    }
    int ring[64], n = count;
    for (int i = 0; i < count; ++i) ring[i] = i;
    for (int guard = 0; n > 3 && guard < 4096; ++guard) {
        bool clipped = false;
        for (int i = 0; i < n && !clipped; ++i) {
            int  ia = ring[(i + n - 1) % n], ib = ring[i], ic_ = ring[(i + 1) % n];
            if (Cross2(o[ia], o[ib], o[ic_]) <= 0) continue;  // reflex vertex
            bool empty = true;
            for (int j = 0; j < n && empty; ++j) {
                int k = ring[j];
                if (k != ia && k != ib && k != ic_ && InTriangle(o[k], o[ia], o[ib], o[ic_])) empty = false;
            }
            if (!empty) continue;
            for (int side = 0; side < 2; ++side) {
                u16 a = (u16)(capBase[side] + ia), b = (u16)(capBase[side] + ib), c = (u16)(capBase[side] + ic_);
                AddTriFacing(a, b, c, nrm[a]);
            }
            for (int j = i; j < n - 1; ++j) ring[j] = ring[j + 1];
            --n;
            clipped = true;
        }
        if (!clipped) break;  // degenerate outline; leave the remainder open
    }
    if (n == 3)
        for (int side = 0; side < 2; ++side) {
            u16 a = (u16)(capBase[side] + ring[0]), b = (u16)(capBase[side] + ring[1]), c = (u16)(capBase[side] + ring[2]);
            AddTriFacing(a, b, c, nrm[a]);
        }
}

void MeshBuilder::Sweep(const Vec3* path, const float* radii, const u32* colors, int count, int sides, bool capEnds) {
    SS_ASSERT(count >= 2, "sweep needs at least two points");
    int  first = vc;
    Vec3 frame{0, 0, 0}, tan0{0, 0, 0}, tanN{0, 0, 0};
    for (int j = 0; j < count; ++j) {
        Vec3 t = Normalize(path[Min(j + 1, count - 1)] - path[Max(j - 1, 0)]);
        if (j == 0) {
            Vec3 ref = std::fabs(t.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
            frame    = Normalize(Cross(t, ref));
            tan0     = t;
        } else {
            frame = Normalize(frame - t * Dot(frame, t));  // parallel transport: no twisting
        }
        tanN   = t;
        Vec3 b = Cross(t, frame);
        for (int k = 0; k < sides; ++k) {
            float a   = 2 * kPi * (float)k / (float)sides;
            Vec3  dir = frame * std::cos(a) + b * std::sin(a);
            AddVertex(path[j] + dir * radii[j], dir, 0, 0, colors ? colors[j] : 0xFFFFFF);
        }
    }
    for (int j = 0; j + 1 < count; ++j)
        for (int k = 0; k < sides; ++k) {
            u16  a = (u16)(first + j * sides + k), b = (u16)(first + (j + 1) * sides + k);
            u16  c = (u16)(first + (j + 1) * sides + (k + 1) % sides), d = (u16)(first + j * sides + (k + 1) % sides);
            Vec3 face = nrm[a] + nrm[b] + nrm[c] + nrm[d];
            AddTriFacing(a, b, c, face);
            AddTriFacing(a, c, d, face);
        }
    if (!capEnds) return;
    for (int end = 0; end < 2; ++end) {
        int  j  = end ? count - 1 : 0;
        if (radii[j] <= 0) continue;
        Vec3 n  = end ? tanN : tan0 * -1.0f;
        u32  cl = colors ? colors[j] : 0xFFFFFF;
        // Recover world positions of this ring from the stored vertices.
        Mat4 keep = xf;
        xf        = Mat4::Identity();
        Vec3 nW   = RotateDir(keep, n);
        Vec3 ctr{0, 0, 0};
        for (int k = 0; k < sides; ++k) ctr = ctr + v[first + j * sides + k].pos;
        u16 c = AddVertex(ctr * (1.0f / (float)sides), nW, 0, 0, cl);
        int ringStart = vc;
        for (int k = 0; k < sides; ++k) AddVertex(v[first + j * sides + k].pos, nW, 0, 0, cl);
        xf = keep;
        for (int k = 0; k < sides; ++k)
            AddTriFacing(c, (u16)(ringStart + k), (u16)(ringStart + (k + 1) % sides), nW);
    }
}

void MeshBuilder::Torus(float R, float r, int segments, int sides, u32 color) {
    ProfilePoint prof[33];
    sides = Min(sides, 32);
    for (int i = 0; i <= sides; ++i) {
        float a = 2 * kPi * (float)i / (float)sides - kPi * 0.5f;
        prof[i] = {R + r * std::cos(a), r * std::sin(a), color};
    }
    Lathe(prof, sides + 1, segments);
}

}  // namespace ss
