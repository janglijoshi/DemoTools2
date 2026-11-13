// Copyright (c) 2026 janglijoshi. All rights reserved.
// Part of DemoTools2. Use is governed by the DemoTools2 Source-Available License - see LICENSE.
#pragma once
#include <string>
#include <vector>

// CS2 console commands for DemoTools2 (dt2_calc, dt2_cam, dt2_preset). Registered with the game's own command system
// (ICvar::RegisterConCommand), the same way HLAE registers its mirv_ commands, so they work in the console, in .cfg
// files and with HLAE's mirv_cmd. The same handlers also serve the command line in the DemoTools2 window.
namespace Console {
using Args = std::vector<std::string>;
using Handler = void (*)(const Args& args);   // args[0] is the command name

void Add(const char* name, const char* help, Handler handler);   // before Install
bool Install();                                                  // registers everything added so far
void Print(const char* fmt, ...);                                // to the CS2 console (and the window's output)
bool Run(const std::string& line);                               // run a "dt2_..." line here (window / presets)
Args Tokenize(const std::string& line);
std::vector<std::string> RecentOutput();                         // last lines printed, for the window
}
