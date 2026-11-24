// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
#pragma once
#include <string>

// Hotkeys for the Cinematics tab: build a sequence / path without opening the menu. They work while CS2 is in front.
// Saved in <DemoTools2>\DemoTools2-hotkeys.cfg.
namespace Hotkeys {

enum Action {
    SeqAdd,          // add a point (sequence)
    SeqRemoveLast,   // remove the selected / last point
    SeqClear,
    SeqGenerate,
    SeqGeneratePreview,
    SeqStartHere,    // the sequence starts at the current tick
    Preview,         // preview the path / stop
    PathToggle,      // play the path in the demo on / off
    GoToStart,       // jump the demo to the path's start
    PathAddKey,      // add a key (path keys mode)
    Undo,
    PovToggle,       // DemoTools2's POV HUD on / off
    RecordPath,      // record the path / stop recording
    Count
};

struct Binding { int vk = 0; bool ctrl = false, shift = false, alt = false; };

void Init();                          // loads the file (start-up)
void Poll(bool gameInFront);          // window thread, every loop: runs the actions whose keys went down
const char* Name(Action a);           // "Add point"
const char* Help(Action a);           // tooltip
Binding Get(Action a);
void Set(Action a, const Binding& b); // saves the file
void ResetDefaults();                 // the default keys again (saves)
std::string KeyText(const Binding& b);   // "Ctrl+F8", "Mouse 4", "-" (none)

// Capturing a new key in the window: Begin, then call Capture every frame until it returns true
void BeginCapture(Action a);
bool Capturing(Action& a);
bool Capture();                       // true when done (a key was pressed, Esc cancelled, Backspace cleared)
}
