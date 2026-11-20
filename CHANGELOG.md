# Changelog

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

### Upgrading
Replace `DemoTools2.dll` and keep the rest of the folder. Your skins (`DemoTools2.cfg`), presets, campaths and hotkeys
carry over.
