#include "game/sim.h"
#include "game/tracks.h"

namespace ss {

// Circuits: control points in metres, x (east), z (north), y (height).
// Closed Catmull-Rom loops; sample 0 is the start/finish line. Layouts never
// cross themselves (no bridges) and keep bend radii well above the barrier
// half-width so the barriers never fold over.
namespace {

constexpr ControlPoint kMeadows[] = {
    {0, -100, 0},    {80, -100, 0},  {140, -90, 1},  {172, -50, 3},  {165, 0, 5},     {125, 22, 6},
    {92, 52, 5},     {102, 100, 3},  {62, 142, 1},   {0, 128, 0},    {-50, 150, 2},   {-112, 122, 4},
    {-142, 62, 5},   {-118, 2, 3},   {-150, -58, 1}, {-112, -102, 0}, {-60, -112, 0},
};

// A long climb up the canyon ("number go up") and a fast drop back down.
constexpr ControlPoint kCanyon[] = {
    {0, -150, 0},   {90, -150, 0},   {160, -130, 2},  {190, -80, 5},   {175, -20, 9},   {130, 20, 12},
    {80, 40, 14},   {20, 30, 14},    {-30, 60, 12},   {-60, 110, 9},   {-120, 120, 6},  {-170, 90, 4},
    {-180, 30, 3},  {-150, -30, 2},  {-170, -90, 1},  {-120, -145, 0}, {-60, -155, 0},
};

// Hilly and twisty, with an S-bend in the middle.
constexpr ControlPoint kPeaks[] = {
    {0, -90, 0},   {70, -95, 2},   {120, -70, 5},  {130, -20, 8},  {100, 20, 10},  {50, 15, 8},   {20, 40, 6},
    {40, 90, 7},   {90, 120, 9},   {140, 140, 8},  {150, 190, 6},  {100, 220, 4},  {30, 210, 3},  {-30, 180, 2},
    {-60, 130, 4}, {-110, 110, 6}, {-150, 60, 5},  {-130, 0, 3},   {-90, -30, 2},  {-60, -80, 1},
};

// Long, flowing, nearly flat sweepers under a synthwave sunset.
constexpr ControlPoint kBoulevard[] = {
    {0, -120, 0},  {100, -120, 0}, {170, -90, 0},   {200, -30, 1},  {190, 40, 2},    {140, 90, 3},
    {70, 100, 3},  {30, 70, 2},    {-10, 20, 1},    {-60, 0, 1},    {-120, 20, 2},   {-170, 70, 3},
    {-210, 40, 3}, {-210, -30, 2}, {-170, -90, 1},  {-90, -125, 0},
};

constexpr int kSamplesPerSpan = 20;

Fx CatmullRom(Fx p0, Fx p1, Fx p2, Fx p3, Fx t) {
    Fx t2 = t * t, t3 = t2 * t;
    Fx a  = Fx::FromInt(2) * p1;
    Fx b  = p2 - p0;
    Fx c  = Fx::FromInt(2) * p0 - Fx::FromInt(5) * p1 + Fx::FromInt(4) * p2 - p3;
    Fx d  = -p0 + Fx::FromInt(3) * p1 - Fx::FromInt(3) * p2 + p3;
    return (a + b * t + c * t2 + d * t3) * Fx::Ratio(1, 2);
}

}  // namespace

const TrackDef kTracks[kTrackCount] = {
    {"MEME MEADOWS", "WHERE IT ALL BEGAN", Theme_Meadow, kMeadows, (i32)SS_COUNTOF(kMeadows)},
    {"STONKS CANYON", "NUMBER GO UP. THEN DOWN.", Theme_Desert, kCanyon, (i32)SS_COUNTOF(kCanyon)},
    {"DOGE PEAKS", "VERY COLD. MUCH HILLS.", Theme_Snow, kPeaks, (i32)SS_COUNTOF(kPeaks)},
    {"VAPORWAVE BOULEVARD", "A E S T H E T I C", Theme_Vaporwave, kBoulevard, (i32)SS_COUNTOF(kBoulevard)},
};

void BuildTrack(Track& t, int trackId) {
    const TrackDef& def = kTracks[Clamp(trackId, 0, kTrackCount - 1)];
    t          = {};
    t.id       = Clamp(trackId, 0, kTrackCount - 1);
    t.theme    = def.theme;
    t.roadHalf = Fx::FromInt(7);
    t.wallHalf = Fx::FromInt(11);
    t.count    = def.count * kSamplesPerSpan;
    SS_ASSERT(t.count <= kMaxTrackSamples, "track has too many samples");

    const int n = def.count;
    for (int span = 0; span < n; ++span) {
        const ControlPoint& c0 = def.points[(span + n - 1) % n];
        const ControlPoint& c1 = def.points[span];
        const ControlPoint& c2 = def.points[(span + 1) % n];
        const ControlPoint& c3 = def.points[(span + 2) % n];
        for (int k = 0; k < kSamplesPerSpan; ++k) {
            Fx           u = Fx::Ratio(k, kSamplesPerSpan);
            TrackSample& s = t.s[span * kSamplesPerSpan + k];
            s.pos.x = CatmullRom(Fx::FromInt(c0.x), Fx::FromInt(c1.x), Fx::FromInt(c2.x), Fx::FromInt(c3.x), u);
            s.pos.z = CatmullRom(Fx::FromInt(c0.z), Fx::FromInt(c1.z), Fx::FromInt(c2.z), Fx::FromInt(c3.z), u);
            s.y     = CatmullRom(Fx::FromInt(c0.y), Fx::FromInt(c1.y), Fx::FromInt(c2.y), Fx::FromInt(c3.y), u);
        }
    }

    t.totalLength = Fx{0};
    for (int i = 0; i < t.count; ++i) {
        TrackSample& s = t.s[i];
        V2           d = t.s[(i + 1) % t.count].pos - s.pos;
        s.length       = Length(d);
        s.tangent      = Normalize(d);
        s.right        = RightOf(s.tangent);
        t.totalLength += s.length;
    }
    for (int i = 0; i < t.count; ++i) t.s[i].curvature = Cross(t.s[i].tangent, t.s[(i + 4) % t.count].tangent);

    static constexpr i32 kRowPercent[4] = {12, 37, 60, 83};
    for (int r = 0; r < 4; ++r) t.itemRows[r] = t.count * kRowPercent[r] / 100;
}
TrackPos LocateOnTrack(const Track& t, V2 pos, i32 hintSeg) {
    i32 seg = WrapSeg(t, hintSeg);
    // Walk segment-by-segment from the hint. On the outside of a bend there
    // is a wedge that belongs to neither neighbour; we stay put rather than
    // ping-pong, so the loop is bounded and the answer is stable.
    for (int it = 0; it < 32; ++it) {
        const TrackSample& s     = t.s[seg];
        Fx                 along = Dot(pos - s.pos, s.tangent);
        if (along < Fx{0}) {
            i32 prev = WrapSeg(t, seg - 1);
            if (Dot(pos - t.s[prev].pos, t.s[prev].tangent) < t.s[prev].length) {
                seg = prev;
                continue;
            }
        } else if (along >= s.length) {
            i32 next = WrapSeg(t, seg + 1);
            if (Dot(pos - t.s[next].pos, t.s[next].tangent) >= Fx{0}) {
                seg = next;
                continue;
            }
        }
        break;
    }
    const TrackSample& s = t.s[seg];
    V2                 d = pos - s.pos;
    return {seg, Dot(d, s.tangent), Dot(d, s.right)};
}

Fx TrackHeight(const Track& t, const TrackPos& tp) {
    const TrackSample& a = t.s[tp.seg];
    const TrackSample& b = t.s[WrapSeg(t, tp.seg + 1)];
    Fx                 f = Clamp(tp.along / a.length, Fx{0}, Fx::FromInt(1));
    return FxLerp(a.y, b.y, f);
}

Angle AngleOf(V2 dir) {
    // CORDIC vectoring: rotate the vector onto the +z axis, summing the
    // rotations. Integer-only, so headings derived from it are deterministic.
    static constexpr i32 kAtan[] = {8192, 4836, 2555, 1297, 651, 326, 163, 81, 41, 20, 10, 5, 3, 1, 1};
    i64   x = dir.z.raw, y = dir.x.raw;
    Angle a = 0;
    if (x == 0 && y == 0) return 0;
    if (x < 0) {
        x = -x;
        y = -y;
        a = kAngleTurn / 2;
    }
    for (int i = 0; i < (int)SS_COUNTOF(kAtan); ++i) {
        i64 nx;
        if (y > 0) {
            nx = x + (y >> i);
            y  = y - (x >> i);
            a += kAtan[i];
        } else {
            nx = x - (y >> i);
            y  = y + (x >> i);
            a -= kAtan[i];
        }
        x = nx;
    }
    return WrapAngle(a);
}

}  // namespace ss
