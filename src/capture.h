// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
#pragma once
#include <cstdint>
#include <string>

// DemoTools2's own recorder, used when HLAE's recording (mirv_streams) isn't there or doesn't work - for example an
// HLAE build that wasn't updated for the newest CS2. Nothing here needs HLAE.
//
// Capture: the frames CS2 shows (its DirectX 11 swap chain, read right before each Present), optionally blended
//   N-to-1 for motion blur, piped as raw video into ffmpeg (H.264 MP4). CS2 in Vulkan mode is not supported.
// Sound: CS2's own sound through Windows' per-process loopback (Windows 10 2004 or newer).
//   Same pass (default): the game plays in real time while the picture and the sound are recorded together; every
//   frame is timed, the video is built at a constant frame rate from them, and both are joined into one MP4.
//   Second pass: the picture at a fixed game step (exact motion blur), then the sound in a real-time replay.
namespace Capture {

bool Install();                       // start-up: finds the swap chain's Present (a hidden test device), hooks it
bool Available(std::string* why);     // hooked, and the game isn't in Vulkan mode
bool SeenFrames();                    // CS2 has presented a frame through the hook (since start-up)

struct Params {
    std::string ffmpeg, file;         // ffmpeg.exe, output .mp4
    int fps = 60;                     // output frame rate
    int samples = 1;                  // game frames per output frame (motion blur; 1 = none)
    float exposure = 0.5f;            // part of each output frame's samples that are blended (0.1 .. 1)
    int crf = 18;
    bool stretch = false;             // 4:3 / 5:4 game: scale to 16:9 while encoding
    bool realtime = false;            // the game plays in real time: frames placed by when they were shown; motion
                                      // blur blends the game's own frames in each output frame's shutter time
};
bool Begin(const Params& p, std::string* why);   // game thread; frames are taken from the next Present on
void End();                                      // game thread; the file is finished in the background
bool Active();
long long FramesWritten();                       // output frames of the current (or last) recording
std::string Error();                             // why the current recording failed ("" = fine)
int WritersRunning();                            // ffmpeg processes still finishing files
uint64_t StartTime();                            // realtime: when the first frame was shown (Sound's clock), 0 = not yet
uint64_t Now();                                  // that clock: 100 ns units

}  // namespace Capture

namespace Sound {

bool Available(std::string* why);
bool Begin(std::string* why);         // game thread; starts capturing CS2's sound (call a little before MarkStart)
void MarkStart();                     // the clip starts now
void MarkStartAt(uint64_t t);         // ... or at this time (Capture::StartTime)
void MarkEnd();                       // the clip ends now
bool End(const std::string& wav);     // stops; writes [MarkStart, MarkEnd) as a 48 kHz 16-bit stereo WAV; false = no sound
void Cancel();

}  // namespace Sound
