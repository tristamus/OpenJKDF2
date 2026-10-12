# jkdf2mp: Jedi Knight multiplayer in your browser

**Play: https://jkdf2mp.jkdf2mp-lobby.workers.dev/** (desktop Chrome or Firefox, plus your own copy of the game)

This fork of [OpenJKDF2](https://github.com/shinyquagsire23/OpenJKDF2) lets you play **Star Wars Jedi Knight: Dark Forces II** and **Mysteries of the Sith** multiplayer in a web browser. Open the page, pick the folder where the game is installed, then host a match or join one from the game's own Multiplayer menu. There's nothing to install and no port forwarding to set up.

jkdf2mp is a free fan project. It isn't affiliated with or endorsed by Lucasfilm or Disney.

## Bring your own copy of the game

- **No game files come with it.** This repository, the web build and the website contain no files from the original games. You need the game installed on your computer, for example from [Steam](https://store.steampowered.com/app/32380/STAR_WARS_Jedi_Knight_Dark_Forces_II/) or [GOG](https://www.gog.com/game/star_wars_jedi_knight_dark_forces_ii).
- **Nothing is uploaded.** When you pick your game folder, the page reads the files inside your browser tab and loads them into the game there. They aren't sent to the lobby server. Your browser may call this "upload"; that's only its wording for letting a page read a folder.
- **The one exception is custom maps,** and even then no original game files are sent. See [custom maps](#custom-maps-download-automatically) below.

## What it adds to OpenJKDF2

OpenJKDF2 already had a WebAssembly (browser) build, but it had no networking, and it needed the game files baked into the download, which can't be shared publicly. This fork adds the following.

### Online multiplayer

- **Host or join in the browser.** The Join screen lists every game on the lobby, with its map, episode and player count.
- **Direct connections, with a fallback.** Once you join, your browser links straight to the host's browser over WebRTC and game traffic goes over that link. If a direct link can't be made, traffic goes through the lobby server instead, so you can still play, with more lag.
- **An always-on lobby.** The lobby runs on Cloudflare's free Workers plan, so it's up without anyone's PC being on. It lists games, helps browsers connect to each other, and relays traffic for players who can't connect directly. It never stores or serves game files. Its source is in [jkdf2mp-bb-server](https://github.com/tristamus/jkdf2mp-bb-server), which also has `relay.py`, a one-file Python lobby for running your own on your PC or local network.

### Mysteries of the Sith

- Pick your Mysteries of the Sith folder instead of Jedi Knight's and the expansion starts.
- Lightsaber blades and some character skins used to draw wrong. Mysteries of the Sith uses 16-bit textures, which the browser's WebGL 2 rejects in the format the engine sends them. The browser build now converts them first, so they look the same as on desktop.

### Your characters and controls are kept

- When you pick your game folder, the characters in its `player` folder come in with their controls and weapon settings. This works for both games.
- Changes you make in the browser are saved in the browser and are there on your next visit. If a character is in both places, the browser's copy wins.
- Display settings you set in the browser (Setup > Display: field of view, gamma, HUD scale and the rest) are kept for your next visit too. The original games' own display and sound settings don't carry over, because they keep them in the Windows registry rather than the game folder.

### Custom maps download automatically

- If the host is playing a custom (community-made) map you don't have, or a different version of it, your browser gets it from the host's browser before the level loads, over the direct link or, if that isn't up, passed through the lobby without being stored there. A panel shows the download's progress, or why it couldn't happen.
- The map is saved in your browser, so next time it loads right away and you can host it too.
- **No original game files are sent.** The original episodes are never offered. Any file inside a custom map that's an exact copy of a file from the original game is left out, and your browser fills it in from your own install. The rebuilt map must match the host's copy exactly (same SHA-256) before it's used. A map made only of original game files isn't sent at all.

### Mods

- Put each mod in its own folder inside your game folder, put its GOB in `Resource`, or turn it on in JK Patch Commander. After you pick the game folder, the page lists every mod it found, all ticked, and you untick any you don't want this time. Each GOB in `Resource` or `mods` that isn't the game's own, and each of JK Patch Commander's active patches, is a mod of its own. So is each GOB and each mod folder in a folder that collects several mods, like a `mod` folder, and that folder's own loose files are one more. A mod can also be a zip file anywhere a mod folder can be, or in `Resource`: the page opens it in the browser as if it had been unzipped there. They only load when ticked, so a host always shares everything its game loads.
- Mysteries of the Sith mods work the same way. The [MotS Remastered](https://www.moddb.com/mods/jedi-knight-remastered) fan pack is treated like the game's own files instead: its `JKMRemasteredAssets.goo` in `Mods` loads for whoever has it, isn't listed, and isn't sent to joiners, so players without it play with different files. Its cutscenes folder, `jkmcutscenes`, is skipped like the game's videos.
- Several ticked mods run together. When two of them change the same file, the one higher in the list wins, and the list says which files and which mod. JK Patch Commander's patches come first, then mod folders, then GOBs in `mods` and `Resource`, the order the original game gives them. To joiners, the ticked mods are one mod, named after them ("Saber Battle X - JK + 2 more"), so a joiner that ticked the same mods downloads nothing and anyone else gets the combination.
- If you host with a mod, players who join get it from your browser the same way as a custom map: files from the original game are left out and filled in from their own copy, and the result must match your mod exactly (same SHA-256). Mods up to 100 MB are sent.
- If you join a game with a different mod, or none, the game restarts in the same tab with the host's, and you join again from Multiplayer with that game already selected. The join screen says which mod a game uses.
- Mods you got from a host are saved in your browser, so next time they load without a download and you can choose them to host.
- Tested with Saber Battle X between two browsers: the in-tab restart, a joiner getting the mod directly and through the lobby, and switching back to no mod. Ticking several mods was tested in headless Chromium with SBX and made-up mods, and in a sharing test of the combination, but not yet between two browsers.

### Screenshots

- Press F12 in a game, or the key you set for screenshots, and the game saves exactly what's on screen, HUD included. Once the picture is stored it says "Screenshot saved to Your Screenshots on the main page", or that it wasn't saved. This works in both games.
- The pictures are kept in your browser. The landing page lists them under Your Screenshots, newest first, where you can open each one full size, download it or delete it, or download them all as a zip. Clearing the site's data in your browser deletes them, so download the ones you want to keep.
- Before this, F12 said "Screenshot taken" but saved nothing: the browser build had no way to write the picture.

### A landing page and clearer errors

- A landing page in the style of early-2000s game lobby sites, with a Play button, "rooms", getting-started steps and a plain explanation of what happens to your files. Every room opens the same game; you host or join inside it.
- The page tells you when your browser can't run the game, and why: a phone or tablet (they can't pick a folder), or missing WebAssembly or WebGL 2. If the game hits an error, it shows over the game instead of in a status line hidden behind it.
- The browser build is multiplayer-only, so the main menu hides Single Player, View Cutscenes and Expansions & Mods. Choose Player, Credits and Setup share one row along the bottom.

### Bug fixes

- **Original maps missing from Host.** Windows installs spell some folders differently (`Episode` instead of `episode`), and the browser's file system is case-sensitive, so the original multiplayer and Capture the Flag episodes could vanish from the Host list. Every folder is now spelled one way.
- **A player stuck lying down.** Someone who joined while another player was dead saw that player running around lying down after they respawned. The fix is in shared engine code, so desktop builds get it too.
- **Files skipped by the file search** on Linux, macOS and the browser: a file whose name sorts before `.`, such as `!dm.gob`, was dropped. Also shared with desktop builds.
- **Firefox couldn't connect** to a lobby run with `relay.py`, because its WebSocket reply used HTTP/1.0, which Firefox refuses. It now uses HTTP/1.1.
- **The browser build compiles from a clean checkout** (it used to stop with "Unknown platform!"), no longer bundles game files, and has a GitHub Actions workflow that builds it.

## Good to know

- **Desktop only** ...unless you can get the files needed onto mobile devices! Use a current Chrome or Firefox on a computer with WebGL 2.
- **No music or cutscenes.** OpenJKDF2's browser build turns off the soundtrack and skips cutscenes.
- **Game passwords aren't private.** A game's password is sent along with the game list and checked in the joining player's browser, as in OpenJKDF2's desktop networking. Don't use a password you use anywhere else.
- **IP addresses are shared.** A direct WebRTC link means the host and each player who joins learn each other's IP address.
- **No TURN server.** Players whose network blocks a direct link play through the lobby, with more lag.
- **Free-plan limits.** Cloudflare's free plan caps the lobby's requests per day. If a busy day hits the cap, the lobby stops answering until midnight UTC.
- **Browser players can't play with desktop players yet.** See below.

## Planned

- **A bridge to desktop players (DirectPlay).** There's a written plan for a small bridge program so OpenJKDF2 desktop players can join browser games, and possibly the original `JK.EXE` after that. Nothing is built yet. This would cost me money, so I won't move forward with this unless I see a lot of traction with this game.

## Where the code is

Everything above is merged into the `web-mp` branch, and the live site is built from it. Each change came in as its own pull request:

- [#1](https://github.com/tristamus/OpenJKDF2/pull/1): browser build from a clean checkout, game folder picker, folder-spelling and file-search fixes
- [#2](https://github.com/tristamus/OpenJKDF2/pull/2): lobby networking over WebSockets
- [#3](https://github.com/tristamus/OpenJKDF2/pull/3): direct WebRTC connections, the lying-down fix, browser checks
- [#4](https://github.com/tristamus/OpenJKDF2/pull/4): characters and controls kept in the browser
- [#9](https://github.com/tristamus/OpenJKDF2/pull/9): the landing page
- [#5](https://github.com/tristamus/OpenJKDF2/pull/5): Mysteries of the Sith 16-bit textures
- [#10](https://github.com/tristamus/OpenJKDF2/pull/10): Single Player hidden from the main menu
- [#6](https://github.com/tristamus/OpenJKDF2/pull/6): custom map downloads
- [#8](https://github.com/tristamus/OpenJKDF2/pull/8): the landing page says custom maps you host are shared
- [#12](https://github.com/tristamus/OpenJKDF2/pull/12): View Cutscenes and Expansions & Mods hidden from the main menu
- [#13](https://github.com/tristamus/OpenJKDF2/pull/13): mods, several at a time, shared with joiners
- [#15](https://github.com/tristamus/OpenJKDF2/pull/15): display settings kept in the browser between visits
- [#17](https://github.com/tristamus/OpenJKDF2/pull/17): screenshots saved in the browser and listed on the landing page
- The lobby's code, including the Firefox fix, is on the `main` branch of [jkdf2mp-bb-server](https://github.com/tristamus/jkdf2mp-bb-server), and each deployed build is kept on its [`builds`](https://github.com/tristamus/jkdf2mp-bb-server/tree/builds) branch.

To build the browser version yourself, see the Emscripten/WebAssembly section of `BUILDING.md`.

## Credits

The engine is [OpenJKDF2](https://github.com/shinyquagsire23/OpenJKDF2) by shinyquagsire23 and its contributors, a function-by-function reimplementation of Jedi Knight: Dark Forces II in C. This fork adds the browser multiplayer pieces and a few fixes on top of it. Star Wars and Jedi Knight are trademarks of Lucasfilm Ltd.

Everything below this line is OpenJKDF2's own README.

---

# OpenJKDF2

![MacOS Screenshot](docs/images/screenshot.png)

## [Latest Releases](https://github.com/shinyquagsire23/OpenJKDF2/releases) | [Report a crash or bug](https://github.com/shinyquagsire23/OpenJKDF2/issues)

OpenJKDF2 is a function-by-function reimplementation of DF2 in C, with 64-bit ports to Windows 7+, macOS 10.15+, and Linux. Files are organized as closely to the original game as possible, based on symbols from the Grim Fandango Remaster Android/Linux/macOS port, as well as scattered assertions from various other games. It also contains the original versions of `byacc` and `flex` used for COG script parsing.

OpenJKDF2 does *not* include any original game assets; a valid copy of JKDF2 is *required* and can be purchased from [GOG](https://www.gog.com/game/star_wars_jedi_knight_dark_forces_ii) or [Steam](https://store.steampowered.com/app/32380/STAR_WARS_Jedi_Knight_Dark_Forces_II/). The GOG version is recommended, since it is DRM-free and also includes the soundtrack in Ogg Vorbis format. If you'd like to try before you buy, a WebAssembly demo of OpenJKDF2 can be found at https://maxthomas.dev/openjkdf2/.

Support for playing the original soundtrack from Ogg Vorbis files is primarily supported for the GOG and Steam versions of the game assets. Original disk soundtracks can also be loaded from `MUSIC/1/Track<2..8>.ogg` and `MUSIC/2/Track<2..12>.ogg` for each disk's soundtrack. If files are missing, it will instead attempt to use a GOG track number from `MUSIC/Track<12..32>.ogg`. Dumping the soundtrack from disks at install time is planned for a future release of OpenJKDF2, but is not currently implemented.

## Platforms
OpenJKDF2 supports the following configurations:

| Configuration | Renderer | Description |
| --- | --- | --- |
| 64-bit Windows/SDL2 | OpenGL 3.3 | 64-bit Windows compilation with SDL2 and OpenAL. DirectX dependencies are replaced with SDL2 and OpenAL. |
| MacOS x86_64/AArch64 | OpenGL 3.3 | 64-bit MacOS compilation with SDL2 and OpenAL Soft. All release packages include both Intel and ARM64. |
| 64-bit Linux/SDL2 | OpenGL 3.3 | 64-bit Linux compilation with SDL2 and OpenAL. |
| ARM64 Android | OpenGL ES 3 | ARMv8 Android compilation with SDL2 and OpenAL Soft. |
| Nintendo DSi | Custom | Nintendo DSi port with hardware rendering, software audio. Extremely RAM-limited, supports emissive palettes. |
| Dreamcast | Custom | KallistiOS+Dreamcast port with basic 3D rendering (no emissive palettes), HW accelerated audio including soundtrack and cutscenes. Extremely RAM-limited. |
| 32-bit Windows/SDL2 | OpenGL 1.1 | Windows compilation with SDL2 and OpenAL Soft. DirectX dependencies are replaced with SDL2 and OpenAL. Targeting Windows XP ~ Windows 7 |
| Emscripten/WebAssembly | WebGL 2/OpenGL ES 3 | WebAssembly with SDL2 and OpenAL. Runs in a web browser. Since WASM only supports 32-bit pointers, this will likely be less buggy than 64-bit, but less performant. |

The following implementations are in-progress or planned:

| Configuration | Renderer | Description | Status |
| --- | --- | --- | --- |
| iOS | Metal? | Not a huge priority, but would be nice to have. | Not started |
| Switch libnx | OpenGL ES 3 | Not a huge priority, but would be nice to have. | Not started |
| 32-bit Windows/SDL2 | OpenGL 3.3 | Windows compilation with SDL2 and OpenAL. DirectX dependencies are replaced with SDL2 and OpenAL. Targeting Windows XP ~ Windows 7 | Not started |
| 32-bit Windows/DirectX | Direct3D 3 | Faithful decompilation with original DirectX bindings/renderer. | Not started |

The following configurations are deprecated:

| Configuration | Renderer | Description |
| --- | --- | --- |
| x86 Linux/SDL2, mmap blobs | OpenGL 3.3 | 32-bit Linux compilation with SDL2 and OpenAL. JK.EXE is memory mapped into the process and used as a "binary blob"; Unimplemented functions will fall back to JK.EXE implementations. |
| 32-bit Linux/SDL2, blobless | OpenGL 3.3 | 32-bit Linux compilation with SDL2 and OpenAL. The output executable is a swap-in replacement for JK.EXE, but will be missing functions and will crash on reaching unimplemented code. |
| x86 Win32/MinGW DLL | Software/DirectX | Win32 hooked build, JK.EXE is patched to load `df2_reimpl.dll` execute `hook_init_win` before JK.EXE's `main` function. Unimplemented functions will fall back to JK.EXE implementations. `df2_reimpl_kvm.dll` is used for the KVM target |

Linux building works on AArch64/RPi4 with llvmpipe, but V3D GLES has trouble with palettes.

OpenJKDF2 requires game data from a licensed copy of Jedi Knight: Dark Forces II in order to run; No game assets are provided by OpenJKDF2. On Linux, paths and filenames may be case-sensitive. Your directory structure should look something like this:
```
.
├── JK.EXE
├── MUSIC
│   ├── Track12.ogg
│   ├── Track13.ogg
│   ├── Track14.ogg
│   ├── Track15.ogg
│   ├── Track16.ogg
│   ├── Track17.ogg
│   ├── Track18.ogg
│   ├── Track22.ogg
│   ├── Track23.ogg
│   ├── Track24.ogg
│   ├── Track25.ogg
│   ├── Track26.ogg
│   ├── Track27.ogg
│   ├── Track28.ogg
│   ├── Track29.ogg
│   ├── Track30.ogg
│   ├── Track31.ogg
│   └── Track32.ogg
├── episode
│   ├── JK1.gob
│   ├── JK1CTF.gob
│   └── JK1MP.gob
├── openjkdf2-64
├── player
└── resource
    ├── Res1hi.gob
    ├── Res2.gob
    ├── jk_.cd
    └── video
        ├── 01-02A.SMK
        ├── 03-04A.SMK
        ├── 06A.SMK
        ├── 08-10A.SMK
        ├── 12A.SMK
        ├── 16A.SMK
        ├── 18-19A.SMK
        ├── 21A.SMK
        ├── 23A.SMK
        ├── 25A.SMK
        ├── 27A.SMK
        ├── 33-34A.SMK
        ├── 36A.SMK
        ├── 38A.SMK
        ├── 39A.SMK
        ├── 41-42A.SMK
        ├── 41DA.SMK
        ├── 41DSA.SMK
        ├── 44A.SMK
        ├── 46A.SMK
        ├── 48A.SMK
        ├── 50A.SMK
        ├── 52-53A.SMK
        ├── 54A.SMK
        └── 57A.SMK
```

## Building

See [here](BUILDING.md) for instructions.

## Contributing

Contributions in the form of code cleanup and documentation are highly welcomed. See [CONTRIBUTING.md](CONTRIBUTING.md) for details on what kinds of cleanup tasks still need to be done. OpenJKDF2 is not currently accepting monetary donations, however [detailed bug and crash reports](https://github.com/shinyquagsire23/OpenJKDF2/issues) are always appreciated, including bugs/crashes involving mods.

## TL;DR: What Isn't Implemented, Yet
 - Load Configuration and Save Configuration in Setup > Controls > Options
 - Using plus or minus to resize the screen (with SDL2, resolution auto-resizes to window size)

## Usage with original JK.EXE and DirectX using hooks
See [here](HOOKS.md) for instructions.

## Methodology
The bulk of research and documentation occurs in IDA. Every function has been identified to a file prefix (ie `stdHashTable_`) with a corresponding .c/.h file. RenderDroid (`rd*`) and LEC stdlib (`std*`) functions are 90% canonically named, based on symbols from Grim Fandango Remastered.

Reverse engineering is a parallel effort between structure documentation and function identification. Once structures are sufficiently documented, Hex-Rays can be used for decompilation. While most Hex-Rays output works outright, many loops and structures require manual intervention. Output is generally cleaned and tidied to remove redunant stack variables or too-deep nesting. `sizeof` and obvious inlining and macros should also be adjusted as appropriate.

Engine variables and yet-to-be-decompiled functions are referenced using `define` macros and static function pointers, respectively. Once a file is decompiled enough that an engine variable is no longer referenced by non-decompiled code, the variables can be declared in their respective C files. For decompiled functions which are only referenced by non-decompiled functions, a `hook_function` call is added in `main.c` to redirect code execution to `df2_reimpl.dll` from `JK.EXE`.

Progress is tracked using `analyze.py`, `output.map` and `ida_copypaste_funclist_nostdlib.txt`: After compiling `df2_reimpl.dll`, symbols can be compared against the `.idb` to determine how much of the original `.text` is actually in use, and how much has been hooked and replaced.

If you'd like a copy of my IDB to examine functions which haven't been decompiled yet (or for any other use), let me know.

## Current Progress

Generated using `analyze.py`. Some filenames may be inaccurate or incomplete (see `ida_copypaste_funclist_nostdlib.txt` for a full function name listing).

```

[file]                         [size]     [% of text]   [% complete]  [decomp / total] 
DirectX                        0x1e       0.003%        100.000%        5 / 5          
jkGob                          0x29       0.004%        100.000%        2 / 2          
jkGuiDecision                  0x45       0.006%        100.000%        3 / 3          
sithStrTable                   0x5b       0.008%        100.000%        4 / 4          
sithCopyright                  0x67       0.010%        100.000%        1 / 1          
jkStrings                      0x89       0.013%        100.000%        5 / 5          
jkGuiGeneral                   0xc5       0.018%        100.000%        3 / 3          
jkSmack                        0xee       0.022%        100.000%        4 / 4          
rdFace                         0xf6       0.023%        100.000%        4 / 4          
jkGuiControlOptions            0x105      0.024%        100.000%        3 / 3          
stdHashKey                     0x107      0.024%        100.000%       10 / 10         
rdCanvas                       0x113      0.025%        100.000%        4 / 4          
jkGuiEsc                       0x18f      0.037%        100.000%        3 / 3          
rdroid                         0x1f6      0.047%        100.000%       27 / 27         
sithHeader                     0x1f9      0.047%        100.000%        1 / 1          
sithTime                       0x213      0.049%        100.000%        6 / 6          
jkGuiSingleTally               0x21b      0.050%        100.000%        4 / 4          
jkGuiSetup                     0x240      0.053%        100.000%        4 / 4          
jkGuiSound                     0x274      0.058%        100.000%        3 / 3          
sithItem                       0x275      0.058%        100.000%        5 / 5          
jkGuiGameplay                  0x2b2      0.064%        100.000%        3 / 3          
stdMemory                      0x2ba      0.065%        100.000%        7 / 7          
sithEvent                      0x2e3      0.069%        100.000%        9 / 9          
stdMci                         0x2ef      0.070%        100.000%        7 / 7          
jkGuiObjectives                0x308      0.072%        100.000%        4 / 4          
jkControl                      0x331      0.076%        100.000%        4 / 4          
Windows                        0x39a      0.085%        100.000%       11 / 11         
stdString                      0x3b3      0.088%        100.000%       11 / 11         
sithRenderSky                  0x3c2      0.089%        100.000%        5 / 5          
jkGuiDialog                    0x3e0      0.092%        100.000%        6 / 6          
sithModel                      0x3f1      0.094%        100.000%        8 / 8          
rdThing                        0x42f      0.099%        100.000%       12 / 12         
stdGdi                         0x430      0.099%        100.000%       10 / 10         
jkCutscene                     0x443      0.101%        100.000%        7 / 7          
sithKeyFrame                   0x44e      0.102%        100.000%        5 / 5          
stdPcx                         0x45e      0.104%        100.000%        2 / 2          
sithDSSCog                     0x460      0.104%        100.000%        4 / 4          
smack                          0x466      0.104%        100.000%        6 / 6          
sithPlayerActions              0x46f      0.105%        100.000%        3 / 3          
rdMath                         0x47d      0.107%        100.000%        6 / 6          
rdLight                        0x49f      0.110%        100.000%        8 / 8          
jkGuiMain                      0x4b1      0.111%        100.000%        6 / 6          
stdFnames                      0x4ee      0.117%        100.000%       14 / 14         
sithSprite                     0x4f1      0.117%        100.000%        6 / 6          
jkGui                          0x4fb      0.118%        100.000%       10 / 10         
jkGuiTitle                     0x4fb      0.118%        100.000%       10 / 10         
sithMaterial                   0x4fd      0.118%        100.000%        9 / 9          
wuRegistry                     0x5b2      0.135%        100.000%       12 / 12         
stdHashTable                   0x5d6      0.138%        100.000%       10 / 10         
Video                          0x5dc      0.139%        100.000%        5 / 5          
sithConsole                    0x5de      0.139%        100.000%       13 / 13         
sithExplosion                  0x61d      0.145%        100.000%        4 / 4          
sithAIClass                    0x689      0.155%        100.000%        7 / 7          
rdPrimit2                      0x69a      0.157%        100.000%        5 / 5          
sithCogScript                  0x6ca      0.161%        100.000%        9 / 9          
Window                         0x6db      0.163%        100.000%       13 / 13         
stdGob                         0x6dd      0.163%        100.000%       14 / 14         
jkAI                           0x6e7      0.164%        100.000%        5 / 5          
sithMain                       0x72b      0.170%        100.000%       16 / 16         
jkGuiControlSaveLoad           0x732      0.171%        100.000%        6 / 6          
jkGuiPlayer                    0x73a      0.171%        100.000%        5 / 5          
rdSprite                       0x76d      0.176%        100.000%        5 / 5          
stdConffile                    0x78d      0.179%        100.000%       13 / 13         
jkGuiMap                       0x793      0.180%        100.000%        8 / 8          
sithTemplate                   0x79d      0.181%        100.000%       10 / 10         
sithParticle                   0x7f5      0.189%        100.000%       10 / 10         
sithSector                     0x806      0.190%        100.000%       13 / 13         
sithComm                       0x80b      0.191%        100.000%       11 / 11         
sithMap                        0x814      0.192%        100.000%        6 / 6          
Main                           0x87b      0.201%        100.000%        4 / 4          
jkGuiMultiTally                0x8aa      0.206%        100.000%        7 / 7          
jkGuiSingleplayer              0x8d8      0.210%        100.000%        7 / 7          
jkCredits                      0x8e4      0.211%        100.000%        6 / 6          
sithCogFunctionSector          0x93a      0.219%        100.000%       22 / 22         
sithCogFunctionAI              0x943      0.220%        100.000%       20 / 20         
stdSound                       0x9bf      0.231%        100.000%       27 / 27         
sithGamesave                   0x9bf      0.231%        100.000%        7 / 7          
jkGuiForce                     0x9dd      0.234%        100.000%       11 / 11         
sithSound                      0xa00      0.237%        100.000%       13 / 13         
rdParticle                     0xa0d      0.239%        100.000%       10 / 10         
rdMaterial                     0xa2d      0.241%        100.000%        9 / 9          
sithSoundClass                 0xa46      0.244%        100.000%       16 / 16         
sithCogFunctionSound           0xa97      0.251%        100.000%       14 / 14         
rdKeyframe                     0xa99      0.251%        100.000%        8 / 8          
rdCamera                       0xaa8      0.253%        100.000%       26 / 26         
jkGuiSaveLoad                  0xb21      0.264%        100.000%        9 / 9          
rdPolyLine                     0xb42      0.267%        100.000%        6 / 6          
jkSaber                        0xb54      0.269%        100.000%        8 / 8          
jkGuiKeyboard                  0xb57      0.269%        100.000%       14 / 14         
jkGuiNetHost                   0xbc6      0.279%        100.000%        6 / 6          
sithOverlayMap                 0xbf8      0.284%        100.000%        9 / 9          
rdVector                       0xd29      0.312%        100.000%       55 / 55         
jkGuiMouse                     0xdb5      0.325%        100.000%       14 / 14         
DirectDraw                     0xdd4      0.328%        100.000%       16 / 16         
jkEpisode                      0xdd9      0.329%        100.000%       10 / 10         
sithCogFunctionPlayer          0xdf0      0.331%        100.000%       42 / 42         
jkHudInv                       0xe43      0.338%        100.000%        9 / 9          
sithCogFunctionSurface         0xe92      0.346%        100.000%       38 / 38         
sithActor                      0xf9c      0.370%        100.000%       13 / 13         
stdConsole                     0xfff      0.380%        100.000%       20 / 20         
jkGuiMultiplayer               0x1097     0.394%        100.000%       13 / 13         
DirectPlay                     0x10cc     0.399%        100.000%       31 / 31         
VBuffer                        0x10dc     0.400%        100.000%        4 / 4          
jkCog                          0x11b8     0.420%        100.000%       40 / 40         
stdControl                     0x11ec     0.425%        100.000%       22 / 22         
sithPuppet                     0x1222     0.430%        100.000%       17 / 17         
sithCamera                     0x124b     0.434%        100.000%       23 / 23         
jkGuiDisplay                   0x12ff     0.451%        100.000%       11 / 11         
jkGuiJoystick                  0x13f0     0.473%        100.000%       19 / 19         
jkDSS                          0x13f6     0.474%        100.000%       32 / 32         
sithCogYACC                    0x152b     0.502%        100.000%       10 / 10         
jkRes                          0x15b6     0.515%        100.000%       23 / 23         
jkMain                         0x16cd     0.541%        100.000%       53 / 53         
sithWorld                      0x1718     0.548%        100.000%       22 / 22         
sithDSS                        0x175d     0.554%        100.000%       22 / 22         
stdMath                        0x182a     0.573%        100.000%       23 / 23         
rdActive                       0x1a55     0.625%        100.000%        8 / 8          
sithCogExec                    0x1aec     0.639%        100.000%       31 / 31         
sithCogParse                   0x1b2a     0.645%        100.000%       26 / 26         
jkHud                          0x1c9b     0.679%        100.000%       17 / 17         
jkDev                          0x1fd6     0.755%        100.000%       41 / 41         
sithInventory                  0x2150     0.791%        100.000%       62 / 62         
sithDSSThing                   0x22aa     0.823%        100.000%       37 / 37         
sithRender                     0x23de     0.851%        100.000%       22 / 22         
jkGuiBuildMulti                0x258b     0.891%        100.000%       24 / 24         
sithSoundMixer                 0x2626     0.905%        100.000%       37 / 37         
stdDisplay                     0x267b     0.913%        100.000%       37 / 37         
sithCogFunction                0x26c2     0.920%        100.000%      119 / 119        
sithAI                         0x2771     0.936%        100.000%       35 / 35         
rdModel3                       0x2a7e     1.008%        100.000%       23 / 23         
std3D                          0x2c4a     1.051%        100.000%       39 / 39         
rdMatrix                       0x2c85     1.056%        100.000%       56 / 56         
sithAICmd                      0x2cc0     1.062%        100.000%       22 / 22         
jkGuiRend                      0x2cd7     1.064%        100.000%       68 / 68         
jkPlayer                       0x2da2     1.083%        100.000%       45 / 45         
sithPhysics                    0x310b     1.164%        100.000%       13 / 13         
sithWeapon                     0x32a8     1.202%        100.000%       33 / 33         
rdCache                        0x331c     1.213%        100.000%       16 / 16         
sithCogFunctionThing           0x3a4c     1.383%        100.000%      142 / 142        
rdClip                         0x81f2     3.084%        100.000%       17 / 17         
sithAIAwareness                0x31f      0.074%        90.488%         5 / 6          
stdComm                        0x53c      0.124%        98.955%        16 / 17         
stdFileUtil                    0x687      0.155%        54.159%         6 / 11         
stdBmp                         0x6b8      0.159%        0.000%          0 / 3          
sithAnimClass                  0x6cc      0.161%        94.080%         4 / 5          
stdStrTable                    0x7b6      0.183%        82.877%         4 / 6          
stdColor                       0x97e      0.225%        24.198%         3 / 11         
jkGame                         0x98f      0.227%        80.384%        12 / 13         
stdPalEffects                  0xa66      0.247%        85.875%        16 / 21         
stdLbm                         0xc24      0.288%        0.000%          0 / 3          
rdColormap                     0xcf4      0.307%        47.738%         7 / 12         
stdPlatform                    0xdde      0.329%        49.042%        37 / 43         
sithPlayer                     0xe72      0.343%        95.944%        23 / 27         
stdBitmap                      0xeb6      0.349%        47.398%         6 / 14         
sithTrackThing                 0xf9f      0.371%        90.098%        12 / 15         
rdPuppet                       0x101f     0.383%        97.407%        15 / 19         
stdGif                         0x1162     0.412%        0.000%          0 / 4          
sithCommand                    0x11b2     0.420%        91.545%        18 / 20         
sithIntersect                  0x12a8     0.443%        92.588%         9 / 12         
rdPrimit3                      0x16e0     0.543%        91.684%         7 / 9          
stdFont                        0x181a     0.572%        75.284%        12 / 20         
sithSurface                    0x1c6a     0.674%        96.838%        33 / 35         
sithCog                        0x1ed3     0.731%        90.686%        21 / 28         
sithMulti                      0x252a     0.882%        91.423%        29 / 35         
sithCollision                  0x2827     0.953%        91.954%        19 / 22         
sithControl                    0x285a     0.958%        99.613%        31 / 33         
sithThing                      0x338c     1.223%        86.829%        43 / 49         
rdRaster                       0xf04d     5.702%        0.195%          1 / 89         
rdZRaster                      0x15fb4    8.346%        0.000%          0 / 73         
rdAFRaster                     0x1620d    8.402%        0.000%          0 / 122        
rdNRaster                      0x304d4    18.339%       0.000%          0 / 87         
---------------------------------------------------------------------------------

Total completion:
-----------------
56.758% by weight
95.857% by weight excluding rasterizer
2694 / 3169 functions
2694 / 2798 functions excluding rasterizer

Subsystem Breakdown (Not Decomp'd)
----------------------------------
[subsys]       [% of text]  [TODO / total]
sith           0.542%           40 / 1319
stdPlatform    0.168%            6 / 43
std            1.494%           47 / 376
jkGui          0.000%            0 / 284
rd             0.216%           11 / 345
jk             0.044%            1 / 324
Raster         40.778%         370 / 371
other          0.000%            0 / 107
-----------------------------------------
total          43.242%         475 / 3169

Subsystem Breakdown (Not Decomp'd, Excl Raster)
-----------------------------------------------
[subsys]       [% of text]  [TODO / total]
sith           0.916%           40 / 1319
stdPlatform    0.283%            6 / 43
std            2.524%           47 / 376
jkGui          0.000%            0 / 284
rd             0.364%           11 / 345
jk             0.075%            1 / 324
other          0.000%            0 / 107
-----------------------------------------
total          4.162%          105 / 2798

```

## See Also

- [OpenJones3D](https://github.com/smlu/OpenJones3D) — Sister project decompiling Indiana Jones and the Infernal Machine, which is based on the same engine and internal libraries
- [OpenJK](https://github.com/JACoders/OpenJK) — Community-maintained open-source engine for Jedi Outcast (SP) and Jedi Academy (SP & MP), the sequels to Dark Forces II.
