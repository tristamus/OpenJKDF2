# Private duel challenges for JKWeb

## Intent

Add consensual, saber-only private duels inside a running free-for-all match to
the browser engine used by JediKnight.zone. Base: Tristamus/OpenJKDF2 `web-mp`,
commit `9eb99b4aa2ca45892d61c96fe54e655d27fe0fae`. Preserve its engine, asset
loading, transport, licenses and credit.

## Design choices

An engine implementation is required: a lobby invitation alone cannot isolate
damage, collisions or Force effects. A COG-only implementation would depend on
each map/mod and cannot reliably gate the existing network hit-claim path.
Use the existing engine and browser transport with a small testable C state
machine, a host-owned protocol adapter and centralized combat predicates.

These are implementation defaults chosen for this feature:

- One private duel at a time, matching original JO/JA. One outstanding offer,
  expiring after five seconds; a reciprocal challenge accepts it.
- Both players explicitly opt in while alive, holding their game's saber,
  within 2 JKU and visible to each other. An accepted offer has one shared
  two-second preparation period before fighting. Separating by 8 JKU ends it.
- Local `duel` challenges the aimed-at player; `duel <slot>` uses a slot from
  `players`. `duel accept`, `duel decline`, `duel cancel`, `duel status`, and
  `/duel ...` chat equivalents provide explicit controls. Requests use stable
  network identities and challenge generations, never display names.
- **Challenge to a duel** is a bindable Controls action for keyboard, mouse
  and joystick buttons. It uses the same aimed challenge for either player:
  the recipient must aim back and press their action to accept. Press
  transitions trigger it once; holding does not repeat. It starts unassigned
  and persists with the player's chosen controls, without shifting old IDs.
- Saber combat and ordinary movement during fighting; no guns, saber throw,
  consumables or targeted Force powers. Stock Force Jump may be used during
  the fight. Preparation suppresses movement/attacks. Preserve starting health,
  shields, Force ranks and inventory. Reject entry with active disallowed powers.
- On a completed duel, restore the surviving winner to full health and full
  shields. Declined/expired/cancelled offers,
  disconnects and unsupported joins do not grant healing. Keep normal respawn
  behavior for the defeated player. No invisibility or private world: all
  observers continue to see both duelists throughout the fight.
- Outsider damage, Force targeting, splash impulse, projectile/body collision
  and duelist attacks on outsiders are isolated in both directions. World
  geometry and environmental hazards remain. Do not claim to sandbox arbitrary
  custom COG scripts that directly rewrite health or velocity.
- Death, departure, respawn, separation, explicit cancellation, map/session
  reset and unsupported late join all release restrictions. Ordinary game
  scoring remains authoritative; announcements do not fabricate kill credit.
- Publish the work only as a new `jkweb-duels` branch in the existing
  `CodaLilitari/OpenJKDF2-MotS-Fixes` fork.
- Scope is peer-hosted browser FFA. Team/objective games, spectator feeds and
  Imperial/native/retail transports cannot initiate this browser extension.
  Every active peer must advertise the duel capability before it starts.
- All authoritative duel state comes from the actual browser room host.
  Malformed, forged, expired, stale and repeated duel requests/snapshots are
  ignored or rejected. Host state snapshots resynchronize late joins and
  packet retries. This does not replace the underlying peer game's trust
  model with a general anti-cheat boundary.

## Implementation boundaries

`sithDuelState` contains pure state transitions and decoder validation.
`sithDuel` connects that state to commands, capabilities, messages and lifecycle.
`sithDuelCombat` owns game-object policy and combat integration helpers. Existing
call sites consult that policy before gameplay side effects. Desktop builds
keep their original behavior behind `PLATFORM_WSRELAY` guards.

## Verification

Test the production state machine and wire decoder with asset-free C fixtures;
test integration predicates against real engine types where practical. Cover
consent/expiry, races, stale generations, spoofed sender, malformed lengths,
countdown, normal combat, outsider interference, participant loss, slot reuse,
and reset. Run existing browser/transport regressions and compile both
Asyncify and JSPI WebAssembly variants. Record what was actually run and keep
multiplayer play testing distinct from synthetic harnesses.
