// tracks.h - the circuit catalogue. A track is a closed Catmull-Rom loop of
// control points (metres) plus a visual theme; BuildTrack bakes it in fixed
// point so every peer gets the identical track.
#pragma once

#include "core/base.h"

namespace ss {

enum TrackTheme : i32 { Theme_Meadow, Theme_Desert, Theme_Snow, Theme_Vaporwave, Theme_Count };

struct ControlPoint {
    i32 x, z, y;
};

struct TrackDef {
    const char*         name;
    const char*         subtitle;
    i32                 theme;
    const ControlPoint* points;
    i32                 count;
};

constexpr int kTrackCount = 4;
extern const TrackDef kTracks[kTrackCount];

}  // namespace ss
