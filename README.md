> [!IMPORTANT]
> **This project has moved.** WindowSmash and ssbu-recomp are now one project, **[OpenSmash-Ultimate](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate)**: new versions, issues and pull requests go there. Join the merged Discord: **[discord.gg/pkgyYGf3YB](https://discord.gg/pkgyYGf3YB)**. This repository is kept for its history.

<div align="center">

<img src="media/og-image.png" alt="OpenSmash-Ultimate" width="100%">

# OpenSmash-Ultimate

**Super Smash Bros. Ultimate as a native PC program: recompiled, not emulated.**
One-click installer, Vulkan renderer, in-game settings, fighters as readable C++ source, online and rollback in progress.

[![Discord](https://img.shields.io/badge/Discord-join-5865F2?logo=discord&logoColor=white)](https://discord.gg/pkgyYGf3YB)
[![Website](https://img.shields.io/badge/website-denkishi.github.io-informational)](https://denkishi.github.io/opensmash-site/)
[![Download](https://img.shields.io/badge/download-ZIP-brightgreen?logo=github)](https://github.com/Denkishi/ssbu-recomp/archive/refs/heads/main.zip)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-lightgrey)
![Game versions](https://img.shields.io/badge/game-13.0.4%20%7C%2013.0.5-green)

### [Install](#install-in-3-steps) &nbsp;|&nbsp; [Website](https://denkishi.github.io/opensmash-site/) &nbsp;|&nbsp; [Discord](https://discord.gg/pkgyYGf3YB)

</div>

> The project is called **OpenSmash-Ultimate**. The repository is still named `Denkishi/ssbu-recomp`.

> **This repository contains no game code, no game assets, no keys and no firmware.**
> You need your own legally obtained copy of the game (13.0.4 or 13.0.5). Everything derived from it is generated on
> your PC, from your dump, and never leaves it. The only game-related text in the tree is names: a list of
> system-library function names chosen for translation, and function names used by the tools.

## What is it?

OpenSmash-Ultimate is a **native Windows port made by static recompilation** of *Super Smash Bros. Ultimate*
(Nintendo Switch, version 13.0.4 or 13.0.5). The game's AArch64 machine code is translated to C ahead of time and
compiled for your PC. There is no CPU interpreter and no JIT while you play. The console's system libraries, graphics
API, audio, video, input and file system are re-implemented by this project's own runtime, and the picture is drawn
with **Vulkan**.

It starts like any PC game, runs in a 16:9 window (F11 for borderless fullscreen), takes any controller, loads its
shaders at start, and its fighters can be read and changed as C++ source.

## Why it is different

- **Native code instead of emulation.** The CPU work of "pretending to be a Switch" is gone because the game's code
  is already x64 when you start it. We make no claim of being faster than any specific project on your hardware; we
  publish what we measured and how (59-60 fps in fights on an NVIDIA GPU and on an Intel integrated GPU).
- **API-level graphics.** The game's graphics API (NVN) is translated to Vulkan, and the game's shaders are translated
  when loaded and prepared on background threads. This keeps the renderer small and tunable. General emulators that
  reproduce a GPU in detail are more faithful to unusual cases; the two approaches serve different goals.
- **Source where people edit.** Fighter scripts become readable C++ that is checked against the original in
  differential tests. That is what a real mod platform and a rollback netcode need: the simulation as code and data you
  can read.
- **Install without ceremony.** One double-click, no Visual Studio, no firmware dump.

We learned from many open-source projects (see [Credits](#credits)) and respect all of them.

## Install in 3 steps

You need **your own game dump and keys**. That is all: the installer fetches everything else.

1. **Download** the game's ZIP: **[ssbu-recomp-main.zip](https://github.com/Denkishi/ssbu-recomp/archive/refs/heads/main.zip)** (the same as the green
   *Code* button > *Download ZIP*), and unpack it, or `git clone` it. **Not** the files under *Releases*: those are build
   tools the installer downloads by itself.
2. **Double-click `Install SSBU.bat`.** It sets up a private Python inside the folder (nothing is installed on your
   system), checks your PC, and **finds your keys and games by itself** in the folders of yuzu, Ryujinx, suyu, sudachi
   and eden. Check what it found and press **Install**: it applies the update and DLC, uses the **prebuilt runtime** from
   a GitHub Release, downloaded by itself (so **no Visual Studio**),
   and builds the game files on your PC. **No firmware is needed**: open-source fonts are included. It makes
   shortcuts, updates itself, and if it stops, double-click it again and it resumes.
3. **Double-click `Play SSBU.bat`** (or the desktop shortcut). Your saves go to `build\saves`.

Full guide, requirements and the same build done by hand: **[BUILDING.md](BUILDING.md)**.

## Features

Status key: **Done** = works and was tested on the development PC; **Experimental** = works, not widely tested or off
by default; **In progress**; **Planned**.

| Feature | Status |
|---|---|
| Native x64 program by static recompilation (AArch64 to C), game versions 13.0.4 and 13.0.5 | Done |
| One-click installer: private Python, PC check, finds keys and games in emulator folders, update + DLC, prebuilt runtime (no Visual Studio), no firmware needed, shortcuts, self-update, resume | Done |
| Vulkan renderer (OpenGL fallback), shaders loaded at start, 16:9 window, F11 borderless | Done |
| In-game settings menu (F1, controller Home, or − and + held 1 s; on by default): graphics, window / fullscreen / borderless, volume, full key and button remapping for keyboard and controllers per player, log viewer and export, F2 stats overlay | Done |
| On-screen keyboard for controllers (no Windows dialogs) | Done |
| Any controller through SDL3 (hot-plug, up to 8 players, rumble, gyro) | Done in automated tests with virtual devices; physical pads not yet confirmed |
| Online play through the game's Local Wireless over a LAN, a room code or a lobby server ([docs/ONLINE.md](docs/ONLINE.md)) | Experimental |
| Rollback netcode: input queues, wire format, time sync and a ring of game-state snapshots (up to 7 frames) are done; frames that run only the game logic come next | In progress |
| Run-ahead to lower input delay | Experimental (off by default) |
| Fighters' code as C++ source: 85.0% of the per-fighter status scripts (20,256 of 23,834 functions, all 96 fighter modules) equal to the original offline; 489 shared state-machine functions proven on real calls | Done for the published part; 100% in progress |
| Function hook table: replace any game function from a mod DLL without rebuilding the game | Done (self-tested) |
| New fighter kinds and single-file fighter packages ([docs/NEW_FIGHTER.md](docs/NEW_FIGHTER.md)) | In progress |
| Deterministic mode with a per-frame state hash log, and a debug fast mode, for testing | Done |
| Freeze watchdog: a report with the stack of every thread | Done |
| Fixed 60 fps in every mode | In progress |
| Items and stages as source | Planned |
| Native Linux build ([BUILDING_LINUX.md](BUILDING_LINUX.md)): movies, menus, a whole match at 60 fps with sound on one PC (CachyOS, NVIDIA, Wayland); F1 menu built (not yet tried on Linux), no online play there yet; Steam Deck not tested | In progress |

### Game modes (measured on the development PC)

| Mode | State |
|---|---|
| Boot, opening movie, title, menus | works |
| Smash (1v1 vs CPU), whole match to the results screen | works |
| Classic Mode, Training, Home-Run Contest | start and play (scripted input only) |
| Spirits, Vault, options, Mii Fighters | reachable |
| Adventure (World of Light) | intro reached; map and fights not tested |
| Team battles, Squad Strike, Tourney, 3-8 players, Stage Builder editing | not tested |

### Performance, as measured

Vulkan pipelines are prepared ahead by background threads from the game's shader containers, with a loading barrier:
**0 pipeline holds in fight frames** in the first-run tests on an NVIDIA and an Intel GPU (one scripted fight, and three
more with other fighters and stages in the fast test mode). A whole match runs on an Intel Raptor Lake iGPU at 59-60 fps.
Not shown: every window clean on a cold first run (NVIDIA: 6 frames of 34-38 ms at the very start of one of two
fights), and a pixel comparison with the OpenGL renderer. The preparation takes about 15 minutes of play on a first
run and costs disk (about 2.5 GB of shader files, up to 0.9 GB of pipeline cache). A freeze that was reported once
was not reproduced in long runs; its cause is **not established**.

## Screenshots

<!-- OWNER: add your own captures here (media/*.png or .gif), taken from your own game. Only our own captures: nothing
     from Nintendo. Ideas: a fight with the F2 overlay, the F1 settings menu, the installer window, the menus.
     Example: ![The settings menu](media/settings.png) -->

![The menus running natively](media/menus.webp)

## How it works

```
your dump --> tools/extract --> main executable, SDK, script modules, data
                 |
                 v
        tools/lifter (recomp.py)        AArch64 -> C, one C function per game function
                 |                       checked against Unicorn on 32,043 instruction cases
                 v
        clang --> game DLL  +  one DLL per script module
                 |
                 v
        runtime/                         what the console provided, re-implemented:
          hostrt, os_threads             loader, threads, memory, C++ exceptions through lifted code
          nn*.c                          ~1,100 system-library entry points (files, time, audio, input,
                                         movies, fonts, applets, accounts, network)
          nvn_vk*.c, nvn_gl.c            the console's graphics API (NVN) on Vulkan / OpenGL
          mx_translate.c                 the game's GPU shader machine code -> GLSL -> SPIR-V
          nnpad_sdl.c                    controllers through SDL3
          nnpad_gca.c                    GameCube controller adapter over WinUSB (shown to SDL as virtual gamepads)
```

- **Game code** is recompiled, not emulated: a mechanical translation, complete and exact, but not readable source.
- **System calls** are re-implemented on Windows, the way Wine re-implements Windows calls.
- **Graphics** are translated at the API level, and the shaders when they are loaded.

### Readable source where people edit

A fighter's moves are small scripts. `fighter_sdk/tools` decompiles them into C++ like this (generated on your PC from
your dump; not stored in this repository):

```cpp
ACMD(mario, game_attack11) {
    frame(agent, 2);
    if (is_excute(agent)) {
        ATTACK(agent, 0, 0, hash40("top"), 2.2f, 361, 30, 0, 20, 1.2f, 0.0f, 6.3f, 6.0f, /* ... */);
    }
    wait(agent, 2);
    if (is_excute(agent)) {
        AttackModule::clear_all(boma);
        WorkModule::on_flag(boma, FIGHTER_STATUS_ATTACK_FLAG_ENABLE_COMBO);
    }
}
```

On the development PC the tool turns 80,880 of 84,886 animation-command scripts of all fighters (95%) into source that
behaves like the original in a differential test; a fighter rebuilds in about a second. Eight fighters (Mario, Luigi,
Link, Samus, Fox, Captain Falcon, Donkey Kong, Ness) run their status source in deterministic matches with identical
per-frame logs against the lifted version (2,010 fight frames each). More: [docs/MODDING.md](docs/MODDING.md).

## Roadmap

| # | Goal | State |
|---|---|---|
| 1 | Rollback online with our own network layer: input queues and snapshot ring done, logic-only frames next, then direct connect by code and lobby | In progress |
| 2 | Zero extra input delay (run-ahead made to fit the frame) | In progress |
| 3 | Fighters 100% as source | In progress |
| 4 | Items and stages as source | Planned |
| 5 | Fixed 60 fps in every mode; every mode playable start to end | In progress |
| 6 | Single-file fighter packages (drop one file in the characters folder) | In progress |
| 7 | Controllers confirmed on physical pads | In progress |
| 8 | Linux / Steam Deck (a native Linux build runs on one tested PC: [BUILDING_LINUX.md](BUILDING_LINUX.md)) | In progress |

## FAQ

**Is this legal?** The repository contains no game code, assets, keys or firmware, and does not help anyone obtain them.
You build the program on your PC from a copy of the game you own. This is not legal advice; laws differ by country.

**Do I need a Switch?** To make your own dump of the game and keys you need a console you own that can run dumping
tools, or an existing dump you made legally. This project cannot do that step for you.

**Does it include the game?** No. Not the game, not the keys, not the firmware, and no links to them.

**Which GPU do I need?** Any GPU with a Vulkan 1.3 driver. An integrated Intel Raptor Lake GPU played a whole match at
59-60 fps in our tests. Windows 10/11 x64. The movies play with Windows' own VP9 Video Extensions and Web Media
Extensions (free, Microsoft Store) when they work, otherwise with FFmpeg, which the installer downloads.

**Can I play online?** Experimental. The game's Local Wireless mode works over a LAN, a room code or a lobby server:
[docs/ONLINE.md](docs/ONLINE.md). Rollback netcode is in progress and is not available yet.

**Which game versions?** 13.0.4 and 13.0.5 ([docs/VERSIONS.md](docs/VERSIONS.md)).

**How do I use my own save file?** With the game closed: the game keeps its saves in `build\saves\save\` (or
`<your install location>\saves\save\` if you chose one in the installer), as a `save_data` folder holding
`system_data.bin`, `mii`, `spirits` and `stage`. Get your save as a folder that contains `save_data`: from your Switch
with JKSV or Checkpoint, from yuzu / Eden / suyu (right-click the game, "Open Save Data Location") or from Ryujinx
(right-click the game, "Open User Save Directory"). Rename `build\saves\save` to `save_old` as a backup, create a new
empty `build\saves\save` and copy your `save_data` folder into it (you should have
`build\saves\save\save_data\system_data.bin`), then start the game. Folders named `save.work.<number>` are temporary
session files: leave them or delete them while the game is closed.

**"ssbu_game_eh_o1.dll is missing" when I start the game?** The game DLL (about 500 MB, built on your PC and not signed) was removed after the installation, usually by an antivirus, or the installation stopped before the end. Open Windows Security > Virus & threat protection > Protection history and restore/allow the file if it is there (adding your install folder to the exclusions stops it happening again), then double-click `Install SSBU.bat` and press **Resume**: it links the missing files again in a few minutes, without compiling everything again.

**Mods and new fighters?** Mods through the function hook table and C++ fighter source work today; new fighters are in
progress ([docs/NEW_FIGHTER.md](docs/NEW_FIGHTER.md)).

**Was this built with AI?** Yes, see below.

## How this was made

**This project was built with AI, and that is part of the point.**

I am not a reverse engineer or a security engineer, and I did not come up through whatever field normally teaches this.
Almost all of the code here was written by AI coding agents (Claude) that I direct: I decide what to try, think up the
angle of attack or the workaround, and the AI implements it quickly and comes back with a result I can check. That loop
(an idea, an implementation within the hour, a measurement that says whether it held) is what made a project of this
size possible for one person.

What I take from it: the bar for entry has dropped a great deal. The creativity is still yours (which problem to solve,
which trick might get around it, when a result is not good enough), but the distance between having the idea and seeing
whether it works is now very short.

Two rules kept it honest: every claim has to come from a test or a measurement (this README says what was measured and
what was not), and nothing derived from the game goes into the repository. It is a study project; expect rough edges,
and mistakes that a specialist would not have made.

## Contributing

Testers with other GPUs and controllers, and people who know the game's internals or the modding scene's tooling, are
very welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) first: three rules (nothing derived from the game in the
repository, clean sources only, every claim measured) apply to every contribution. Questions and bug reports: the
[Discord](https://discord.gg/pkgyYGf3YB) or GitHub issues (attach the log from the settings menu and your
`config\graphics.ini`).

## Credits

- **Denkishi**, maintainer.
- **xtlg511**: support for game version 13.0.5, the libopus build step, a `run.py` fix.
- **jfb112697**: a `setup.py` check fix.
- **SNO2207 / WindowSmash**: parts of the online network code and the lobby design are adapted with the author's
  permission (see [docs/ONLINE.md](docs/ONLINE.md)); the installer's approach was inspired by its tools (nothing copied).
- **yuzu and suyu**: the open-source system fonts shipped in `runtime/fonts_open` ([README](runtime/fonts_open/README.md)).
- **Slippi and Hero88go/melee-unlocked**: the public design of rollback netplay that guides our netcode.
- **sbergeron42/ssbu-decomp**: the matching-decompilation effort (our work on it is paused).
- yuzu and its successors, SDL, glslang, libopus, Unicorn, Capstone, the N64 static-recompilation projects and
  BelmanteGu/SuperMarioWonderRecomp, from which technique was learned.
- The Smash modding community (param labels, skyline-smash, ARCropolis) for names of script functions and constants.

## Legal

OpenSmash-Ultimate is not affiliated with, endorsed or sponsored by Nintendo, Bandai Namco or Sora Ltd. *Super Smash
Bros.* and *Nintendo Switch* are trademarks of their owners and are named only to say what the software works with.
The repository contains no copyrighted game data, no console keys and no firmware, and does not help anyone obtain
them. **You must own the game and dump it yourself.** No Nintendo logos or artwork are used.

Licensed under the GNU General Public License, version 3 or later (see [LICENSE](LICENSE)).
