# Challenge to a duel

JKWeb Duels adds a consensual private saber fight inside an ordinary browser
free-for-all. It takes inspiration from Jedi Outcast and Jedi Academy and uses
JK/MotS's existing combat, maps and characters.

It started as `jkweb-duels` in `CodaLilitari/OpenJKDF2-MotS-Fixes` and is
part of the jediknight.zone browser build. A host turns it on for their game
with **Private duels** on **Host > Advanced**. It is off by default, and the
choice is kept for the next game that host opens.

## Playing

1. The host ticks **Private duels** on **Host > Advanced** and starts a
   free-for-all game. The room's Info then shows **Private duels: On**.
   Finish any active Force powers and equip a usable saber.
2. **K** is the default **Challenge to a duel** button. Remap it in
   **Setup > Controls > Keyboard**, or assign a mouse or joystick button in
   its Controls screen. Older profiles receive K only when it is free and
   the duel action has no existing binding. Existing controls and console
   shortcuts are preserved; a deliberate unbind stays unbound after saving.
3. Stand near another player, aim at them and press **K** (or your chosen duel button). They
   receive a challenge that expires after five seconds.
4. The other player aims back at you and presses their own duel button to
   accept. Both must still be nearby, visible, alive and holding their saber.
   Aiming at somebody else does not accept the challenge. Holding the button
   does not send repeated requests.
5. Both players prepare for two seconds, then fight. During preparation,
   movement and attacks are suppressed. Ordinary movement and stock Force
   Jump are available once fighting starts.
6. The surviving winner of a completed duel recovers full health and shields.
   The defeated player uses the game's normal death/respawn flow.

Chat remains an alternative: `/duel` challenges the player you aim at, and
`/duel accept` accepts an incoming challenge. The console accepts `duel` without
the slash. `players` lists player slots; `duel <slot>` challenges a particular
player. The existing console shortcut `bind k duel` also works. If you already
use that shortcut and want to assign K through the Controls menu instead,
run `unbind k` first to remove the separate console binding. A console K
shortcut takes precedence over the native duel action on that key, so the
same press does not trigger both. Other assigned duel buttons remain usable.

| Command | Action |
| --- | --- |
| `duel` | Challenge the aimed-at player, or accept their reciprocal challenge |
| `duel <slot>` | Challenge the player listed in that slot by `players` |
| `duel accept` | Accept the newest incoming challenge |
| `duel decline` | Decline every incoming challenge |
| `duel cancel` | Withdraw your challenge or end your duel |
| `duel status` | Show your duel and how many others are under way |

Use `/duel ...` for the same actions from chat. Commands are handled locally
before ordinary chat is broadcast.

## Combat and spectators

Several pairs can duel at once, up to 32 in a game. Each player is in one
duel at most and has at most one challenge of their own waiting: challenging
someone else replaces it. A player can have several challenges waiting for
them; accepting one ends the other challenges to and from both duelists, and
nobody can challenge a player who is already in a duel. Each pair is isolated
from everyone else, other duelists included. Both players remain visible in
the same map, so everyone else can watch. Outsiders and duelists pass through each other;
outsider bodies do not block, shove or absorb the duelists' saber contacts.
Projectiles, explosions and targeted Force effects are filtered across the
duel boundary in both directions. The two opponents still collide and fight
normally with one another.

The fight permits saber strikes, ordinary movement and stock Force Jump.
Other weapons, saber throw, consumables and targeted Force powers are blocked.
Starting health and shields are preserved. Normal map geometry and
environmental hazards remain in effect.

Challenges require a distance of at most 2 JK units and line of sight. At
8 JK units of separation the duel ends. Death, departure, respawn, cancellation,
map changes and incompatible late joiners release the duel restrictions.
Declining, expiring or cancelling a challenge does not heal either player;
disconnecting and abandoning a duel do not grant recovery. Ordinary match
scoring remains in charge of kills and deaths: a duel kill counts for the
match's score and the leaderboard like any other kill.

## Compatibility

The extension targets peer-hosted browser free-for-all, in both Jedi Knight and
Mysteries of the Sith. It uses the stock saber bin for each game. Team/objective
modes, spectator-only clients and Imperial/native/retail transport sessions
cannot initiate it. A host running a Dedicated Server referees the duels
without playing. All active players must advertise support before it starts.
If an unsupported player joins, the duel ends instead of silently losing its
protection on that client.

The rules cover ordinary stock gameplay among cooperating clients, including
the stock engine's combat and Force paths. Duel messages, participant body
updates and results validate the sender's authority. The underlying peer game
still has trusted scripting and world channels; this feature is not a general
anti-cheat boundary. A custom COG can directly rewrite a player's health,
velocity or inventory outside the combat paths. Mods that replace the stock
saber or inventory behavior need separate gameplay qualification.

