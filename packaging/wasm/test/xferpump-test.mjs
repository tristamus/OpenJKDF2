// The Imperial Network Protocol's map transfer pump, imperial_XferPump in
// src/Platform/Networking/Imperial/stdComm_Imperial.c, compiled on its own
// with the C compiler (cc) and made-up neighbours: a queue maps.js has
// filled, and the relay's WebSocket draining at a modelled upload.
// node packaging/wasm/test/xferpump-test.mjs
//
// It sends at most IMPERIAL_XFER_RATE (1 MiB/s) as before, nothing while the
// socket has IMPERIAL_XFER_BACKLOG (16 KiB) or more yet to send, and lets
// maps.js send first, telling it whether the host is in a match.

import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const source = readFileSync(new URL('../../../src/Platform/Networking/Imperial/stdComm_Imperial.c', import.meta.url), 'utf8');

// The pump, its constants and its state, as the file has them.
const defines = source.match(/^#define IMPERIAL_XFER_\w+ .*$/gm);
const state = source.match(/^static [\w ]+ imperial_(aXferSys|nextXferAlias|xferTokens|xferTokensMs|aXferBuf|msecHeard)\b.*;.*$/gm);
const at = source.indexOf('static void imperial_XferPump()');
const pump = source.slice(at, source.indexOf('\n}\n', at) + 3);
if (!defines || defines.length < 4 || !state || state.length < 6 || at < 0)
    throw new Error("couldn't find imperial_XferPump and what it uses");

const harness = `
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static uint32_t simMs = 100000;
static uint32_t stdPlatform_GetTimeMsec(void) { return simMs; }
static int imperial_bInGame = 1;

// maps.js: frames of frameLen bytes waiting for joiner 2.
static int queued = 0, frameLen = 16 + 24 + 64 * 1024;
static int serves = 0, lastInMatch = -1, takesSinceServe = 0;
static void imperial_js_xfer_serve(int bInMatch) { serves++; lastInMatch = bInMatch; takesSinceServe = 0; }
static int imperial_js_xfer_take(uint8_t* buf, int cap, int* pTo)
{
    takesSinceServe++;
    if (!queued) return -1;
    queued--;
    *pTo = 2;
    return frameLen;
}
static int jkrelay_IsHost(void) { return 1; }
static uint32_t jkrelay_HostSystemId(void) { return 1; }
static uint32_t jkrelay_LocalSystemId(void) { return 1; }

// The relay's WebSocket: what it has yet to send drains at upBytesPerSec.
static double backlog = 0, upBytesPerSec = 0, maxBacklog = 0;
static long long sent = 0;
int jkrelay_SendBacklog(void) { return (int)backlog; }
static int jkrelay_SendBig(uint32_t idFrom, uint32_t idTo, const uint8_t* p, size_t n)
{
    sent += (long long)n;
    backlog += (double)n;
    if (backlog > maxBacklog) maxBacklog = backlog;
    return 1;
}

${defines.join('\n')}
${state.join('\n')}
${pump}

static int failed = 0;
#define CHECK(c, ...) do { if (!(c)) { failed++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\\n"); } } while (0)

static void reset(int frames)
{
    simMs = 100000;
    queued = frames;
    sent = 0;
    backlog = maxBacklog = 0;
    serves = 0;
    imperial_xferTokens = IMPERIAL_XFER_BURST;
    imperial_xferTokensMs = simMs;
    imperial_aXferSys[2] = 7;
}

// Runs the pump each stepMs for ms, the socket draining between.
static void runFor(int ms, int stepMs)
{
    for (int t = 0; t < ms; t += stepMs)
    {
        simMs += (uint32_t)stepMs;
        backlog -= upBytesPerSec * stepMs / 1000.0;
        if (backlog < 0) backlog = 0;
        imperial_XferPump();
    }
}

int main(void)
{
    // A socket that never drains: one frame goes, then nothing while it waits.
    reset(48);
    upBytesPerSec = 0;
    runFor(3000, 16);
    CHECK(sent == frameLen, "sent %lld with the socket full", sent);
    CHECK(queued == 47, "took %d frames", 48 - queued);
    CHECK(serves == 3000 / 16 + (3000 % 16 != 0), "maps.js asked %d times", serves);

    // Nothing at all while 16 KiB or more waits, however long the gap.
    reset(48);
    backlog = IMPERIAL_XFER_BACKLOG;
    simMs += 5000;
    imperial_XferPump();
    CHECK(sent == 0 && queued == 48, "sent %lld behind a 16 KiB backlog", sent);
    backlog = IMPERIAL_XFER_BACKLOG - 1;
    imperial_XferPump();
    CHECK(sent == frameLen, "sent %lld once the backlog went under 16 KiB", sent);

    // A fast connection: still at most 1 MiB/s, as before.
    reset(1000);
    upBytesPerSec = 1e9;
    runFor(10000, 16);
    printf("    fast upload: %.0f bytes a second over 10 s\\n", sent / 10.0);
    CHECK(sent <= 10LL * IMPERIAL_XFER_RATE + IMPERIAL_XFER_BURST + frameLen, "sent %lld in 10 s", sent);
    CHECK(sent >= 9LL * IMPERIAL_XFER_RATE, "sent only %lld in 10 s", sent);

    // 2 Mbit/s up: the socket never has more than 16 KiB and a frame waiting.
    reset(1000);
    upBytesPerSec = 250000;
    runFor(10000, 16);
    printf("    2 Mbit/s upload: %.0f bytes a second, at most %.0f bytes waiting in the socket\\n", sent / 10.0, maxBacklog);
    CHECK(maxBacklog < IMPERIAL_XFER_BACKLOG + frameLen, "%.0f waiting", maxBacklog);
    CHECK(sent >= 9 * 250000LL, "sent only %lld in 10 s", sent);

    // maps.js hears whether the host is in a match: a game packet from
    // another player in the last 2 s, in a game.
    reset(0);
    imperial_msecHeard = 0;
    imperial_XferPump();
    CHECK(lastInMatch == 0, "in a match before hearing anyone");
    imperial_msecHeard = simMs;
    simMs += 1999;
    imperial_XferPump();
    CHECK(lastInMatch == 1, "not in a match 1999 ms after a packet");
    simMs += 2;
    imperial_XferPump();
    CHECK(lastInMatch == 0, "in a match 2001 ms after a packet");
    imperial_msecHeard = simMs;
    imperial_bInGame = 0;
    imperial_XferPump();
    CHECK(lastInMatch == 0, "in a match out of a game");

    printf(failed ? "%d failed\\n" : "all passed\\n", failed);
    return failed ? 1 : 0;
}
`;

const dir = mkdtempSync(join(tmpdir(), 'xferpump-'));
try {
    writeFileSync(join(dir, 'pump.c'), harness);
    execFileSync('cc', ['-std=c11', '-O2', '-Wall', '-Werror', '-Wno-unused-function', '-Wno-unused-variable', '-o', join(dir, 'pump'), join(dir, 'pump.c')], { stdio: 'inherit' });
    execFileSync(join(dir, 'pump'), { stdio: 'inherit' });
} catch (e) {
    process.exitCode = 1;
} finally {
    rmSync(dir, { recursive: true, force: true });
}
