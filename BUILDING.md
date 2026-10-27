# Building from a fresh clone

The repository holds only this project's own tools and runtime. Nothing that comes from the game or from the console's
software is in it, with one stated exception: a list of function names (see "The SDK function list"). Everything else of that kind (the lifted C code, the lifted system-library code, the generated headers and
tables, the script-module libraries) is **generated on your PC from your own dump**, into `build/` (ignored by git).

## The short way: one click

Nothing has to be installed first.

1. Download this repository (Code, Download ZIP) and unpack it, or `git clone` it.
2. Double-click **`Install SSBU.bat`**. It puts a private Python into `deps\python` (the python.org embeddable package,
   checked against its SHA-256, plus pip and `requirements.txt`; nothing is installed system-wide, PATH and the registry
   are not touched, deleting `deps\` removes it), clears the downloaded-file mark (Unblock-File) and opens the installer
   window (`scripts\installer_window.ps1`). The window has already looked for your files: `prod.keys` and firmware in
   `%USERPROFILE%\.switch` and in the folders of yuzu, Ryujinx, suyu, sudachi and eden (also portable `user\` folders and
   their game folders), the base game (`.xci`, `.nsp`, `.nsz`, `.nca` or an extracted dump folder), the 13.0.4 or 13.0.5
   update and the DLC `.nsp` beside it (`scripts\find_inputs.py`; key files are checked by key name only, no key value
   is read into anything). It also checks the PC (64-bit Windows 10/11, CPU, RAM, free disk measured against what a build
   takes, Vulkan, the movie codecs: `scripts\precheck.py`). **Install location** says where the big files go: the
   unpacked game (about 17 GB; with `.nsz` packages about as much again for a while, to decompress them), the build
   (about 8 GB), the logs and your saves. It is `build\` inside this folder unless you choose another folder (Choose...,
   for example on a drive with more space); the field shows the free space of that drive and what is needed as you
   type. The choice is remembered in `build_location.txt` in this folder, and Resume, `Play SSBU.bat`, the shortcuts,
   `scripts\setup.py`, `scripts\run.py` and `scripts\precheck.py` all use it (`scripts\build_location.py` prints it;
   `install.py --build-dir <folder>` sets it). Only a few small files stay in `build\` here (`python_path.txt`, the
   downloads of the bootstrap, the update record), and `deps\` (the private Python, LLVM, the runtime package) stays in
   this folder. Change what it found if needed and press **Install**. It shows
   each step, a progress bar with the time left, and the log. **Dry run** shows what would happen and changes nothing.
   The installer (`scripts\install.py`) first updates this folder from GitHub (`scripts\update.py`: a git pull, or for a
   ZIP copy only the changed files; the build then redoes only what changed), makes the dump, lists your DLC for the game
   (`build\config\dlc.txt`), downloads libopus for the music (`scripts\fetch_media.py`), uses the **prebuilt runtime** of the
   GitHub Release when there is one for this version (below: then only LLVM 9 is needed, no Visual Studio), downloads
   the missing tools from their official pages (each archive checked against the SHA-256 written in `scripts/install.py`),
   builds, and makes a desktop and Start menu shortcut. If it stops, double-click it again: it goes on from there (Resume).
3. Double-click **`Play SSBU.bat`** or the **SSBU native** shortcut (your saves go to `saves\` in the install location,
   `build\saves` by default).

Online play (experimental: Local Wireless over a LAN, a room code or a lobby server): [docs/ONLINE.md](docs/ONLINE.md).

The rest of this page is the same build done by hand, step by step.

## By hand

```
git clone https://github.com/Denkishi/ssbu-recomp
cd ssbu-recomp
python scripts/setup.py --dump <your dump folder> --suyu <folder> [--llvm ... --sdl3 ... --vulkan-headers ... --glslang ...]
```

One command, eleven resumable steps. When it ends it prints how to start the game.

## With the prebuilt runtime (no MSVC Build Tools)

A GitHub Release can carry `ssbu-runtime-<commit>.zip`: this project's own runtime, prebuilt (`ssbu_native.exe`,
`SDL3.dll`, `ssbu_glslang.dll`, their licences and a `manifest.json` with the SHA-256 of each file). Nothing of the game
is in it: since `runtime/game_data.h`, everything the exe needs that is derived from a dump (the SDK data symbols, the
module list, the lifted SDK functions, the exception-table test vectors) lives in the game DLL, which is built on your
PC. With the package, your PC builds only the game DLL and the script-module DLLs, with **LLVM/clang 9 alone**: no MSVC
Build Tools, no Windows SDK, no SDL3 / Vulkan-Headers / glslang / suyu.

```
python scripts/setup.py --dump <your dump folder> --runtime <ssbu-runtime-....zip>
```

`scripts/install.py` does this by itself when `RUNTIME_RELEASE` in it names the Release asset of this version (the zip is
downloaded into `deps/` and checked against the SHA-256 written there), or when a `ssbu-runtime-*.zip` is in `deps/`
(`--runtime <zip>` names one, `--no-runtime` builds everything as above). A package is used only by the version of the
sources it was built from (`manifest.json` holds a digest of the tree; `--runtime-any-tree` overrides). The digest is
the same for a `git clone` and for GitHub's **Download ZIP** of the same commit (line ends do not count). Packages made
since 2026-10-20 also list the files one by one (`tree_files`): then files you only added (a mod of your own in `mods\`)
do not count, and a refusal names the files that differ. When the package cannot be used, `setup.py` and the installer
say why in plain words (your copy is older or newer than the package, or incomplete) and what to do, usually: download
the latest ZIP and unpack it into a new folder.

How the game libraries are built without MSVC (`RECOMP_NOCRT=1`, `tools/lifter/build.py`): the lifted code includes only
`<stdint.h>` (clang's own), `<string.h>`, `<math.h>` and `<setjmp.h>`, which come from `runtime/nocrt/` with the same
declarations as the Universal CRT's (objects compiled both ways are byte-identical but for the COFF time stamp); the
functions they call (`memcpy`, `memset`, `sqrt`, `fma`, `floor`, ... and `__chkstk`, `_setjmp`, `longjmp`) come from
`ucrtbase.dll` and `ntdll.dll`, which every Windows 10 and 11 has, through import libraries `lld-link` makes from the
`.def` files in `runtime/nocrt/`; the DLLs are linked by `lld-link` with no C runtime and no entry point. `--nocrt` builds
the game libraries this way while still building the exe with MSVC.

Making the package (maintainers; needs the MSVC Build Tools and the tools of the table below, no dump):
`python scripts/make_release.py [--opus <opus.dll>]`. It builds the exe with `tools/lifter/build.py host` (linked against
`runtime/nocrt/ssbu_game.def`, not against a game DLL), audits the package (only the listed files; the exe's linker map
names only the runtime's own objects; no path or name of the PC; with `--against <build\work>` none of a dump build's
tables is in any file), writes `dist/ssbu-runtime-<commit>.zip` and prints its SHA-256 for `RUNTIME_RELEASE`.

**On Linux**, see [BUILDING_LINUX.md](BUILDING_LINUX.md): a native build with `scripts/linux/install_deps.sh` and
`setup.py --target linux`. This file describes the Windows build.

## What you need

| Requirement | Version tested | Where from |
|---|---|---|
| Windows 10/11 x64 | 11 | |
| Python | 3.14 (3.10 or newer should do) | https://www.python.org/downloads/ |
| Python packages | numpy, lz4, zstandard, Pillow, unicorn, capstone | `pip install numpy lz4 zstandard Pillow unicorn capstone` (the build needs only these; `capstone` is for the differential test in step `check`, and the fighter tools and `tools/lifter/patches/kinds_scan.py` use it too) |
| LLVM/clang | **9.0.0** (8.0.0 also works) | https://github.com/llvm/llvm-project/releases/tag/llvmorg-9.0.0 (Windows installer, 64-bit) |
| MSVC Build Tools, with the "C++ x64/x86 build tools" and a Windows 10/11 SDK | 2026 (v18) / SDK 10.0.26100 | https://visualstudio.microsoft.com/downloads/ ("Build Tools"). clang finds it by itself; `build_glslang.bat` finds it with `vswhere` |
| SDL3, development package for Visual C++ (`SDL3-devel-<ver>-VC.zip`) | 3.4.18 | https://github.com/libsdl-org/SDL/releases |
| Khronos Vulkan-Headers | the 1.4.x tag matching your driver | https://github.com/KhronosGroup/Vulkan-Headers |
| glslang, Windows x86_64 release package (`glslang-*-windows-x86_64-release.zip`, has `include/` and `lib/`) | 16.6.0 | https://github.com/KhronosGroup/glslang/releases |
| Source checkout of a yuzu-family emulator (only `src/core/hle/service/mii/` is read, by `tools/lifter/gen_sdktest.py`, for the reference data of the self-test) | suyu | any yuzu-lineage open-source repository that still has `src/core/hle/service/mii/types/raw_data.cpp` |
| A GPU with a Vulkan 1.3 driver (to run, not to build) | | your GPU vendor |
| Media Foundation (movies) with the Microsoft Store codecs **VP9 Video Extensions** and **Web Media Extensions 2.x** (the movies' Vorbis sound; without it the opening movie stays black; Windows N editions also need the Media Feature Pack) | part of Windows + the two Store packages | Microsoft Store. Check with PowerShell: `Get-AppxPackage *WebMediaExtensions*, *VP9VideoExtensions* \| Select Name, Version`. The log line `no Vorbis decoder on this PC` names the missing one; `SSBU_SKIP_MOVIES=1` skips every movie meanwhile |
| FFmpeg, the movies' fallback where Media Foundation cannot play them (Store codecs missing or broken, Windows N): an LGPL build with only the WebM demuxer and the VP9 / H.264 / Vorbis / Opus decoders, five DLLs that import only Windows' own; **optional** (without it those PCs stay on a black opening movie) | 8.1.3 (`ssbu-ffmpeg-8.1.3-lgpl-win64.zip`) | `scripts/install.py` (or `python scripts/fetch_media.py`) downloads it from this project's GitHub Release into `deps\ffmpeg` and checks its SHA-256 and each DLL's; `scripts/ffmpeg/build_ffmpeg_win.sh` rebuilds it from FFmpeg's signed source. `build.py` compiles `runtime/mf_ffmpeg.c` with `deps\ffmpeg\include` (or `FFMPEG_ROOT`), setup.py copies the DLLs beside the exe. Log: `movie: ... the FFmpeg fallback plays it`; `SSBU_MOVIE_FFMPEG=1` uses it for every movie, `=0` never |
| Dear ImGui (MIT, ocornut/imgui), source folder with `imgui.h` and `imgui.cpp`; **optional**: the in-game settings menu (F1), left out of the build without it | v1.91.9b | https://github.com/ocornut/imgui/releases/tag/v1.91.9b; `scripts/install.py` downloads it into `deps\imgui` and checks its SHA-256 |
| libopus as a DLL (the music and the game's other Opus sounds; **without it they are silent**, the rest runs) | 1.6.1, built from source as below | the source release from https://opus-codec.org/downloads/ (check it against Xiph's `SHA256SUMS`), or MSYS2's `mingw-w64-ucrt-x86_64-opus` |

Unpack SDL3, Vulkan-Headers, glslang and the emulator checkout anywhere and give the folders with `--sdl3`,
`--vulkan-headers`, `--glslang`, `--suyu`, or put them under `deps/SDL3`, `deps/Vulkan-Headers`, `deps/glslang`,
`deps/suyu` (ignored by git), or set `SDL3_ROOT`, `VULKAN_HEADERS`, `GLSLANG_ROOT`, `SUYU_ROOT`. clang: `--llvm <folder
with bin\clang.exe>` or `LLVM_ROOT`, default `C:\llvm-9.0.0`. The Vulkan SDK is not needed to build.

libopus: give the DLL or its folder with `--opus`, or put it under `deps/opus` (`opus.dll` or `libopus-0.dll`), or set
`OPUS_DLL`; step `link` copies it beside the exe. Built from the source release with CMake (the "C++ CMake tools for
Windows" component of the Build Tools, or cmake.org) and the MSVC tools:

```
cmake -S opus-1.6.1 -B opus-build -DBUILD_SHARED_LIBS=ON -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF
cmake --build opus-build --config Release
python scripts/setup.py ... --opus opus-build
```

MSYS2's `libopus-0.dll` (`C:\msys64\ucrt64\bin`, `mingw64`, `clang64`) needs nothing: the runtime loads it from there.
Without any of these the deps step and the link step say so, and the log of a run says
`nn::codec: libopus NOT FOUND: Opus streams will be silent`. At run time `SSBU_OPUS_DLL=<file>` names another DLL.

## What you dump from your own console

The port handles **Super Smash Bros. Ultimate 13.0.4 and 13.0.5** (base game with the 13.0.4 or 13.0.5 update applied) and
nothing else; step `convert` tells them apart and refuses another build ([docs/VERSIONS.md](docs/VERSIONS.md)). From
your own copy of the game, with the homebrew dumping tools you prefer (for example nxdumptool, running on a console you
own), produce a folder:

```
<dump>/exefs/      the whole ExeFS: main, sdk, rtld, subsdk0, main.npdm   (with the update applied)
<dump>/romfs/      the RomFS with the update applied (it contains data.arc)
<dump>/sysdata/fonts      optional: your console's system shared fonts (.ttf); without them the open-source ones are used
<dump>/sysdata/zoneinfo   optional: the tz database files
```

If you only have packages (`.nsp`, or a cartridge image `.xci` of the base game) and your own keys, one command makes the
folder:

```
python tools/extract/dump_folder.py <base.nsp|base.xci> <update.nsp> <dump> [--firmware <firmware dump folder>]
```

It writes `exefs\` (from the update) and `romfs\` (base with the update applied), and with `--firmware` the system fonts.
The pieces are also tools of their own: `tools/extract/romfs_extract.py <base> <update> <out>` (RomFS with the update
applied; `.nsp` or `.xci`), `tools/extract/nca_exefs.py` (ExeFS from an NCA) and `tools/extract/fw_fonts.py` (system
fonts from a firmware dump). They read the keys from `~/.switch/prod.keys` and need `pip install pycryptodome`. This
repository contains no keys and no link to any.

### System fonts

The game draws its text with the console's 7 shared system fonts. A firmware is **optional**:

- **With** your firmware (the installer's Firmware field, `dump_folder.py --firmware`, or `tools/extract/fw_fonts.py`
  into `<dump>/sysdata/fonts`), the game uses your console's own fonts and looks exactly as on the console.
- **Without** it, the runtime uses the open-source replacements shipped in `runtime/fonts_open` (Open Sans, Source Sans
  Pro and the yuzu project's button-icon font, from suyu; licences and sources in `runtime/fonts_open/README.md`;
  `tools/extract/open_fonts.py` makes them again from their source). `scripts/setup.py` stages them beside the exe.
  What changes: menus and texts use a different typeface; Chinese, Japanese and Korean characters and most symbols
  are missing (each is drawn as an empty box), so play in a Latin-alphabet language; the button icons are yuzu's.
  Fights and saves are not affected.

`SSBU_FONTS=<folder>` chooses a font folder by hand. The runtime looks there (or in `..\sysdata\fonts`), then in
`fonts_open\` beside the exe, and stops at start (code 7) only when a font file is in neither place.

## The SDK function list

`sdklift.py` lifts selected functions of the console SDK module (`sdk`) to C. Which functions, and where they must be cut
or trapped, is a curated list of that module's function names with this project's notes:
`tools/lifter/sdklift_roots.txt`. It holds names and addresses only, no code. `--sdk-roots <file>` uses another list; the
format is documented in `tools/lifter/sdklift.py` and `tools/audit/lift_select.py` regenerates its automatic block.

## The steps

| Step | What it does |
|---|---|
| `deps` | checks every tool above and the dump; runs a test compile and link |
| `stage` | lays the sources out in `build/work` (the tools expect their old single-folder layout) |
| `convert` | `main` and `sdk` (NSO) to `build/binaries/<13.0.4 or 13.0.5>/*.elf`; checks the build and the segment hashes |
| `modules` | extracts the game's script modules (NRO) from `data.arc`, writes their address map and registry |
| `tables` | generates every header and table the runtime needs (imports, SDK data, self-test vectors, exception-table tests) |
| `lift` | lifts the main executable (`recomp_eh.py`) |
| `sdk` | lifts the system-library functions the runtime uses (`sdklift.py`) |
| `compile` | compiles the lifted main with clang -O1 into `ssbu_game_eh_o1.dll` (functions over 20,000 statements stay unoptimised, 180 s per file) |
| `nro` | lifts and compiles the script modules into `build/work/nro_bin_eh_o2/` (`--modules a,b` for a subset) |
| `link` | builds the glslang shim DLL and links the host runtime into `build/work/ssbu_native.exe`, copies `SDL3.dll` (and libopus, when found) |
| `check` | `SSBU_SDK_SELFTEST`, `SSBU_PAD_SELFTEST`, and the differential lifter test against the Unicorn emulator |

`--status` shows what is done, `--redo <step>` and `--from <step>` repeat steps, `--only <step>` runs one, `--jobs N` sets
the compiler parallelism (default `min(24, cpus)`). A failed step is reported with the tail of its log (`build/logs/<step>.log`);
fix the cause and run the same command again.

### Measured (clean clone, 32-thread PC, `--jobs 20`, other programs running)

| Step | Time |
|---|---|
| deps, stage, convert, modules, tables | 1 s, 0.1 s, 1.4 s, 3 s, 1.3 s |
| lift (112,890 functions, 1,161 C files, 1.3 GB) | 19 s |
| sdk (1,375 functions) | 1.2 s |
| compile (1,161 files, 511 MB DLL) | 27 min 22 s |
| nro (98 modules) | 15 min 20 s |
| link (glslang shim + 44 host files) | 34 s |
| check (SDK self-test 0 failures, pad self-test 49/49, C++ runtime self-test; the differential test of the translator only with `--full-check`) | under 1 min |
| **Total** | **about 45 min** |

Disk: `build/` is about 7 GB (generated C 3.8 GB, objects 1.6 GB, libraries 1.2 GB), plus shader caches that grow as you
play. Verified end to end: the scripted match (`tests/seq_battle_play.txt`, 80 steps) ran 242 s on the built exe: the
input sequence reached its last step, 12,613 frames presented, 0 Vulkan call failures, clean shutdown.

## Running

```
python scripts/run.py --userdata <folder for your saves>
```

`--userdata` is where the game writes its save data (use a copy of a folder while testing). Without `--seconds` the game
runs until you close it. The first run builds shader and pipeline caches in `build/cache` and stutters while it does.

Scripted match, as used to verify the build (window off-screen and muted because the exe is not named `ssbu_native.exe` or
`ssbu_user.exe`):

```
python scripts/run.py --userdata <copy of a save folder> --seq tests/seq_battle_play.txt --seconds 240 --seed 1 --exe ssbu_match.exe --log match.log
```

Fast test run (the debug mode, `tests/DEBUG_MODE.md`; the link step now also compiles `debug_mode.c`; the 44 host files of the
timing table above are 45 since then):

```
python scripts/run.py --userdata <copy of a save folder> --seq tests/seq_battle_play_fast.txt --seconds 240 --exe ssbu_fast.exe --log fast.log --env SSBU_SPEED=0 --env SSBU_SKIP_MOVIES=1 --env SSBU_AUDIO_MUTE=1 --env SSBU_VK_PREP=0 --env SSBU_DEBUG_END=20s
```

`SSBU_SPEED=0` runs uncapped, `SSBU_DEBUG_END=20s` ends the run after 20 s of fight, `SSBU_VK_PREP=0` leaves out the background
shader preparation (on a normal run it works for about 15 minutes of play and writes about 2.5 GB into `build/cache`).

Options are environment variables, set with `--env NAME=VALUE` (full list: comments at the top of the runtime sources):
`SSBU_RENDERER=vk|gl`, `SSBU_SHOW=1` (show the window of a test exe), `SSBU_STRICT=1|report|path` (list the host
functions that are not the real behaviour), `SSBU_TRACE`, `SSBU_PAD_BACKEND=xinput`, `SSBU_FIXED_SEED=<n>`.
Pad mapping file and tools: `ssbu_native.exe --pad-test`, `--pad-map <n>`.
Skyline plugins (ARCropolis, smashline and the mods built on them) that you install yourself: [docs/SKYLINE.md](docs/SKYLINE.md).

## GameCube controller adapter

The Nintendo adapter (WUP-028) and clones such as the Mayflash 4-port adapter **in its "Switch" mode** (both are USB `057e:0337`) are read directly (`runtime/nnpad_gca.c`). Each controller plugged into a port reaches the game as a **GameCube controller** (style `Gc`, with its analog triggers and its rumble motor), not as a Pro Controller.

It needs the adapter's **WinUSB driver**, the one Slippi's installer ("install GameCube adapter drivers") or Zadig (WinUSB on `WUP-028`) puts on it. Nothing is installed or changed by this project. It works with only one app at a time, so close Delfinovin or Slippi before playing.

| Setting | Effect |
|---|---|
| `SSBU_GCA=0` | does not look for the adapter at all |
| `SSBU_GCA_STATS=1` | every 2 s in the log: reports per second, interval, apply time, age of the state at the game's samples |
| `controllers.ini`, `[general]` `gamecube_style = pro` | shows these controllers to the game as Pro Controllers (default `native`) |

If a GameCube controller misbehaves in the game, try `gamecube_style = pro`, which keeps the adapter and changes only how the game is told about it.

## In-game menu (F1)

A settings menu drawn over the game's picture (Dear ImGui, Vulkan renderer only). It is **off by default**: turn it on with
`SSBU_MENU=1` or `menu = on` in `config\graphics.ini` (`SSBU_MENU=0` wins over the file).

- **F1**, or a controller's Home/Guide or Capture/Share button, opens and closes it; **+** or **Esc** also close it. Those
  controller buttons never reach the game. A controller without a usable Home button (a Steam Deck, whose Steam button
  belongs to Steam): hold **−** and **+** together for 1 second (the game sees them until the menu opens).
- While it is open the game keeps running but gets no input from the pads and the keyboard.
- Pages: Display, Graphics, Audio, Controls, Experimental, Add-on content, Language, Log (with **Export log**), About. Every
  control is marked **[live]** (acts at once) or **[restart]** (saved now, used at the next start).
- **Experimental** page: ON / OFF switches for **Online play** (then choose: Lobby server - recommended, the public lobby
  is filled in -, Same network (LAN), Host a room (shows a ROOM CODE with a Copy button) or Join with a code, typed or
  entered with the on-screen keyboard) and **Run-ahead** (greyed out when the build lacks the run-ahead pieces). They are
  saved to `config\graphics.ini` and used at the next start; **Restart the game now** starts it again. Then use the game's
  own Local Wireless menu: one player Create Room, the others Rooms List.
- **Controls** page: remap the keyboard and every controller. For each Switch control (A B X Y L R ZL ZR + - stick
  clicks, d-pad, both sticks' four directions; the game's default action is shown beside it) press **Change** and then the
  key, button, trigger or stick direction you want (6 s; Esc, Home or B for a key cancels), **Clear** or **Default**.
  A controller's changes are saved either for every controller of its kind (Xbox, PlayStation, Switch, GameCube, other)
  or for that one controller, in `config\controllers.ini`, and used at once. Also: which player each controller and the
  keyboard are, stick dead zone / outer zone, trigger threshold, and a live test of what the game would get.
- **F2** shows a small stats overlay (fps, frame time, online state, run-ahead), menu open or not.

It is built only when Dear ImGui v1.91.9b is found: `scripts/install.py` downloads it into `deps\imgui` and checks its
SHA-256, or give a folder with `python scripts/setup.py ... --imgui <folder with imgui.h>`, or set `IMGUI_ROOT`. Without it
the build still works, and F1 only writes a log line saying why there is no menu.

## Keyboard movement

A key of the left stick pushes the stick fully at once, which the game reads as a flick: on the keyboard a direction always
dashes and down always drops through a platform. `movement = ssf2` in the `[keyboard]` section of `config\controllers.ini`
(or `SSBU_KEYBOARD_MOVEMENT=ssf2`, or **SSF2 Style** on the Controls page of the settings menu; default `full`, the old
behaviour) reads the keys the way Super Smash Flash 2 does (`runtime/nnhid.c` `kb_ssf2`, which lists the rules and the
measurements):

- a direction walks; the same key again within 200 ms of letting it go dashes; the other key while running dash-dances;
- down crouches (and fast-falls in the air); down twice within 200 ms drops through a platform;
- a direction and A within about 130 ms (down and A: 100 ms) is a smash attack, later a tilt; with the shield held a direction rolls;
- up waits 4 frames: A or B in them is an up smash or an up special on the ground, otherwise it jumps (a tap is a short hop);
- the key of the d-pad's up is the taunt key: with a direction held it gives the taunt of that side, as in SSF2.

Measured on Mario only (the stick thresholds are expected to be the same for every fighter). The game's own stick jump
must be on (its default) for up to jump.

## On-screen keyboard

When the game asks for text (a name, for example) a Switch-style keyboard is drawn over the picture (Vulkan renderer).
Controller: d-pad or left stick move, **A** types the key, **B** deletes, **Y** space, **X** symbols page, **L / R** move the
text cursor, **ZL** or the left stick button = shift, **+** confirms, **-** cancels. The PC keyboard works too: type,
Backspace/Delete, Left/Right/Home/End, Enter confirms, Esc cancels. `SSBU_SWKBD=dialog` uses the old Windows dialog
instead (the OpenGL path always does).

## Third-party code

Dear ImGui (MIT, https://github.com/ocornut/imgui) is compiled into the runtime when found, as described above; its licence
text stays in the downloaded folder.

## Troubleshooting

- `[tools] FAILED: ... the Microsoft C++ Build Tools, which are missing`: read the sentence before it first: it says why
  the prebuilt runtime is not used (your copy of the project is older or newer than the package, an incomplete copy, the
  download failed). Fixing that is usually enough (no Build Tools needed). Before 2026-10-20 this message also came on
  every PC where LLVM was not installed yet, wrongly: the installer took the probe's "runtime package: ..." line for a
  refusal. If you do need the Build Tools (about 3 GB on C:, 5-15 minutes, administrator rights), press **Install Build
  Tools** in the installer window, or run in a Command Prompt
  `winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`
  (`--passive` shows Microsoft's progress window; the old `--quiet` showed nothing and could wait on a hidden prompt).
- Not enough space on C:: choose another **Install location** in the installer window (another drive); it is
  remembered, and Resume goes on there. A build already made elsewhere is not moved: a new location starts again.
- `"build_glslang.bat" is not recognised` / `vswhere.exe is not recognised`: Visual Studio Build Tools are missing or
  incomplete. Install "Build Tools" with the "MSVC C++ x64/x86 build tools" component and a Windows SDK. The script finds
  `vswhere.exe` at `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\` and calls the batch file by its absolute path;
  a harmless "vswhere" notice from `vcvars64.bat` in `build/logs/link.log` can be ignored when the DLL is produced.
- `missing input: ...main.npdm`: the ExeFS dump lacks `main.npdm`; dump the whole ExeFS.
- `main is not a build this port knows` / `sdk is not a build this port knows`: not 13.0.4 or 13.0.5 with the update applied.
- `the list of SDK functions to lift was not found`: `tools/lifter/sdklift_roots.txt` is missing from your clone.
- No music (the menus and fights are silent apart from some effects), and the run's log says `nn::codec: libopus NOT
  FOUND`: there is no libopus beside the exe; see libopus under "What you need", then run `--redo link`.
- Black window at the start, and the log says `MFCreateSourceReaderFromByteStream c00d36c4`, `no Media Foundation decoder
  path` or `no Vorbis decoder on this PC`, then `no FFmpeg DLLs`: Windows cannot play the movies and the FFmpeg fallback
  is missing. Run the installer again (it fetches FFmpeg into `deps\ffmpeg`; or `python scripts/fetch_media.py`, then
  `--redo link`), or install Web Media Extensions (2.x) and VP9 Video Extensions from the Microsoft Store; meanwhile
  `--env SSBU_SKIP_MOVIES=1` skips every movie.
- DLC not found: the add-on list is `build\config\dlc.txt` (written by `tools/audit/dlc_list.py`, whose default output is
  that file; it reads the file names of your own DLC dumps).
- A crash: the console report is also written to `<exe>_crash.txt` and a minidump to `<exe>_crash_last.dmp` beside the
  exe; attach the `.txt` (and the log) to an issue.
- A compiler job that fails after a power loss or while the PC is under load: run the same command again; only what
  changed is rebuilt. Closing a running game first matters: a running exe holds its DLLs open and the link fails.
- Output with non-ASCII characters from Windows tools is shown with `?`; the logs in `build/logs` are the originals.
