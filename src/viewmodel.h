// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
#pragma once
#include <string>

// DemoTools2's own viewmodel (the first-person gun): position and FOV, written from DemoTools2's own research of
// client.dll.
//  - CS2 asks one function where to put a pawn's first-person gun (left/right, forward/back, up/down and its FOV).
//    For the local player it uses your viewmodel_offset_* / viewmodel_fov settings; for anyone else (every player
//    in a GOTV demo) the values that player had, which the demo recorded. Then it limits them (X -2..2.5,
//    Y and Z -2..2, FOV 60..68).
//  - DemoTools2 puts itself between CS2 and that function (at its calls, the function itself isn't changed) and
//    changes the answer: your own position and FOV (Custom), also past CS2's limits if you want.
//  - With the POV HUD on, the watched player is the "local" player, so CS2 took your settings for their gun, and the
//    gun jumped between the two positions. While the POV HUD swaps, DemoTools2 answers with the player's own values.
namespace Viewmodel {
struct Settings {
    bool custom = false;   // off: the demo's own (every player's own, as recorded)
    bool extended = false; // allow values past CS2's own limits
    float x = 1.f, y = 1.f, z = -1.f, fov = 60.f;
};

void Init();                    // start-up thread: finds the function and its calls, loads DemoTools2-viewmodel.cfg
bool Available();
Settings Get();
void Set(const Settings& s);    // from the window; saved
std::string Status();

struct Preset { const char* name; float x, y, z, fov; };
extern const Preset kPresets[];
extern const int kNumPresets;
void Limits(bool extended, float lo[4], float hi[4]);   // x, y, z, fov
}  // namespace Viewmodel
