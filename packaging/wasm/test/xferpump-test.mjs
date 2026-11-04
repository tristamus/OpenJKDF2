// The Imperial Network Protocol's map transfer pump, imperial_XferPump in
// src/Platform/Networking/Imperial/stdComm_Imperial.c, compiled on its own
// with the C compiler (cc) and made-up neighbours: a queue maps.js has
// filled, and the relay's WebSocket draining at a modelled upload.
// node packaging/wasm/test/xferpump-test.mjs
//
// It sends at most IMPERIAL_XFER_RATE (1 MiB/s) as before, nothing while the
// socket has IMPERIAL_XFER_BACKLOG (16 KiB) or more yet to send, and lets
// maps.js send first, telling it whether the host is in a match and how the
// relay server's answers to a timed request came, and sends one when maps.js
// asks. What waits for a machine that left the game is dropped.
//
// Then that request on its own (jkrelay_AskTimed in jkrelay_client.c), with a
// made-up connection: one at a time, its round trip and the shortest, and
// asked again only while the server hasn't answered one.

import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const source = readFileSync(new URL('../../../src/Platform/Networking/Imperial/stdComm_Imperial.c', import.meta.url), 'utf8');

// The pump, its constants and its state, as the file has them.
const defines = source.match(/^#define IMPERIAL_XFER_\w+ .*$/gm);
const state = source.match(/^static [\w ]+ imperial_(aXferSys|aXferSeen|msecXferGone|nextXferAlias|xferTokens|xferTokensMs|aXferBuf|msecHeard)\b.*;.*$/gm);
const from = source.indexOf('static void imperial_XferForgetGone(');
const at = source.indexOf('static void imperial_XferPump()');
const pump = source.slice(from, source.indexOf('\n}\n', at) + 3);
if (!defines || defines.length < 4 || !state || state.length < 8 || from < 0 || at < from)
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
static int serves = 0, lastInMatch = -1, takesSinceServe = 0, wantProbe = 0, probes = 0, lastGot = -1;
static double lastRtt, lastBase, lastWaited;
static int imperial_js_xfer_serve(int bInMatch, int got, double rtt, double base, double waited)
{
    serves++;
    lastInMatch = bInMatch;
    takesSinceServe = 0;
    lastGot = got;
    lastRtt = rtt;
    lastBase = base;
    lastWaited = waited;
    return wantProbe;
}
static void jkrelay_AskTimed(void) { probes++; }
static int jkrelay_TimedState(double* pRtt, double* pBase, double* pWaited)
{
    *pRtt = 120;
    *pBase = 40;
    *pWaited = 300;
    return 3;
}
static int imperial_js_xfer_take(uint8_t* buf, int cap, int* pTo)
{
    takesSinceServe++;
    if (!queued) return -1;
    queued--;
    *pTo = 2;
    return frameLen;
}
static int jkrelay_IsHost(void) { return 1; }
// The machines in the game, and what maps.js was told to drop.
static uint32_t inGame[4] = { 7 };
static int forgot[65];
static int jkrelay_HasPlayer(uint32_t id)
{
    for (int i = 0; i < 4; i++)
        if (inGame[i] && inGame[i] == id) return 1;
    return 0;
}
static void imperial_js_xfer_forget(int id) { forgot[id]++; }
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

    // maps.js gets how the relay server's answers came, and a request is
    // timed when it asks for one, not otherwise.
    reset(0);
    probes = 0;
    imperial_XferPump();
    CHECK(lastGot == 3 && lastRtt == 120 && lastBase == 40 && lastWaited == 300, "told %d %.0f %.0f %.0f", lastGot, lastRtt, lastBase, lastWaited);
    CHECK(probes == 0, "timed %d unasked", probes);
    wantProbe = 1;
    imperial_XferPump();
    wantProbe = 0;
    CHECK(probes == 1, "timed %d when asked", probes);

    // A joiner's machine that leaves: nothing more goes to it, within 1 s.
    // One the server hasn't said joined yet isn't taken for gone.
    reset(0);
    imperial_bInGame = 1;
    imperial_aXferSys[3] = 8;
    memset(forgot, 0, sizeof(forgot));
    runFor(1500, 16);
    CHECK(imperial_aXferSys[2] == 7 && imperial_aXferSys[3] == 8 && !forgot[2] && !forgot[3], "dropped a machine still there");
    inGame[1] = 8;
    runFor(1500, 16);
    inGame[0] = 0;
    inGame[1] = 0;
    runFor(1100, 16);
    CHECK(forgot[2] == 1 && forgot[3] == 1, "dropped %d and %d times", forgot[2], forgot[3]);
    CHECK(!imperial_aXferSys[2] && !imperial_aXferSys[3], "still numbered");
    runFor(3000, 16);
    CHECK(forgot[2] == 1 && forgot[3] == 1, "dropped again");

    printf(failed ? "%d failed\\n" : "all passed\\n", failed);
    return failed ? 1 : 0;
}
`;

// jkrelay_client.c's timed request, from its comment to the #endif after it,
// and the line in jkr_HandlePacket that takes its answer.
const client = readFileSync(new URL('../../../src/Platform/Networking/Imperial/jkrelay_client.c', import.meta.url), 'utf8');
const probeAt = client.indexOf('// A request the relay server answers at once (LIST), timed.');
const probeBlock = client.slice(probeAt, client.indexOf('\n#endif', probeAt));
const handleAt = client.indexOf('static void jkr_HandlePacket(');
const replies = client.slice(handleAt, client.indexOf('switch (type)', handleAt));
if (probeAt < 0 || probeAt > handleAt || !/#ifdef __EMSCRIPTEN__\s+if \(jkr_TimedAnswer\(seq\)\) return;\s+#endif/.test(replies))
    throw new Error("couldn't find jkrelay_AskTimed, or jkr_HandlePacket doesn't take its answer first");

const probeHarness = `
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define JKR_HDR_SIZE 16
#define JKR_LIST 3
#define JKR_WS_MAX_SOCKS 8
static struct { int sock, inSession; uint32_t token, nextSeq; uint8_t appGuid[16]; } R;
static struct { int used, ws; } jkr_wsSocks[JKR_WS_MAX_SOCKS];
static uint64_t nowMs = 5000;
static uint64_t jkr_NowMs(void) { return nowMs; }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint8_t lastType;
static uint32_t lastSeq;
static int sends = 0;
static size_t lastLen;
static void jkr_PutHdr(uint8_t* p, uint8_t type, uint32_t seq) { memset(p, 0, JKR_HDR_SIZE); p[4] = type; wr32(p + 12, seq); }
static void jkr_SendRaw(const uint8_t* p, size_t len) { sends++; lastType = p[4]; lastSeq = (uint32_t)p[12] | (uint32_t)p[13] << 8 | (uint32_t)p[14] << 16 | (uint32_t)p[15] << 24; lastLen = len; }

