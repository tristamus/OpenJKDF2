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
// asks, or until IMPERIAL_XFER_TIMED_FIRST have come. It notes each frame for
// the page (stdComm_WSRelay_Frame). What waits for a machine that left the
// game is dropped.
//
// Then that request on its own (jkrelay_AskTimed in jkrelay_client.c), with a
// made-up connection: one at a time, its round trip (to when the page took
// the answer in) and the shortest, asked again after 10 s while one is out
// (not answered, or lost), and once the server has answered, no sooner than
// 2 s after the last however often it's asked for.

import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const source = readFileSync(new URL('../../../src/Platform/Networking/Imperial/stdComm_Imperial.c', import.meta.url), 'utf8');

// The pump, its constants and its state, as the file has them.
const defines = source.match(/^#define IMPERIAL_XFER_\w+ .*$/gm);
const state = source.match(/^static [\w ]+ imperial_(aXferSys|aXferSeen|msecXferGone|nextXferAlias|xferTokens|xferTokensMs|aXferBuf|msecHeard|msecSent)\b.*;.*$/gm);
const from = source.indexOf('static void imperial_XferForgetGone(');
const at = source.indexOf('static void imperial_XferPump()');
const pump = source.slice(from, source.indexOf('\n}\n', at) + 3);
if (!defines || defines.length < 5 || !state || state.length < 9 || from < 0 || at < from)
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
static int serves = 0, lastInMatch = -1, takesSinceServe = 0, wantProbe = 0, probes = 0, lastGot = -1, timedGot = 3;
static double lastRtt, lastBase, lastWaited, lastAt;
static int imperial_js_xfer_serve(int bInMatch, int got, double rtt, double base, double waited, double answeredAt)
{
    serves++;
    lastInMatch = bInMatch;
    takesSinceServe = 0;
    lastGot = got;
    lastRtt = rtt;
    lastBase = base;
    lastWaited = waited;
    lastAt = answeredAt;
    return wantProbe;
}
static void jkrelay_AskTimed(void) { probes++; }
static int jkrelay_TimedState(double* pRtt, double* pBase, double* pWaited, double* pAt)
{
    *pRtt = 120;
    *pBase = 40;
    *pWaited = 300;
    *pAt = 777;
    return timedGot;
}
// The page's note of each frame (stdComm_WSRelay.c).
static int framesNoted = 0;
static void stdComm_WSRelay_Frame(void) { framesNoted++; }
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
    imperial_msecHeard = imperial_msecSent = 0;
    imperial_XferPump();
    CHECK(lastInMatch == 0, "in a match before hearing anyone");
    imperial_msecHeard = simMs;
    simMs += 1999;
    imperial_XferPump();
    CHECK(lastInMatch == 1, "not in a match 1999 ms after a packet");
    simMs += 2;
    imperial_XferPump();
    CHECK(lastInMatch == 0, "in a match 2001 ms after a packet");
    // Or one in the last minute, while this game still sends its own to the
    // others: a player whose tab stalls goes quiet for seconds.
    imperial_msecSent = simMs;
    simMs += 8000;
    imperial_XferPump();
    CHECK(lastInMatch == 1, "not in a match 10 s after a packet, with the game sending 8 s ago");
    imperial_msecSent = simMs - 10001;
    imperial_XferPump();
    CHECK(lastInMatch == 0, "in a match with the game sending to no one for 10 s");
    imperial_msecHeard = simMs - 60001;
    imperial_msecSent = simMs;
    imperial_XferPump();
    CHECK(lastInMatch == 0, "in a match a minute after the last packet");
    imperial_msecHeard = 0;
    imperial_XferPump();
    CHECK(lastInMatch == 0, "in a match with the game sending, before hearing anyone");
    imperial_msecHeard = simMs;
    imperial_bInGame = 0;
    imperial_XferPump();
    CHECK(lastInMatch == 0, "in a match out of a game");
    imperial_bInGame = 1;
    // Each pump notes the frame for the page, first.
    framesNoted = 0;
    imperial_XferPump();
    CHECK(framesNoted == 1, "noted %d frames in one pump", framesNoted);

    // maps.js gets how the relay server's answers came, and a request is
    // timed when it asks for one, not otherwise.
    reset(0);
    probes = 0;
    imperial_XferPump();
    CHECK(lastGot == 3 && lastRtt == 120 && lastBase == 40 && lastWaited == 300 && lastAt == 777,
          "told %d %.0f %.0f %.0f %.0f", lastGot, lastRtt, lastBase, lastWaited, lastAt);
    CHECK(probes == 0, "timed %d unasked", probes);
    wantProbe = 1;
    imperial_XferPump();
    wantProbe = 0;
    CHECK(probes == 1, "timed %d when asked", probes);
    // A host has IMPERIAL_XFER_TIMED_FIRST answers come as its game starts,
    // whether maps.js asks or not, for the shortest.
    timedGot = IMPERIAL_XFER_TIMED_FIRST - 1;
    probes = 0;
    imperial_XferPump();
    CHECK(probes == 1, "timed %d with %d answers", probes, timedGot);
    timedGot = IMPERIAL_XFER_TIMED_FIRST;
    imperial_XferPump();
    CHECK(probes == 1, "timed %d more with %d answers", probes - 1, timedGot);
    timedGot = 3;

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
// The page's clock, and when the page took in what was read last: as it's
// read here unless a check says otherwise.
static double emscripten_get_now(void) { return (double)nowMs; }
static double jkr_wsRecvAtMs = 0.0;
static int jkr_TimedAnswer(uint32_t seq);
static int answer(uint32_t seq) { jkr_wsRecvAtMs = (double)nowMs; return jkr_TimedAnswer(seq); }
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
    double rtt, base, waited, at;
    R.sock = 1; R.inSession = 1; R.token = 9; R.nextSeq = 100;
    jkr_wsSocks[1].ws = 4;
    CHECK(jkrelay_TimedState(&rtt, &base, &waited, &at) == 0 && waited < 0, "known before any");

    // One at a time: a LIST, as the menu sends.
    jkrelay_AskTimed();
    jkrelay_AskTimed();
    CHECK(sends == 1 && lastType == JKR_LIST && lastSeq == 101 && lastLen == JKR_HDR_SIZE + 20, "sent %d, type %d, seq %u", sends, lastType, lastSeq);
    nowMs += 30;
    CHECK(jkrelay_TimedState(&rtt, &base, &waited, &at) == 0 && waited == 30, "waited %.0f", waited);
    CHECK(!answer(100), "took another answer");
    nowMs += 20;
    CHECK(answer(101), "didn't take its answer");
    CHECK(jkrelay_TimedState(&rtt, &base, &waited, &at) == 1 && rtt == 50 && base == 50 && waited < 0, "%.0f %.0f %.0f", rtt, base, waited);

    // Answered: the next goes 2 s after the last went, not before.
    jkrelay_AskTimed();
    nowMs += 1949;
    jkrelay_AskTimed();
    CHECK(sends == 1 && jkrelay_TimedState(&rtt, &base, &waited, &at) == 1 && waited < 0, "sent %d within 2 s", sends);
    nowMs += 1;
    jkrelay_AskTimed();
    CHECK(sends == 2 && lastSeq == 102, "sent %d at 2 s", sends);

    // Behind a queue: longer than the shortest, which stays.
    nowMs += 900;
    CHECK(jkrelay_TimedState(&rtt, &base, &waited, &at) == 1 && waited == 900, "waited %.0f", waited);
    answer(102);
    CHECK(jkrelay_TimedState(&rtt, &base, &waited, &at) == 2 && rtt == 900 && base == 50, "%.0f %.0f", rtt, base);

    // Once the server has answered, one that doesn't come back is taken as
    // lost after 10 s, and another goes; the lost one's answer, if it comes
    // late, isn't taken for it.
    nowMs += 1100;
    jkrelay_AskTimed();
    CHECK(sends == 3 && lastSeq == 103, "sent %d", sends);
    nowMs += 9999;
    jkrelay_AskTimed();
    CHECK(sends == 3, "sent %d within 10 s", sends);
    CHECK(jkrelay_TimedState(&rtt, &base, &waited, &at) == 2 && waited == 9999, "waited %.0f", waited);
    nowMs += 1;
    jkrelay_AskTimed();
    CHECK(sends == 4 && lastSeq == 104, "sent %d at 10 s", sends);
    nowMs += 40;
    CHECK(jkrelay_TimedState(&rtt, &base, &waited, &at) == 2 && waited == 40, "waited %.0f", waited);
    CHECK(!answer(103), "took the lost one's answer");
    nowMs += 20;
    CHECK(answer(104), "didn't take its answer");
    CHECK(jkrelay_TimedState(&rtt, &base, &waited, &at) == 3 && rtt == 60 && base == 50 && waited < 0, "%.0f %.0f %.0f", rtt, base, waited);

    // Another connection: nothing known; a server that never answers is
    // asked again every 10 s.
    jkr_wsSocks[1].ws = 5;
    CHECK(jkrelay_TimedState(&rtt, &base, &waited, &at) == 0, "known on another connection");
    jkrelay_AskTimed();
    nowMs += 9000;
    jkrelay_AskTimed();
    CHECK(sends == 5, "sent %d", sends);
    nowMs += 1000;
    jkrelay_AskTimed();
    CHECK(sends == 6, "sent %d", sends);

    // Asked for each frame, answered within 50 ms: one each 2 s, where the
    // menu asks every 5 s.
    jkr_wsSocks[1].ws = 6;
    {
        int first = sends;
        uint32_t answered = lastSeq;
        uint64_t sentAt = 0, shortest = (uint64_t)-1, longest = 0;
        for (int t = 0; t < 60000; t += 16, nowMs += 16)
        {
            if (lastSeq != answered && nowMs - sentAt >= 50)
            {
                answered = lastSeq;
                answer(lastSeq);
            }
            int before = sends;
            jkrelay_AskTimed();
            if (sends == before) continue;
            if (sends - first > 1)
            {
                if (nowMs - sentAt < shortest) shortest = nowMs - sentAt;
                if (nowMs - sentAt > longest) longest = nowMs - sentAt;
            }
            sentAt = nowMs;
        }
        printf("    asked for each frame for 60 s: %d sent, %llu to %llu ms apart\\n", sends - first, (unsigned long long)shortest, (unsigned long long)longest);
        CHECK(sends - first == 30, "sent %d in 60 s", sends - first);
        CHECK(shortest >= 2000 && longest <= 2016, "%llu to %llu ms apart", (unsigned long long)shortest, (unsigned long long)longest);
        CHECK(jkrelay_TimedState(&rtt, &base, &waited, &at) == 30 && rtt == 64 && base == 64, "%.0f %.0f", rtt, base);
    }

    // An answer the page took in before the engine read it, a frame or two
    // on: timed to when the page took it, which the state gives too.
    jkr_wsSocks[1].ws = 7;
    {
        double sentAt = (double)nowMs;
        jkrelay_AskTimed();
        nowMs += 300;
        jkr_wsRecvAtMs = sentAt + 45;
        CHECK(jkr_TimedAnswer(lastSeq), "didn't take its answer");
        CHECK(jkrelay_TimedState(&rtt, &base, &waited, &at) == 1 && rtt == 45 && base == 45 && at == sentAt + 45 && waited < 0,
              "%.0f %.0f %.0f %.0f", rtt, base, at - sentAt, waited);
    }

    // Not in a game: nothing.
    {
        int before = sends;
        R.inSession = 0;
        nowMs += 20000;
        jkrelay_AskTimed();
        CHECK(sends == before, "sent %d out of a game", sends - before);
    }

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
