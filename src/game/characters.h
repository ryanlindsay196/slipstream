// characters.h - the roster. Original characters riffing on meme ideas.
//
// Stats are fixed point and live in the simulation (they change how a kart
// drives), so every peer applies them identically. Colours and taglines are
// presentation data that happen to sit in the same table.
#pragma once

#include "core/base.h"

namespace ss {

enum CharacterId : i32 {
    Char_Doggo,
    Char_Stonks,
    Char_Moai,
    Char_GalaxyBrain,
    Char_VibeCat,
    Char_Honk,
    Char_RaveCrab,
    Char_Banana,
    Char_Count
};

struct CharacterDef {
    const char* name;
    const char* tagline;
    u32         color;      // kart paint
    Fx          topSpeed;   // multiplier on the base top speed
    Fx          accel;      // metres per tick per tick
    i32         turnRate;   // binary angle units per tick at full lock
    Fx          weight;     // relative mass in kart-to-kart bumps
    u8          barSpeed, barAccel, barHandling, barWeight;  // 1..5, for the select screen
};

inline constexpr CharacterDef kCharacters[Char_Count] = {
    {"DOGGO", "SUCH SPEED. VERY KART. WOW.", 0xF28A2E, Fx::Ratio(100, 100), Fx::Ratio(60, 10000), 380, Fx::Ratio(100, 100), 3, 3, 3, 3},
    {"STONKS", "NUMBER GO UP.", 0x3CC25A, Fx::Ratio(103, 100), Fx::Ratio(50, 10000), 360, Fx::Ratio(110, 100), 5, 2, 2, 3},
    {"MOAI", "STONE COLD. LITERALLY.", 0x7A8794, Fx::Ratio(104, 100), Fx::Ratio(45, 10000), 335, Fx::Ratio(160, 100), 5, 1, 1, 5},
    {"GALAXY BRAIN", "ASCENDED RACING LINE.", 0x9B59D9, Fx::Ratio(98, 100), Fx::Ratio(65, 10000), 430, Fx::Ratio(90, 100), 2, 4, 5, 2},
    {"VIBE CAT", "JUST VIBING.", 0x2EC9C9, Fx::Ratio(98, 100), Fx::Ratio(75, 10000), 400, Fx::Ratio(80, 100), 2, 5, 4, 1},
    {"HONK", "HONK. HONK HONK.", 0x3A7BE8, Fx::Ratio(101, 100), Fx::Ratio(60, 10000), 390, Fx::Ratio(120, 100), 4, 3, 3, 4},
    {"RAVE CRAB", "SIDEWAYS IS FASTER.", 0xE8433A, Fx::Ratio(99, 100), Fx::Ratio(65, 10000), 410, Fx::Ratio(100, 100), 3, 4, 4, 3},
    {"BANANA", "FOR SCALE.", 0xF2C230, Fx::Ratio(97, 100), Fx::Ratio(75, 10000), 420, Fx::Ratio(70, 100), 1, 5, 5, 1},
};

}  // namespace ss