${probeBlock}

static int failed = 0;
#define CHECK(c, ...) do { if (!(c)) { failed++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\\n"); } } while (0)

int main(void)
{
    double rtt, base, waited;
    R.sock = 1; R.inSession = 1; R.token = 9; R.nextSeq = 100;
    jkr_wsSocks[1].ws = 4;
    CHECK(jkrelay_TimedState(&rtt, &base, &waited) == 0 && waited < 0, "known before any");

    // One at a time: a LIST, as the menu sends.
    jkrelay_AskTimed();
    jkrelay_AskTimed();
    CHECK(sends == 1 && lastType == JKR_LIST && lastSeq == 101 && lastLen == JKR_HDR_SIZE + 20, "sent %d, type %d, seq %u", sends, lastType, lastSeq);
    nowMs += 30;
    CHECK(jkrelay_TimedState(&rtt, &base, &waited) == 0 && waited == 30, "waited %.0f", waited);
    CHECK(!jkr_TimedAnswer(100), "took another answer");
    nowMs += 20;
    CHECK(jkr_TimedAnswer(101), "didn't take its answer");
    CHECK(jkrelay_TimedState(&rtt, &base, &waited) == 1 && rtt == 50 && base == 50 && waited < 0, "%.0f %.0f %.0f", rtt, base, waited);

    // Behind a queue: longer than the shortest, which stays.
    jkrelay_AskTimed();
    nowMs += 900;
    CHECK(jkrelay_TimedState(&rtt, &base, &waited) == 1 && waited == 900, "waited %.0f", waited);
    jkr_TimedAnswer(102);
    CHECK(jkrelay_TimedState(&rtt, &base, &waited) == 2 && rtt == 900 && base == 50, "%.0f %.0f", rtt, base);

    // Once the server has answered, one that doesn't come back isn't sent again.
    jkrelay_AskTimed();
    nowMs += 20000;
    jkrelay_AskTimed();
    CHECK(sends == 3, "sent %d", sends);
    CHECK(jkrelay_TimedState(&rtt, &base, &waited) == 2 && waited == 20000, "waited %.0f", waited);

    // Another connection: nothing known; a server that never answers is
    // asked again every 10 s.
    jkr_wsSocks[1].ws = 5;
    CHECK(jkrelay_TimedState(&rtt, &base, &waited) == 0, "known on another connection");
    jkrelay_AskTimed();
    nowMs += 9000;
    jkrelay_AskTimed();
    CHECK(sends == 4, "sent %d", sends);
    nowMs += 1000;
    jkrelay_AskTimed();
    CHECK(sends == 5, "sent %d", sends);

    // Not in a game: nothing.
    R.inSession = 0;
    nowMs += 20000;
    jkrelay_AskTimed();
    CHECK(sends == 5, "sent %d out of a game", sends);

    printf(failed ? "%d failed\\n" : "the timed request: all passed\\n", failed);
    return failed ? 1 : 0;
}
`;

const dir = mkdtempSync(join(tmpdir(), 'xferpump-'));
try {
    for (const [name, code] of [['pump', harness], ['probe', probeHarness]]) {
        writeFileSync(join(dir, name + '.c'), code);
        execFileSync('cc', ['-std=c11', '-O2', '-Wall', '-Werror', '-Wno-unused-function', '-Wno-unused-variable', '-o', join(dir, name), join(dir, name + '.c')], { stdio: 'inherit' });
        execFileSync(join(dir, name), { stdio: 'inherit' });
    }
} catch (e) {
    process.exitCode = 1;
} finally {
    rmSync(dir, { recursive: true, force: true });
}
