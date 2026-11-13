# DemoTools2: CS2 demo tools for HLAE

> Source-available - all rights reserved. Read the [LICENSE](LICENSE) before using or sharing any of this code.

DemoTools2 (formerly DemoSkins) is loaded into CS2 by HLAE and has two parts:

* **Camera and calcs (WIP)** (*Camera* tab, `dt2_calc` / `dt2_cam` / `dt2_preset`): *work in progress - still
  being built and may change or not work in every demo.* Attach the camera to a player, their
  gun or their grenade and follow it smoothly, like `mirv_calc` + `mirv_cam` in HLAE for CS:GO. Record it with HLAE's
  `mirv_streams` as usual.
* **Skins** (*Skins* tab): choose a player in a CS2 demo by their XUID (SteamID64), then give their guns, knife and
  gloves any finish, wear, pattern seed, StatTrak counter, name tag, **stickers** and a **charm**, and change their
  **agent** (player model). Each player can have a separate **T side** and **CT side** loadout. Only that player's
  items change. It works the way nSkinz did for CS:GO.

Coming from DemoSkins: load `DemoTools2.dll` *instead of* `DemoSkins.dll` (not both). Your skins are taken over from
`DemoSkins.cfg` in the same folder the first time.

It only does anything while a **demo is playing** in CS2 started with **`-insecure`** (HLAE adds this for you). In any
other game it refuses to start, so it can't be used in matchmaking or on VAC servers.

## Use it

1. Start CS2 through **HLAE** (*File → Launch CS2*). Keep "-insecure" ticked. Windowed or *fullscreen windowed* works
   best, so the DemoTools2 window can sit on top of the game.
2. Open the console and load the DLL. The quotes are needed if the path has spaces:
   ```
   mirv_loadlibrary "C:\HLAE\DemoTools2\DemoTools2.dll"
   ```
   The console replies `LoadLibraryA OK.`, and the DemoTools2 window opens.
3. Play a demo (`playdemo mydemo`). The DemoTools2 window shows (press **Insert** in CS2 to show or hide it):
   * **Left:** the players in the demo (hover a name to see their XUID). You can also add a player by XUID.
   * **Middle:** *Both sides / T side / CT side* buttons, then *Knife*, *Gloves* and *Agent*, then that player's
     guns. The guns they are holding right now are marked `>`.
   * **Right:** for knife and gloves, first pick the model (Karambit, Sport Gloves, ...). Then the finish list
     (search it, coloured by rarity), wear (with FN/MW/FT/WW/BS buttons), pattern seed, StatTrak with a kill count
     (guns and knives), name tag, and *Remove*. Guns also have **Stickers** and **Charm** tabs; *Agent* shows the
     agents to choose from.
4. Press **Insert** again (or close the window) to hide it. Your choices are saved in `DemoTools2.cfg` next to the DLL and come
   back in every demo where that player appears.

To load it automatically, add the `mirv_loadlibrary` line to HLAE's CS2 config, or to an `.cfg` you `exec`.

## Camera and calcs (WIP)

> **Work in progress.** The camera tools are still being built. They may change in later versions and may not work in
> every demo. The skins part doesn't depend on them.

### Attach the camera to a part

**Camera → Attach to a part** mounts the camera on a part of a player or their gun, so it moves *and turns* with it,
like a camera bolted on (not a camera that follows):

1. Pick the player and the part: gun muzzle / gun body / shell eject / any gun attachment by name, head, eyes, neck,
   chest, pelvis, shoulders, elbows, hands, knees, feet, or their newest grenade.
2. Pick how the camera turns: *with the part* (rigid mount), *with their aim*, *level* (the part's direction without
   tilt), or *always look at their face*.
3. Press **Attach camera**, then drag **Forward / Right / Up** (units, on the part) and **Pitch / Yaw / Roll**
   (degrees, relative to the part) and **FOV** while you watch: it updates live. *Facing them* (yaw 180) puts the
   camera in front of the part looking back, e.g. a camera on the muzzle filming the player's face; *Looking forward*
   looks the way the part points. *Smooth* above 0 takes out shake.
4. Save the result under **Presets** to use it in other demos.

### One click: Follow a player