## Build and verification

Use Emscripten **6.0.12**, the same version as upstream's browser workflow.
Initialize `lib/zlib` and `lib/libpng`, source `emsdk_env.sh`, then run:

```sh
cmake -B build_emcc --toolchain cmake_modules/toolchain_wasm.cmake
cmake --build build_emcc -j4
cmake -B build_emcc_jspi --toolchain cmake_modules/toolchain_wasm.cmake -DOPENJKDF2_WASM_JSPI=ON
cmake --build build_emcc_jspi -j4
sh packaging/wasm/test/duel-state-test.sh
sh packaging/wasm/test/duelcombat-test.sh
sh packaging/wasm/test/duel-mutations-test.sh
sh packaging/wasm/test/duel-integration-test.sh
sh packaging/wasm/test/duel-pairs-test.sh
sh packaging/wasm/test/duel-controls-test.sh
```

The `WebAssembly` workflow builds both engines and runs the existing browser
regressions plus the duel checks on this branch. Its artifact contains the
engine and web page only. Players supply their own game files; never enable
`OPENJKDF2_WASM_PRELOAD_DATA` for a distributable build.

### Several pairs and the host's box (2026-11-22)

Wire version 2 carries a pair index in every frame (28 bytes each, as
before) and a header frame from the host with its session and whether it
allows duels; the join tag is `JDL2` and the hello capability bit 1. A
version 1 frame is refused. `duel-pairs-test.sh` runs the host with five
seats: two duels fighting at once, isolation between them, busy players,
the newest challenge accepted and the others ended, one challenge of your
own at a time, the box turned off mid-duel, and a Dedicated Server host. The
three-peer TCP test also covers two offers to one player and the box off.

### Verified on 2026-11-21 (the single-pair version)

| Check | Result |
| --- | --- |
| Full Asyncify and JSPI WebAssembly builds, Emscripten 6.0.12 | Passed |
| Existing browser/native test groups plus all five new duel groups | 33/33 passed |
| Reliable messages, roster authority and join compatibility | 140 desktop, 153 browser and 140 retro checks passed |
| Three independent native peers running the production duel modules over TCP | 131/131 assertions passed; 300 game frames |
| Default K, existing bindings, intentional unbinding, console shortcuts, Controls enumeration and keyboard/mouse profile save/load | Passed in browser/native builds, with and without quality-of-life controls |
| Production saber-trace and body-update handlers | Passed, including door obstruction, outsider pass-through and owner death evidence |
| Hit-claim validation and motion/playout | 430/430 and 80/80 checks passed |
| Level Library and playlist menus | 1,879 and 2,250 checks passed |
| JSPI suspension/exports and both engines' lost-graphics support | Passed |

The reliable-message, controls and three-peer checks are included in the 33 test groups,
not additional groups. Independent source review completed with the identified
authority, cancellation, respawn, obstruction and mixed-version join defects
fixed and covered by regressions. The existing upstream build warnings remain.

The TCP fixture compiles the complete production adapter, codec and combat
modules against real engine types. It supplies a minimal world, clock and
runtime callbacks; it is not a complete running game. It covers host/client
winner roles, an observing host, consent, expiry, duplicate and forged packets,
recovery, ambiguous death evidence, replacement lives, cancellation races and
reciprocal aimed challenges. The controls fixture runs the production binding,
profile and input code with simulated device state, verifying saved keyboard
and mouse bindings and one command per press. It also covers adoption of K
by older profiles, preservation of custom controls, saved unbinding, and
coexistence with the separate console shortcut system.
The separate handler fixtures exercise the real saber trace and network body
message receivers.

**Rendered browser gameplay remains unverified.** Chromium could not start in
the test environment because its local IPC socket creation was denied. These
builds and fixtures do not replace a live WebRTC/WebSocket match, real-map
physics, rendering or execution of stock JK/MotS COG scripts.

### Multiplayer gameplay check

The remaining acceptance check is Trist's rendered match with three browser
clients on a stock map. Aim at another player and press K, then aim back and
press K on that player's client. Confirm the duel starts after preparation,
the third player can watch and walk through both duelists, and outsider shots,
explosions and Force effects do not interfere. Check both a closed movable
door and an outsider standing between the fighters. Finish the duel and check
full recovery on the living winner and ordinary loadout/movement on the
defeated player's next life. Repeat with a client winner, with the host
watching two clients, and in both JK and MotS.

## Provenance

Browser multiplayer, page, transport, existing combat validation and upstream
fixes are from Tristamus and the OpenJKDF2 contributors. The private-duel feature
is implemented by Lilitari, with design feedback on recovery, unobstructed
movement and visible duels. Existing licenses and
contributor credits remain in place. JO/JA served as behavioral references;
their commercial game assets are not included.
