# JKWeb Private Duels Implementation Plan

**Goal:** Add a mutually accepted private saber duel to browser FFA matches.

**Architecture:** A host-owned C state machine, capability-negotiated protocol,
and centralized combat policy integrated into existing engine paths.

**Tech stack:** C11, Emscripten 6.0.12, existing WebRTC/WebSocket transport,
Node.js asset-free regressions.

**Spec:** `docs/superpowers/specs/2026-11-21-private-duels-design.md`

## Global constraints

- Base `tristamus/OpenJKDF2`, web-mp commit `9eb99b4aa2ca45892d61c96fe54e655d27fe0fae`.
- No game assets in published code or packages.
- Browser FFA only; require all active players to advertise duel support.
- Preserve upstream credits, existing normal-play behavior and native builds.
- Use branch `jkweb-duels` in `CodaLilitari/OpenJKDF2-MotS-Fixes`.
- Full health and shields for the survivor of a completed duel; no heal on
  invitation cancellation. Both fighters remain visible and outsiders cannot
  block, push or otherwise impede their bodies.
- Five-second offer, two-second preparation, 2 JKU challenge / 8 JKU separation.

## Review focus

- Forged/stale messages must not grant unilateral invulnerability.
- Slot reuse or a dead/disconnected peer must not leave restrictions behind.
- Damage, Force, splash and collision paths must consult the same duel state.
- Old clients must not enter a room's protected duel without capability support.
- Tests and build receipts must not be described as live multiplayer playtests.

## Tasks

- [x] Add tested C state machine and host protocol; integrate command, handshake,
  dispatch and lifecycle entry points. Native fixtures first, then production.
- [x] Add centralized combat policy and engine hooks; cover classic and claimed
  hits, saber, projectile, Force, inventory, movement and splash interactions.
- [x] Integrate user instructions, build registration and regression workflow.
- [x] Build both WASM variants; run focused and existing regression suites.
- [x] Obtain independent review and fix concrete findings for the dedicated
  `jkweb-duels` development branch.

## Execution record

- Upstream baseline Asyncify WASM build completed with the upstream compiler
  warnings. Existing reliable-message tests: 134 checks per each of three
  build shapes. Existing transport tests: 88 passes. All 28 baseline browser
  and native test groups passed.
- Independent review identified body/roster message authority, preparation
  cancellation, replacement-life restrictions and movable-door saber
  obstruction issues, plus old-host join compatibility. All findings were
  fixed, covered by production-code regressions and cleared on review.
- Final Asyncify and JSPI builds passed, as did all 32 browser/native test
  groups. The three-peer integration fixture passed 119 assertions over
  280 TCP game frames. The remaining workflow checks passed as recorded in
  `docs/JKWEB_DUELS.md`.
- The rendered browser match remains an acceptance check: Chromium's IPC
  socket initialization was denied by the environment. Native process
  integration results are not represented as live browser gameplay.