In the **Camera** tab, pick a player (or *Observed* = whoever's eyes the demo shows), a mode, and press **Follow**:

| Mode | Camera |
|---|---|
| Eyes | their first-person view, with smoothed aim |
| Chase | behind where they look, looking at them |
| Follow | behind where they *move* (smooth for running players) |
| Orbit | circling around them |
| Over the shoulder | fixed offset behind and to the side of their eyes |
| Their newest grenade | follows the last grenade they threw through the air |
| Gun barrel (muzzle) | at the barrel of the gun in their hand (its `muzzle_flash` attachment), aiming where they aim |
| Shoulder bone, from behind | at their shoulder bone, facing where they look (side < 0: left shoulder) |
| Face, from the front | in front of their face, looking back at it |

*Smooth move* and *Smooth turn* set how many seconds the camera takes to catch up (0 = locked on). *Field of view*
0 keeps the game's. **Stop camera** gives the view back.

The camera works in **third person**: while it is on, DemoTools2 switches the demo to chase cam (`spec_mode 3`, the
same as the demo UI's *Chase Cam* button), so the player's own body and world gun are drawn and there is no
first-person gun in front of the camera. Stopping the camera goes back to first person (`spec_mode 2`). Untick
*Third person* (or `dt2_cam thirdperson 0`) to keep the demo's own mode. *Observed* in the player list is locked to
whoever is watched at the moment you press Follow.

Then record as always with HLAE: `mirv_streams add normal mystream`, `mirv_streams record start`. Smoothing runs on demo
time, so it looks the same while recording at a fixed frame rate as while watching.

### Calcs

Every calc has a name and is computed from the demo each frame. The **Calcs (live)** list in the window shows each one
with its current value (green) or *no value* (red, e.g. the player is dead), and the command line under it runs the
same commands as the console.

```
dt2_calc <handle|vecang|fov> add <name> <type> <arguments...>
dt2_calc remove <name>  |  dt2_calc list  |  dt2_calc print <name>  |  dt2_calc clear
dt2_calc probe <handle calc>     lists the attachments and bones that entity's model has, with positions
```

Attachments and bones give a position *and* the angles of that part, so the camera turns with the gun or the body.
Mix them with `mix` to take the position from a part and the angles from the player's aim, then `offset` to move
back/forward, sideways or up from there:

```
dt2_calc handle add p player s1mple
dt2_calc handle add gun weapon p
dt2_calc vecang add muzzle attachment gun muzzle_flash      // the end of the barrel
dt2_calc vecang add aim eye p
dt2_calc vecang add barrel mix muzzle aim
dt2_calc vecang add cam offset barrel -12 4 2              // a bit behind and beside the barrel
dt2_calc vecang add shoulder bone p shoulder_r
dt2_calc vecang add front offset aim 60 0 0                // 60 units in front of his face...
dt2_calc vecang add facecam lookat front aim               // ...looking back at it
```

| Kind | Type | Arguments | Value |
|---|---|---|---|
| handle | `player` | xuid or part of the name | that player |
| handle | `observed` | | the player whose eyes the demo shows |
| handle | `index` | entity index | any entity |
| handle | `weapon` | player handle | the gun in their hand |
| handle | `viewmodel` | player handle | their first-person gun (only for the player whose eyes the demo shows) |
| handle | `owner` | handle | its owner |
| handle | `grenade` | [player handle] | the newest grenade in the air (thrown by that player) |
| vecang | `value` | x y z pitch yaw roll | fixed |
| vecang | `origin` | handle | position and angles of an entity |
| vecang | `eye` | handle | eye position and view angles of a player |
| vecang | `attachment` | handle name | a model attachment: `muzzle_flash`, `shell_eject`, `stattrak`, `keychain`, ... |
| vecang | `bone` | player handle name-or-number | a skeleton bone: `head`, `neck`, `shoulder_l`/`_r`, `elbow_l`/`_r`, `hand_l`/`_r`, `pelvis`, `knee_l`/`_r`, ... |
| vecang | `offset` | vecang forward right up [pitch yaw roll] | relative to another vecang |
| vecang | `chase` | player handle distance height [pitch] | behind where they look, looking at them |
| vecang | `follow` | handle distance height | behind where it moves, looking at it |
| vecang | `orbit` | vecang radius height degrees/s [start yaw] | circling around it |
| vecang | `lookat` | from-vecang target-vecang | position of the first, looking at the second |
| vecang | `mix` | position-vecang angles-vecang | position of one, angles of the other |
| vecang | `level` | vecang | the same, but only its yaw (no tilt or roll) |
| vecang | `add` / `subtract` | a b | sum / difference |
| vecang | `smooth` | vecang position-time angle-time [max speed] | follows it smoothly (seconds, units/s) |
| fov | `value` | degrees | fixed |
| fov | `smooth` | fov-calc time | follows it smoothly |

```
dt2_cam source <vecang calc> | none      the camera's position and angles
dt2_cam fov <fov calc | degrees | game>
dt2_cam enabled 0|1
dt2_cam blend <seconds>                   glide to a new source instead of cutting
dt2_cam thirdperson 0|1                   chase cam while the camera is on (default 1)
dt2_cam hideviewmodel 0|1                 r_drawviewmodel 0 while the camera is on (default 0)
dt2_cam                                   status
```

Example, a smooth camera over s1mple's shoulder that switches to his grenade:

```
dt2_calc handle add p player s1mple
dt2_calc vecang add eye eye p
dt2_calc vecang add shoulder offset eye -60 18 8
dt2_calc vecang add cam smooth shoulder 0.2 0.12
dt2_calc handle add nade grenade p
dt2_calc vecang add nadecam follow nade 120 25
dt2_cam blend 0.6
dt2_cam source cam; dt2_cam enabled 1
// later (e.g. with HLAE: mirv_cmd addAtTick 12345 "dt2_cam source nadecam"):
dt2_cam source nadecam
```

### Presets

`dt2_preset save <name>` (or *Save* in the window) writes every calc and the camera settings to
`DemoTools2-presets\<name>.cfg` next to the DLL; `dt2_preset load <name>` brings them back in any demo. Presets are
plain `dt2_` commands, so you can also `exec` them or edit them by hand.

### Notes

* The camera takes over the view the same way CS2's own demo view override does, right after HLAE's camera code, so
  HLAE's campath/mirv_input still work when the DemoTools2 camera is off.
* If `r_drawviewmodel` is locked, unlock cheats with HLAE first (`mirv_cvar_unlock_sv_cheats`).
* If the field of view doesn't change, use HLAE's `mirv_fov` instead.

## T and CT loadouts

The three buttons above the item list choose which loadout you edit:

* **Both sides:** used whatever side the player is on, unless that side sets its own item.
* **T side / CT side:** used only while the player is on that side. Anything the side doesn't set comes from *Both
  sides* (those rows are marked `(both)`), so you only set what differs, for example a Karambit on T and a Butterfly
  on CT with the same AK skin on both. *Copy it here* copies a *Both sides* gun skin to the side to change it there.

The side is the player's team at that moment, so after the half-time switch the other loadout is used by itself. A
dropped gun follows its owner's side.

## Stickers and charms

Choose a finish first, then open the gun's **Stickers** tab: click a slot (1 to 5), then a sticker (search by name,
event, team or player; the list shows 300 at a time). The next slot is selected for you. `x` empties a slot. Stickers
are put on new (no wear), at the gun's normal sticker positions.

The **Charm** tab picks a charm and its pattern number. The game makes the charm when it makes the gun, so a new charm
shows on a gun already on screen after the next round or *Refresh now*; guns bought or picked up later have it at once.

## Agents

Agents are set **per side**: choose *T side* or *CT side* above the item list, then **Agent**, and pick from that
side's agents (the *Both sides* tab has no agent row). Agents someone in the demo already has are marked "in this
demo"; other agents follow the same rules as knife and glove models (*Any model*, see "Older demos"). Gloves are put
on again after the model changes, so they stay right. Removing the agent puts the player's real model back.
`agents=0` in `DemoTools2.cfg` (or the *Agents* switch) turns agents off. An agent saved for both sides by an older
version moves to the side it belongs to.

## When a skin shows up

* Guns the player **buys or picks up after** you choose the skin always get it, because DemoTools2 applies it the
  moment the gun appears.
* For a gun **already on screen**, DemoTools2 rebuilds the skins right away, the same as CS2's
  `regenerate_weapon_skins` command (no sv_cheats needed), and again after every round change or demo jump.
* **Refresh now** reloads the current moment of the demo (a 1-tick jump), if you ever need it.

* Skins belong to the gun's **owner** (who bought it or spawned with it; the game's "X's AK-47"), not to whoever holds
  it: the player's own AK keeps their skin when someone else picks it up, and a gun they pick up from another player
  keeps its own skin.
