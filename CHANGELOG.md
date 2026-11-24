# Changelog

## 2.1 (2026-11-24)

HLAE-update proof. HLAE still loads DemoTools2 (`mirv_loadlibrary`), but after that nothing depends on HLAE being
updated for the newest CS2.

### New
- **Two recorders: HLAE or DTRecord.** Pick one under **Recorder** in the Record tab (it also applies to *Record the
  path* and *Record from here*); the one you pick is used, and the choice is remembered.
- **DTRecord**, DemoTools2's own recorder. It doesn't use HLAE's recording at all:
  - it reads the picture CS2 shows from DirectX 11 and encodes it with ffmpeg;
  - **picture and sound in one go** (default, *Sound: With the picture*): each clip plays once in real time, every
    frame is timed, the video is built at a constant frame rate from them, CS2's own sound is recorded at the same
    time on the same clock (Windows' per-process loopback, Windows 10 version 2004 or newer), and both are joined
    into one MP4. Motion blur blends the frames CS2 shows in each video frame's time, so it is smoother the higher
    CS2's FPS;
  - *Sound: Second pass*: the picture at a fixed game step with DTRecord's own exact motion blur (4-32 samples),
    then the clip again in real time for the sound; **Game sound** off: picture only.
- With **HLAE** picked:
  - if HLAE takes the commands but records nothing (an HLAE build not updated for this CS2), DemoTools2 notices
    within a few seconds, stops and tells you to switch to DTRecord;
  - if CS2 crashes while HLAE starts recording, the next session starts on DTRecord and the window says why.
- **Record from here** on every Camera tab (with the folder it saves to and **Open folder**), and
  `dt2_record start [name]` / `dt2_record stop`: records the view from now until you stop it (DemoTools2 camera,
  campath or free camera), with the chosen recorder.
- **Points on the mount** (Camera → Attach to a part): the camera stays bolted to the part - the gun muzzle, a hand,
  the head - and glides between positions you set at moments of the demo (forward / right / up, pitch / yaw / roll,
  FOV). Pause, place the camera, **Add point here**; go to a later moment, place it again, add another. Each point can
  be loaded back into the sliders, jumped to, updated or deleted.
- **Freeze path record** (Cinematics → Record the path): the demo stays paused at the current moment and the whole
  campath is flown and recorded through the frozen scene. The path runs on its own clock, one step per recorded frame
  (1 / (fps x blur samples)), so it is as smooth as a normal recording; no sound. Saved as `freeze_<name>_<time>.mp4`.
- **Stay on this gun** (Camera → Attach to a part, gun parts, on by default): the camera stays on the gun that was in
  their hand when you pressed Attach camera, also when it is holstered on their back while they hold another weapon.
  If a holstered gun has no working attachment, the gun's own position is used (and logged). Calc: `keptgun`.
- Record tab: **Not this demo? Choose the file** under the demo found by itself; a file you pick stays until the next
  demo starts.
- Status shows **HLAE** (what DemoTools2 uses it for) and **DTRecord**.
- **Viewmodel** tab on the Camera page - **Position and FOV:** your own viewmodel (left/right, back/forward, down/up,
  FOV) for everyone's first-person gun in a demo, with presets (CS2 default, Classic, Wide, Centered) and an option
  to go past CS2's limits. Off, every player keeps their own, as recorded. Console: `dt2_viewmodel`.

### Changed
- The camera's view hook is also found when an HLAE build changes a little more of CS2's view code than today's
  builds do.
- ffmpeg is also found in DemoTools2's folder and on PATH.

### Fixed
- **CS2 crashed right after loading DemoTools2 with a current HLAE** (crash in `AfxHookSource2.dll` while Panorama
  draws). To find DirectX's Present, DTRecord made a small test device and swap chain; HLAE hooks device and swap
  chain creation for the whole process, took them for the game's, and used them after DemoTools2 had released them.
  With HLAE loaded, DTRecord now never creates anything: it finds the swap chain CS2 already has.
- **POV HUD: the gun jumped.** With the POV HUD on, CS2 thinks the watched player is you, so it placed their gun with
  *your* viewmodel settings in one part of the frame and with theirs in the other. DemoTools2 now gives CS2 the
  player's own position and FOV while the POV HUD swaps.
- **POV HUD and the gun / hands.** The POV HUD learned when CS2 draws the frame only once per session. When that moves
  (a CS2 update, or HLAE 2.193's stream preview and stream actions, which draw extra views in a frame), the swap
  could land where CS2 draws the gun and hands. It is now re-learned all the time, and every view CS2 sets up uses
  the demo's own state, also a second view in the same frame.
- **POV HUD after a CS2 update.** The two controller fields it changes are now taken from CS2's own schema (by name)
  instead of only from code patterns, and before it changes anything it checks that the data really is what it
  should be. If not, it stays off and says why, instead of writing to the wrong place.
- **Switching the POV HUD off** now reloads the view at the current tick, so a first-person model or screen effect
  CS2 made while the POV HUD was on doesn't stay. **Reload view** (Cinematics tab) does the same at any time.
- The POV HUD waits, and says so in its status, while CS2 isn't running DemoTools2's view hook, instead of swapping
  without it.
- **HLAE recordings stopped after one clip** with "HLAE didn't record ... no video in the take folder". DemoTools2
  checked the take folder's file sizes as Windows lists them, which stay at 0 while HLAE's ffmpeg is still writing.
  It now reads the real size, and if nothing is there at the end of a clip it waits up to 30 s for HLAE before
  calling it broken. HLAE's video gets up to 10 minutes to finish (was 2).
- **Recording from the Camera tab saved nothing when the demo was paused.** Stop recording set the end at the current
  tick, and the take only ended once a later tick came; it now ends on Stop, paused or not.
- **Sound lost when joining failed.** If ffmpeg couldn't join a clip's picture and sound, the picture was saved alone
  and the sound deleted. The sound is now kept next to the clip, ffmpeg's message goes to `<clip>.mp4.join.log`, and
  the status says so (DTRecord and HLAE). A single clip is no longer "joined" into a second copy.
- **The Record tab found the wrong demo, or none, for old demos.** CS2 often doesn't keep an old demo file open, and
  the paths found in its memory include every demo played that session; old demos often carry no Steam IDs, so all
  scored the same. Now a file must be on the map CS2 has loaded and long enough for the current tick, players are
  matched by Steam ID and by name, the same file found under two spellings counts once, and the pick doesn't flip
  between equal candidates.
- **CS2 crashed right after a demo jump** ("reading the entity list"): a player CS2 was tearing down had an invalid
  model-name pointer (`0xffff...`). Entity, name and model pointers are now checked for a possible address before
  they are read.

### Notes
- DTRecord needs CS2 in DirectX 11 mode (the default). With `-vulkan`, record with HLAE.
- While DTRecord records sound, `snd_mute_losefocus` is set to 0 so CS2's sound keeps playing while the DemoTools2
  window has the focus.

## 2.0 (2026-11-20)

DemoTools2 2.0 adds two new pages, **Record** and **Cinematics**, and a redesigned window. It has five pages:
**Camera, Cinematics, Record, Skins and Status**.

### New: Record
Record every kill of one player straight to MP4, without cutting anything by hand.

- Finds the demo CS2 is playing by itself and reads every kill from the file. Pick a player and the clip list fills in.
- One clip per kill, or kills close together joined into one multi-kill clip. Set the seconds before and after each kill.
- Each clip shows the round, the kills (weapon, headshot, wallbang, no-scope, through smoke) and its length. Untick
  the clips you don't want. **Watch** jumps the demo to a clip.
- **POV with HUD** (the player's own HUD) or **Clean** first person, with or without the gun.
- Motion blur (frame rate, samples, amount), quality, and one file per clip, all clips joined, or both.
- 4:3 and 5:4 resolutions are stretched to 16:9 automatically (1440x1080 becomes 1920x1080).
- Uses HLAE's recording (`mirv_streams`) and HLAE's ffmpeg. Files go to `recordings\<demo name>` next to the DLL.

### New: Cinematics
Campaths made inside the demo.

- **Sequence mode:** pause the demo, fly to each spot and press *Add point*. Then press *Generate* and DemoTools2
  works out how many ticks each move needs (camera speed, turn speed, shortest move) and builds the real path.
- **Cuts**, so one sequence can hold several shots. Edit any point's ticks, position, angles and FOV, reorder points
  or set one to the current view.
- **Path keys mode:** add keys while the demo plays. Smooth or linear interpolation, ease in and out, hold, even
  speed, change the length, start here, and *look at a player*.
- **Quick moves:** orbit and dolly, made from the current view.
- A **top-down map** of the path, its points and the camera. Undo, and save / load in HLAE's campath XML format.
- **Record the path** to MP4, with the same video settings as the Record page.
- **Hotkeys** that work without the menu open. Defaults: F6 add point, F7 remove point, Shift+F7 clear, F8 generate,
  Shift+F8 generate + preview, F9 preview, F10 path on/off, F11 go to start. Any action can be rebound, and each one
  can be unbound.
- **POV HUD (beta):** DemoTools2's own POV HUD for GOTV demos (`dt2_pov 1`). When the player dies it shows the
  spectator HUD and watches a living teammate, and it goes back to the player next round. The radar only shows
  enemies the team has spotted. HLAE's `mirv_pov` switches also appear when the loaded HLAE has that command.

### New window design
- A **sidebar** with icons replaces the tab bar. It always shows whether a demo is playing, and Status shows a count
  when something needs attention.
- Each page has a title and a one-line description. Sections have clear headings, inputs have outlines, the spacing
  is roomier and the colours are refined.
- **Status** shows summary tiles (Working / Limited / Off), a new **Recording (HLAE)** line, the DemoTools2 and CS2
  versions, and an **Open the log** button.
- The Record clip list is now a table: on/off, round, kills, length, Watch.

### Changed
- **Camera** is now marked **Beta** (it was WIP). While a campath plays, the campath has the camera and the calc
  camera waits.
- Console commands added: `dt2_campath`, `dt2_record`, `dt2_pov`.

### Fixed
- Record: a clip whose recording failed could pick up an older video with the same name from an earlier run of the
  same demo. Only videos from the current run are used now.
- Record: **Stop** threw away the clip being recorded. It is now kept, and included in the joined video.
- Record: an ffmpeg job that hangs for more than 10 minutes is now stopped, instead of being left running in the
  background.
- Camera: after pressing Enter, the command line keeps the keyboard focus, so you can type the next command straight
  away.
- Cinematics: buttons and the *Ticks to next* field were cut off in narrower windows.
- Camera: the delete (x) buttons in the calcs list were cut off.

### Fixed since the 1.9 / 1.10 test builds
- CS2 and the menu could freeze ("Not Responding") when the menu was opened while a demo was loading.
- POV HUD: the camera no longer freezes facing one way, the hands stay visible, the grey death blur no longer shows,
  and the radar no longer shows every enemy.
- The playing demo is found even after CS2 closes the file, and kills are found in demos without round events.
- The **Environment** page (weather) and the old-animations test are not in 2.0. Their settings in `DemoTools2.cfg`
  are ignored. The start-up checks they needed are gone too, so there is less to break after a CS2 update.

### Licence
- DemoTools2 is now open source under the **MIT License** (it was source-available, all rights reserved).

### Upgrading
Replace `DemoTools2.dll` and keep the rest of the folder. Your skins (`DemoTools2.cfg`), presets, campaths and hotkeys
carry over.
