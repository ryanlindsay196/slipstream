// input.h - action mapping and rebinding.
//
// The platform reports raw devices (platform-neutral key codes, XInput-style
// pads). Gameplay only ever sees *actions*, each with one keyboard and one
// controller binding, saved to a plain-text file that is portable across
// platforms because it stores neutral names ("key:W pad:A"), not OS codes.
// Menu navigation is deliberately not rebindable, so a bad binding can never
// lock the player out of the controls screen.
#pragma once

#include "game/sim.h"
#include "platform/platform.h"

namespace ss {

struct Arena;

enum Action : u8 {
    Act_Accelerate,
    Act_Brake,
    Act_SteerLeft,
    Act_SteerRight,
    Act_Drift,
    Act_UseItem,
    Act_LookBack,
    Act_Pause,
    Act_Count
};
extern const char* kActionNames[Act_Count];

enum BindKind : u8 { Bind_None, Bind_Key, Bind_PadButton, Bind_PadAxisPos, Bind_PadAxisNeg };

struct Binding {
    u8  kind;
    u16 code;  // plat::Key, plat::PadButton or plat::PadAxis
};

enum BindSlot { Slot_Keyboard, Slot_Pad, Slot_Count };

struct Bindings {
    Binding slots[Act_Count][Slot_Count];
};

void        DefaultBindings(Bindings& b);
float       ActionValue(const Bindings& b, Action a, const plat::InputFrame& in);  // 0..1
PlayerInput SampleRaceInput(const Bindings& b, const plat::InputFrame& in);
void        BindingName(Binding bind, char* out, int cap);

// Rebinding: returns true and fills *out when a fresh press on the slot's
// device type is seen between prev and now.
bool CaptureBinding(BindSlot slot, const plat::InputFrame& now, const plat::InputFrame& prev, Binding* out);
bool AnyInputHeld(const plat::InputFrame& in);
// Assigns `bind` to (action, slot) and clears the same binding from any other
// action, so one key can't silently drive two actions.
void AssignBinding(Bindings& b, Action a, BindSlot slot, Binding bind);

bool SaveBindings(const Bindings& b, const char* path, Arena& scratch);
bool LoadBindings(Bindings& b, const char* path, Arena& scratch);  // keeps defaults for missing lines

// Fixed menu navigation (keyboard arrows/Enter/Esc, d-pad/stick/A/B/Start).
struct MenuInput {
    bool up, down, left, right, confirm, back;
};
MenuInput ReadMenuInput(const plat::InputFrame& now, const plat::InputFrame& prev);

}  // namespace ss