* A gun you give a skin becomes a fresh item: the **stickers, keychain and other extras** the player's real gun had in
  the demo are taken off (the same as the WeaponPaints server plugin does). Name tag, StatTrak, stickers and charm come
  from what you set in the window.
* Removing a skin puts the gun's real skin back, stickers and keychain included.

## Smooth playback

* While you click in the DemoTools2 window, CS2 isn't the focused window and slows itself down on purpose. Type
  `engine_no_focus_sleep 0` in the CS2 console to keep full speed (press Insert to hide the window when you're done).
* A skin change rebuilds only that one weapon; nothing runs in the background when no skins are set.

## Why a separate window

DemoTools2 draws its menu in its own small window instead of inside the game picture. HLAE hooks CS2's DirectX
drawing for recording, and an in-game menu would have to share those objects with it (that is what crashed 1.2/1.3).
The separate window can't interfere with HLAE or with your recordings: it never appears in captured frames.

## Any knife or glove model

CS2 plays demos with the demo's own animation data, which only covers the knife and glove models its players used.
A model outside that list makes CS2 crash a few seconds later (traced to the demo's AnimAssetData table). With
**Any model** ticked (the default), DemoTools2 switches demo playback to CS2's own animation data while such a model is
in use (it sets the game's `ag2_use_networked_serialization_context_demo` setting to 0 directly; the console command
alone did not take effect). Then every knife and
glove type works. Models some player in the demo already has are marked **in this demo** in the pickers; those work
even with Any model off. If player animations ever look wrong, untick Any model and restart the demo.

## Older demos

Demos recorded with an older CS2 version play fine with DemoTools2: gun skins, knife finishes, gloves, StatTrak and
name tags all work. The only question is knife and glove **models nobody in that demo has**: those need CS2's own
animation data, and when a demo is much older its animation data no longer matches, so CS2 crashes (seen with a demo 16
versions old).

* Demos up to **5 versions** older (`any_model_versions_back` in `DemoTools2.cfg`) may use any model. The first time
  for each demo version DemoTools2 notes it in `DemoTools2-animtest.txt`; after 30 s without a crash that version is
  remembered as working (`anim_ok_versions`). If CS2 crashes or is closed in those 30 s, the next start remembers the
  version as not working (`anim_bad_versions`) and never tries it again.
* Older demos, and versions that crashed, offer only the knife and glove models marked **in this demo**; the window
  shows "older demo". `any_model_old_demos=1` allows any model in every demo (expect crashes).

DemoTools2 compares the versions the way CS2 does for TrueView (`demo version ..., game version ...` in the log).

## Legacy-model finishes

Many finishes (for example USP-S | Neo-Noir, Glock-18 | Bullet Queen) are painted for the weapon's older "legacy"
mesh. DemoTools2 switches the weapon to that mesh for those finishes with the weapon's `body` body group, the same thing
the WeaponPaints server plugin does, so they don't come out scrambled. The list comes from the game's item data (`use_legacy_model`) via SkinStudio's
skin list; regenerate it with `tools/gen_catalog.py <catalog> <skins_en.json>`.

## First person

Since CS2's 2025 view-model update, the gun you see in first person is not the weapon itself but a separate model the
weapon makes for itself (`cs2_hudmodel_weapon`). It gets its own copy of the model, the mesh choice and the skin when it
is made. DemoTools2 2.1 keeps it in step: same mesh as the weapon (legacy finishes), and it is made again after every
skin rebuild and after a knife model change, so first person shows the same skin as third person.

## Gloves on players who had none

A player model shows its own default gloves through its `first_or_third_person` body group, and while it does, CS2
deletes any econ gloves every frame. Servers switch it to `*_hide_default_gloves` when a player has gloves; DemoTools2
does the same for players you give gloves to (and puts it back when you remove them).

## Limits of this version

* **Knife model swap:** pick any knife and the player's knife becomes it (model, animations and finish). DemoTools2
  changes the knife's subclass the way the game does when a server swaps it, then loads the new model.
* **Gloves** change both the type and the finish. They refresh through the game's own "re-apply gloves" flag.
* **StatTrak** adds the counter with your kill count; on a gun already on screen it can need *Refresh now* to appear.
* **Charms** appear when the game makes the gun: on a gun already on screen, after the next round or *Refresh now*.
* **Agents** change about 1-2 seconds after you pick one (the model is loaded first).
* **After a demo jump** the demo's own skins can show for about a second before yours are put back on.
* **Camera** tools are a work in progress.
* **CS2 updates.** Field positions are read from the game itself at start-up, so most updates need nothing. A few
  function numbers are fixed. If an update moves one, the log says what failed, and the numbers can be changed in
  `DemoTools2.cfg` (the `index.` lines) without rebuilding.

## After a CS2 or HLAE update

DemoTools2 reads what it needs from the running game at start-up instead of relying on fixed addresses. After an
update, anything it can't find (or that doesn't behave as expected) switches itself off and is listed in the
**Status** tab, with the reason; the menu and every other part keep working. Typical outcome: the menu, saved
skins and settings always work; a skin part (say knife models) may show *Off* until DemoTools2 is updated for that
CS2 build; the camera (WIP) depends on HLAE and may switch off after an HLAE update. If a part crashes, it stays off
for the next session (safe mode) and the Status tab says so. *Open DemoTools2 folder* in that tab opens the logs.

## Something wrong?

Check **`DemoTools2.log`** next to the DLL. If the game crashed, the crash report (which step, and where) is in
**`DemoTools2.previous.log`** after the next start, and the part that crashed stays switched off for that session
(*safe mode*), so the rest keeps working. The same lines appear in the game console with a `[DemoTools2]` prefix.
Typical messages:

| Log says | Meaning |
|---|---|
| `REFUSED: ... -insecure` | CS2 wasn't started by HLAE with -insecure. |
| `WARNING: X::y not in schema` | A CS2 update renamed a field: that part may not work. Send the log. |
| `Can't find the entity list` | `index.entity_system_offset` changed in a CS2 update. |
| `CEconItemAttribute table not found` | StatTrak and gloves can then only change values a player already has. |
| `g_pMemAlloc not exported` | Same as above, for items whose attribute list is full. |
| `SetModel NOT FOUND` / `subclass change NOT FOUND` | A CS2 update moved these: knife model swap is off, knife finishes still work on the same model. |
| `regenerate_weapon_skins NOT FOUND` | *In place* falls back to the 1-tick jump. |
| `first-person models NOT found` | First person may show the old skin until the weapon is drawn again. |
| `camera: view hook NOT found` | A CS2 update moved the view code: the camera can't take over the view. Send the log. |
| `console commands: ...` | The dt2_ commands aren't in the console; the command line in the Camera tab still works. |
| `body groups: NOT found` | Gloves can't be shown on players who had default gloves; legacy finishes use the mesh mask only. |

## Build it yourself

* **Windows:** install Visual Studio 2022 ("Desktop development with C++") and CMake, then run `BUILD.bat`. The
  result is `build\Release\DemoTools2.dll`.
* **Linux cross-build:** use [llvm-mingw](https://github.com/mstorsjo/llvm-mingw); see `build/build.sh`.
* After a new SkinStudio item catalog, regenerate the finish list with
  `python3 tools/gen_catalog.py path/to/skinstudio_catalog.json`, and the sticker / charm / agent lists with
  `python3 tools/gen_extras.py path/to/skinstudio_catalog.json path/to/items_game.txt`.

## Licence

DemoTools2 is **source-available, not open source**: Copyright (c) 2026 janglijoshi, all rights reserved. You may
read the code, build it and use it yourself, and send pull requests. You may **not** re-upload, redistribute, sell,
or reuse it (or parts of it) in other projects without written permission. The full terms are in [LICENSE](LICENSE).

Download DemoTools2 only from this repository's **Releases** page.

Third-party parts (Dear ImGui, MIT) keep their own licences, and the credits (HLAE / advancedfx, cs2-dumper) are in
[NOTICE.md](NOTICE.md). DemoTools2 isn't made by or affiliated with Valve or the HLAE project.
