#include "app/input.h"
#include "core/memory.h"

#include <cstdio>
#include <initializer_list>

namespace ss {

using namespace plat;

const char* kActionNames[Act_Count] = {"ACCELERATE", "BRAKE", "STEER_LEFT", "STEER_RIGHT",
                                       "DRIFT",      "USE_ITEM", "LOOK_BACK", "PAUSE"};

namespace {

const char* kKeyNames[Key_Count] = {
    "NONE", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
    "T", "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "UP", "DOWN",
    "LEFT", "RIGHT", "SPACE", "ENTER", "ESCAPE", "BACKSPACE", "DELETE", "TAB", "LSHIFT", "RSHIFT", "LCTRL",
    "RCTRL", "LALT", "RALT", "COMMA", "PERIOD", "SLASH", "SEMICOLON", "MINUS", "EQUALS", "F1", "F2", "F3",
    "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
};
static_assert(SS_COUNTOF(kKeyNames) == Key_Count, "key name table out of sync");

const char* kPadButtonNames[Pad_ButtonCount] = {"A",      "B",      "X",       "Y",         "LB",
                                                "RB",     "BACK",   "START",   "LSTICK",    "RSTICK",
                                                "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"};
const char* kPadAxisNames[Axis_Count]        = {"LX", "LY", "RX", "RY", "LT", "RT"};

constexpr float kAxisPress = 0.55f;  // digital threshold for axes

float AxisValue(const InputFrame& in, u16 axis, bool positive) {
    float best = 0;
    for (const GamepadState& p : in.pads) {
        if (!p.connected) continue;
        float v = (float)p.axes[axis] / 32767.0f;
        if (!positive) v = -v;
        if (axis == Axis_LT || axis == Axis_RT) v = v < 0.08f ? 0 : v;  // trigger deadzone
        best = Max(best, v);
    }
    return best;
}

bool PadButtonDown(const InputFrame& in, u16 button) {
    for (const GamepadState& p : in.pads)
        if (p.connected && (p.buttons & (1u << button))) return true;
    return false;
}

float BindingValue(Binding b, const InputFrame& in) {
    switch (b.kind) {
        case Bind_Key:        return in.keys[b.code] ? 1.0f : 0.0f;
        case Bind_PadButton:  return PadButtonDown(in, b.code) ? 1.0f : 0.0f;
        case Bind_PadAxisPos: return AxisValue(in, b.code, true);
        case Bind_PadAxisNeg: return AxisValue(in, b.code, false);
    }
    return 0;
}

bool ParseBinding(const char* name, BindSlot slot, Binding* out) {
    if (!strcmp(name, "NONE")) {
        *out = {Bind_None, 0};
        return true;
    }
    if (slot == Slot_Keyboard) {
        for (u16 k = 1; k < Key_Count; ++k)
            if (!strcmp(name, kKeyNames[k])) { *out = {Bind_Key, k}; return true; }
        return false;
    }
    for (u16 b = 0; b < Pad_ButtonCount; ++b)
        if (!strcmp(name, kPadButtonNames[b])) { *out = {Bind_PadButton, b}; return true; }
    for (u16 a = 0; a < Axis_Count; ++a) {
        size_t n = strlen(kPadAxisNames[a]);
        if (strncmp(name, kPadAxisNames[a], n) != 0) continue;
        if (name[n] == 0 || (name[n] == '+' && name[n + 1] == 0)) { *out = {Bind_PadAxisPos, a}; return true; }
        if (name[n] == '-' && name[n + 1] == 0) { *out = {Bind_PadAxisNeg, a}; return true; }
    }
    return false;
}

}  // namespace

void DefaultBindings(Bindings& b) {
    b = {};
    auto set = [&](Action a, Key key, Binding pad) {
        b.slots[a][Slot_Keyboard] = {Bind_Key, key};
        b.slots[a][Slot_Pad]      = pad;
    };
    set(Act_Accelerate, Key_W, {Bind_PadButton, Pad_A});
    set(Act_Brake, Key_S, {Bind_PadButton, Pad_B});
    set(Act_SteerLeft, Key_A, {Bind_PadAxisNeg, Axis_LX});
    set(Act_SteerRight, Key_D, {Bind_PadAxisPos, Axis_LX});
    set(Act_Drift, Key_Space, {Bind_PadButton, Pad_RB});
    set(Act_UseItem, Key_E, {Bind_PadButton, Pad_LB});
    set(Act_LookBack, Key_Q, {Bind_PadButton, Pad_X});
    set(Act_Pause, Key_Escape, {Bind_PadButton, Pad_Start});
}

float ActionValue(const Bindings& b, Action a, const InputFrame& in) {
    return Max(BindingValue(b.slots[a][Slot_Keyboard], in), BindingValue(b.slots[a][Slot_Pad], in));
}

PlayerInput SampleRaceInput(const Bindings& b, const InputFrame& in) {
    PlayerInput p{};
    float       steer = ActionValue(b, Act_SteerRight, in) - ActionValue(b, Act_SteerLeft, in);
    p.steer           = (i8)Clamp((int)(steer * 127.0f), -127, 127);
    auto digital      = [&](Action a, u8 bit) {
        if (ActionValue(b, a, in) >= kAxisPress) p.buttons |= bit;
    };
    digital(Act_Accelerate, In_Accel);
    digital(Act_Brake, In_Brake);
    digital(Act_Drift, In_Drift);
    digital(Act_UseItem, In_Item);
    digital(Act_LookBack, In_LookBack);
    return p;
}

void BindingName(Binding bind, char* out, int cap) {
    switch (bind.kind) {
        case Bind_Key:        snprintf(out, (size_t)cap, "%s", kKeyNames[bind.code]); break;
        case Bind_PadButton:  snprintf(out, (size_t)cap, "%s", kPadButtonNames[bind.code]); break;
        case Bind_PadAxisPos:
            snprintf(out, (size_t)cap, (bind.code == Axis_LT || bind.code == Axis_RT) ? "%s" : "%s+",
                     kPadAxisNames[bind.code]);
            break;
        case Bind_PadAxisNeg: snprintf(out, (size_t)cap, "%s-", kPadAxisNames[bind.code]); break;
        default:              snprintf(out, (size_t)cap, "NONE"); break;
    }
}

bool AnyInputHeld(const InputFrame& in) {
    for (int k = 1; k < Key_Count; ++k)
        if (in.keys[k]) return true;
    for (const GamepadState& p : in.pads) {
        if (!p.connected) continue;
        if (p.buttons) return true;
        for (i16 a : p.axes)
            if (a > 9830 || a < -9830) return true;  // 30%
    }
    return false;
}

bool CaptureBinding(BindSlot slot, const InputFrame& now, const InputFrame& prev, Binding* out) {
    if (slot == Slot_Keyboard) {
        for (u16 k = 1; k < Key_Count; ++k) {
            if (k == Key_Escape || k == Key_Backspace || k == Key_Delete) continue;  // reserved for cancel/clear
            if (now.keys[k] && !prev.keys[k]) { *out = {Bind_Key, k}; return true; }
        }
        return false;
    }
    for (int p = 0; p < kMaxPads; ++p) {
        const GamepadState& a = now.pads[p];
        const GamepadState& b = prev.pads[p];
        if (!a.connected) continue;
        for (u16 btn = 0; btn < Pad_ButtonCount; ++btn) {
            if (btn == Pad_Back) continue;  // reserved for cancel
            u32 bit = 1u << btn;
            if ((a.buttons & bit) && !(b.buttons & bit)) { *out = {Bind_PadButton, btn}; return true; }
        }
        const i16 press = (i16)(kAxisPress * 32767.0f);
        for (u16 ax = 0; ax < Axis_Count; ++ax) {
            if (a.axes[ax] >= press && b.axes[ax] < press) { *out = {Bind_PadAxisPos, ax}; return true; }
            if (a.axes[ax] <= -press && b.axes[ax] > -press) { *out = {Bind_PadAxisNeg, ax}; return true; }
        }
    }
    return false;
}

void AssignBinding(Bindings& b, Action a, BindSlot slot, Binding bind) {
    for (int other = 0; other < Act_Count; ++other) {
        Binding& o = b.slots[other][slot];
        if (other != a && o.kind == bind.kind && o.code == bind.code) o = {Bind_None, 0};
    }
    b.slots[a][slot] = bind;
}

bool SaveBindings(const Bindings& b, const char* path, Arena& scratch) {
    ScratchScope scope(scratch);
    constexpr size_t kCap = 4096;
    char*            buf  = (char*)scratch.Alloc(kCap, 1);
    int              len  = snprintf(buf, kCap, "# Slipstream controls. Format: ACTION key:<KEY> pad:<BUTTON|AXIS+|AXIS->\n");
    for (int a = 0; a < Act_Count; ++a) {
        char k[32], p[32];
        BindingName(b.slots[a][Slot_Keyboard], k, sizeof(k));
        BindingName(b.slots[a][Slot_Pad], p, sizeof(p));
        len += snprintf(buf + len, kCap - (size_t)len, "%s key:%s pad:%s\n", kActionNames[a], k, p);
    }
    return SaveFile(path, buf, (size_t)len);
}

bool LoadBindings(Bindings& b, const char* path, Arena& scratch) {
    ScratchScope scope(scratch);
    u8*          data = nullptr;
    size_t       size = 0;
    if (!LoadFile(path, scratch, &data, &size)) return false;

    char* text = (char*)data;  // LoadFile null-terminates
    int   loaded = 0;
    for (char* line = text; line && *line;) {
        char* next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (line[0] != '#') {
            char action[32] = {}, tok1[40] = {}, tok2[40] = {};
            if (sscanf(line, "%31s %39s %39s", action, tok1, tok2) >= 2) {
                for (int a = 0; a < Act_Count; ++a) {
                    if (strcmp(action, kActionNames[a]) != 0) continue;
                    for (const char* tok : {tok1, tok2}) {
                        Binding bind;
                        if (!strncmp(tok, "key:", 4) && ParseBinding(tok + 4, Slot_Keyboard, &bind))
                            b.slots[a][Slot_Keyboard] = bind;
                        else if (!strncmp(tok, "pad:", 4) && ParseBinding(tok + 4, Slot_Pad, &bind))
                            b.slots[a][Slot_Pad] = bind;
                    }
                    ++loaded;
                }
            }
        }
        line = next;
    }
    return loaded > 0;
}

MenuInput ReadMenuInput(const InputFrame& now, const InputFrame& prev) {
    auto key = [&](Key k) { return now.keys[k] && !prev.keys[k]; };
    auto pad = [&](PadButton btn) { return PadButtonDown(now, btn) && !PadButtonDown(prev, btn); };
    auto stick = [&](u16 axis, bool positive) {
        return AxisValue(now, axis, positive) > 0.6f && AxisValue(prev, axis, positive) <= 0.6f;
    };
    MenuInput m;
    m.up      = key(Key_Up) || pad(Pad_DUp) || stick(Axis_LY, true);
    m.down    = key(Key_Down) || pad(Pad_DDown) || stick(Axis_LY, false);
    m.left    = key(Key_Left) || pad(Pad_DLeft) || stick(Axis_LX, false);
    m.right   = key(Key_Right) || pad(Pad_DRight) || stick(Axis_LX, true);
    m.confirm = key(Key_Enter) || key(Key_Space) || pad(Pad_A) || pad(Pad_Start);
    m.back    = key(Key_Escape) || pad(Pad_B) || pad(Pad_Back);
    return m;
}

}  // namespace ss
