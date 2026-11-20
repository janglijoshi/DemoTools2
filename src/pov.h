// Copyright (c) 2026 janglijoshi. All rights reserved.
// Part of DemoTools2. Use is governed by the DemoTools2 Source-Available License - see LICENSE.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// DemoTools2's own POV HUD (written from scratch): on a GOTV demo, make the client treat the player you watch as its
// own local player, so CS2 shows that player's HUD as in their POV demo. Test stage, console only:
//   dt2_pov 1   on: the watched player's controller becomes the local one (their own HUD)
//   dt2_pov 3   only clears "watching as GOTV"
//   dt2_pov 0   off (everything put back)
namespace Pov {
void Init();                       // console command (start-up thread)
void OnFrameStage(int stage);      // game thread, every FrameStageNotify call, before the game's own
void AfterFrameStage(int stage);   // ... and after it
void Stop();                       // demo stopped / jumped: put everything back (game thread)
int Mode();
void OnViewSetup();
void OnViewDone();                 // the view hook is done: mode 1 swaps now (camera already worked out)                // the view hook runs (game thread): finds the render window
bool CameraPawn(uint32_t& handle); // mode 2: the pawn whose eyes the camera should use
void SetMode(int mode);            // from the window (applied on the game thread next frame)
std::string Status();

// For the recorder (game thread)
struct PlayerSlot { int slot = -1; uint64_t xuid = 0; int team = 0; bool alive = false; std::string name; };
std::vector<PlayerSlot> Players();   // players in the demo now; slot = the number kill events use
uint64_t WatchedXuid();              // who the demo watches in first person now (0: nobody)
bool SpecXuid(uint64_t xuid);        // make the demo watch this player; true once it does (call every frame)
void LockTo(uint64_t xuid);          // POV follows this player (0: back to "whoever is watched when switched on")
}
