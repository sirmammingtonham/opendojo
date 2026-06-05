# OpenDojo

Tekken 8 practice-mode tool. Save, share, and reload practice mode recordings
recordings as portable drill files. 

A drill is a byte-perfect copy of Tekken's in-engine practice recording. Once
loaded, the game plays it back exactly as if you'd just finished recording it
yourself. 

I just wanted a way to download and load defense drills without needing to re-record them each time I loaded the game.

Demo: https://www.youtube.com/watch?v=hpciOVFaxrw

## Features

- **Export** any combination of your 8 practice-mode recording slots to a
  shareable `.drill.txt` text file
- **Import** drills back into your slots ("Add to empty slots" or
  "Replace all")
- **OpenDojo Cloud** (in-game): browse drills uploaded by other players,
  filter to your CPU character, one-click download. Upload your own drills
  in one click from the Export tab — no account, no signup
- **Autosave per character**: opt in and your last recording is snapshotted
  per CPU character and restored when you load against that character again
- In-game overlay menu with keyboard + controller navigation
- Filter the drill list to the CPU character you're currently practicing
  against
- Customizable open/close binds (default: `F12` on keyboard, `Back + Y` on
  controller)

## Install

OpenDojo ships as a single DLL that masquerades as `dinput8.dll` — Tekken
auto-loads it on launch.

1. Grab `dinput8.dll` from the latest release.
2. Drop it into Tekken's `Win64` folder, next to `Polaris-Win64-Shipping.exe`:

   ```
   <SteamLibrary>\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\dinput8.dll
   ```

3. Launch the game normally. That's it.

To uninstall, delete `dinput8.dll`.

OpenDojo creates an `opendojo/` folder in the same directory the first time
you save a drill. Your drills, autosaves, and config live there.

## Usage

1. Launch Tekken 8 and start a practice match.
2. Press `F12` (or `Back + Y` on controller) to open the menu.

### Tabs

- **Drills** — list of `.drill` files in `opendojo/`. Each row has an
  **Add** button (load into empty recording slots; refuses if too few are
  free) and **Replace** (clear all slots, then load the drill). Filter
  defaults to the live CPU character; toggle "Show all" to see everything.
- **Browse** — search OpenDojo Cloud for drills other players have
  uploaded. Filter to your current CPU character, sort by newest or
  most-downloaded, click **Download** to save into your local
  `opendojo/` folder. Downloads land alongside your own drills and
  show up in the Drills tab on the next refresh.
- **Export** — save the currently-occupied slots as a new drill file
  (local) and/or upload them to OpenDojo Cloud (community). Name and
  description are optional; CPU character and side are autodetected
  from the live match.
- **Settings** — autosave toggle and key/controller rebinding.
- **About** — version + current binds.

### Controls

| Action | Keyboard | Controller |
|---|---|---|
| Open / close menu | `F12` (rebindable) | `Back + Y` (rebindable) |
| Navigate | Arrow keys / Tab | D-pad / Left stick |
| Activate | Enter / Space | A |
| Cancel / back | Esc | B |
| Switch tabs | — | LB / RB |
| Close menu | X button in title bar | Same toggle |

## File format

Drills are plain text — readable and editable. Example:

```
# OpenDojo drill
name:         jin_string_defense
description:  block string mixups
character:    jin
cpu_side:     p2
recordings:   3

--- recording 1
name:         slot 1
events:       12
total_frames: 180
#
  n    .       30
  f    1+2      8
  ...
```

Edit the event lines (`<dir> <buttons> <frames>`) by hand if you want;
`meta=NNNN` annotations are recorder state and should not be edited.

## Build from source

Requires CMake 3.20+ and MSVC (Visual Studio 2019 or 2022 with the C++
workload). The project fetches Dear ImGui and MinHook automatically.

```powershell
cmake -B dll/build -S dll -A x64
cmake --build dll/build --config Release
```

The build produces `dll/build/Release/dinput8.dll` and (if the path exists)
copies it into the Tekken folder for one-step iteration. Override the
deploy path:

```powershell
cmake -B dll/build -S dll -A x64 -DOPENDOJO_DEPLOY_DIR="D:/Games/TEKKEN 8/Polaris/Binaries/Win64"
```

If `clang-format` is on the path or in a known VS install, it runs
automatically before each build to keep the source tree formatted.

### Cloud features

The in-game Browse and Upload tabs (OpenDojo Cloud) are enabled in the
official release builds. A DLL you build yourself is compiled without
cloud access — those tabs show a "cloud not configured" message and
everything else works normally.

## Compatibility

- Tekken 8 v3.00.02. Other versions may shift the memory offsets the mod uses — they'd need re-resolving.
- Windows 10/11, x64.
- Should coexist with other `dinput8.dll`-style proxy mods only if you stack
  them via an injector — both can't live at the same filename.

## Troubleshooting

- **Menu doesn't open** — check `opendojo.log` next to the DLL. The mod logs
  its hook progress on every launch.
- **"Not ready" in the menu** — you haven't recorded into a slot yet this
  session. Record once and the message disappears.
- **Imported drill doesn't show in the practice menu** — close and reopen
  the practice pause menu once. The data and playback are correct
  immediately, but the menu sometimes caches its display.


## Code
ts hella vibe coded lol, all the RE was done by claude with ghidra and cheatengine mcps.

Spent like 2 days trying to set up an in-game menu and got maybe 50% of the way there, but it's a ton of work so I'm giving up on that for now... I had claude write all findings to `/docs` in case there's any useful info for future modders.

Please feel free to submit a pull request with features and fixes!
