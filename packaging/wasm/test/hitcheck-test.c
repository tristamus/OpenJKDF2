// Tests for the hit claim check (src/Platform/Networking/WSRelay/
// stdComm_WSRelayHitCheck.c), on its own, with made-up records and claims.
// From the top of the source tree:
//
//   cc -std=c11 -O2 -Wall -o hitcheck-test packaging/wasm/test/hitcheck-test.c
//      src/Platform/Networking/WSRelay/stdComm_WSRelayHitCheck.c -lm
//   ./hitcheck-test

#include "../../../src/Platform/Networking/WSRelay/stdComm_WSRelayHitCheck.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static WSRelayHitVec vec(float x, float y, float z)
{
    WSRelayHitVec v = { x, y, z };
    return v;
}

static int near(double a, double b, double eps)
{
    return fabs(a - b) <= eps;
}

static WSRelayHitRecord record;

// A player running along x at speed JKU/s from t0 to t1, one frame every
// frameMs, in life 1.
static void run(double t0, double t1, double frameMs, float speed)
{
    stdComm_WSRelayHit_ClearRecord(&record);
    for (double t = t0; t <= t1 + 1e-9; t += frameMs)
        stdComm_WSRelayHit_Record(&record, t, vec((float)((t - t0) / 1000.0 * speed), 0, 0), 1, 1);
}

static void test_record(void)
{
    // Spots stay at least 4 ms apart however fast the frames come.
    stdComm_WSRelayHit_ClearRecord(&record);
    for (int i = 0; i <= 1000; i++)
        stdComm_WSRelayHit_Record(&record, 1000.0 + i, vec((float)i, 0, 0), 1, 1);
    CHECK(record.count >= 200 && record.count <= 260, "1 ms frames for 1 s kept %d spots", record.count);
    double at;
    float d = stdComm_WSRelayHit_Closest(&record, 1000.0, 2000.0, 1, vec(500.0f, 0.3f, 0), &at);
    CHECK(near(d, 0.3, 1e-3) && near(at, 1500.0, 1.0), "path through 1 ms frames: %f at %f", d, at);

    // The ring keeps the newest.
    stdComm_WSRelayHit_ClearRecord(&record);
    for (int i = 0; i < 3 * WSRELAYHIT_RECORD_LEN; i++)
        stdComm_WSRelayHit_Record(&record, i * 10.0, vec((float)i, 0, 0), 1, 1);
    CHECK(record.count == WSRELAYHIT_RECORD_LEN, "ring holds %d", record.count);
    d = stdComm_WSRelayHit_Closest(&record, 0.0, 1e9, 1, vec(5.0f, 0, 0), &at);
    CHECK(near(d, 3 * WSRELAYHIT_RECORD_LEN - WSRELAYHIT_RECORD_LEN - 5, 1e-3), "oldest kept is the ring's length back: %f", d);

    // Time never goes back.
    stdComm_WSRelayHit_ClearRecord(&record);
    stdComm_WSRelayHit_Record(&record, 100.0, vec(0, 0, 0), 1, 1);
    stdComm_WSRelayHit_Record(&record, 50.0, vec(9, 9, 9), 1, 1);
    CHECK(record.count == 1, "an older frame is left out");
}

static void test_closest(void)
{
    // 1.5 JKU/s along x, 16 ms frames, from t = 1000 to 2000.
    run(1000.0, 2000.0, 16.0, 1.5f);
    double at;

    float d = stdComm_WSRelayHit_Closest(&record, 1000.0, 2000.0, 1, vec(0.75f, 0.1f, 0), &at);
    CHECK(near(d, 0.1, 1e-4) && near(at, 1500.0, 1.0), "beside the path: %f at %f", d, at);

    // Only the part of the path in the window counts.
    d = stdComm_WSRelayHit_Closest(&record, 1700.0, 2000.0, 1, vec(0.75f, 0, 0), &at);
    CHECK(near(d, 0.3, 1e-3) && near(at, 1700.0, 1e-6), "window starts after it: %f at %f", d, at);
    d = stdComm_WSRelayHit_Closest(&record, 1000.0, 1300.0, 1, vec(0.75f, 0, 0), &at);
    CHECK(near(d, 0.3, 1e-3) && near(at, 1300.0, 1e-6), "window ends before it: %f at %f", d, at);

    // Past the newest spot (t = 1992), the player is where it left them.
    d = stdComm_WSRelayHit_Closest(&record, 2500.0, 2600.0, 1, vec(1.488f, 0, 0), &at);
    CHECK(near(d, 0.0, 1e-3), "newest spot holds: %f", d);

    // Another life doesn't count.
    d = stdComm_WSRelayHit_Closest(&record, 1000.0, 2000.0, 2, vec(0.75f, 0, 0), &at);
    CHECK(d < 0.0f, "no spots of life 2: %f", d);

    // Dead spots don't count, and nothing joins the last live spot of a life
    // to the first of the next.
    stdComm_WSRelayHit_ClearRecord(&record);
    stdComm_WSRelayHit_Record(&record, 1000.0, vec(0, 0, 0), 1, 1);
    stdComm_WSRelayHit_Record(&record, 1010.0, vec(1, 0, 0), 1, 1);
    stdComm_WSRelayHit_Record(&record, 1020.0, vec(1, 0, 0), 2, 0);
    stdComm_WSRelayHit_Record(&record, 1030.0, vec(5, 0, 0), 2, 1);
    stdComm_WSRelayHit_Record(&record, 1040.0, vec(6, 0, 0), 2, 1);
    d = stdComm_WSRelayHit_Closest(&record, 0.0, 2000.0, 2, vec(3.0f, 0, 0), &at);
    CHECK(near(d, 2.0, 1e-4) && near(at, 1030.0, 1e-6), "life 2 starts at its first live spot: %f at %f", d, at);
    d = stdComm_WSRelayHit_Closest(&record, 0.0, 2000.0, 1, vec(3.0f, 0, 0), &at);
    CHECK(near(d, 2.0, 1e-4) && near(at, 1010.0, 1e-6), "life 1 ends at its last live spot: %f at %f", d, at);
    d = stdComm_WSRelayHit_Closest(&record, 1015.0, 2000.0, 1, vec(1.0f, 0, 0), &at);
    CHECK(d < 0.0f, "life 1 is over before the window: %f", d);
}

static void test_window(void)
{
    CHECK(near(stdComm_WSRelayHit_WindowFrom(5000.0, 300.0, -1.0), 4700.0, 1e-9), "no update number: the reach");
    CHECK(near(stdComm_WSRelayHit_WindowFrom(5000.0, 300.0, 4900.0), 4800.0, 1e-9), "a recent update: from it, less the slack");
    CHECK(near(stdComm_WSRelayHit_WindowFrom(5000.0, 300.0, 4000.0), 4700.0, 1e-9), "an old update: the reach");
    CHECK(near(stdComm_WSRelayHit_WindowFrom(5000.0, 150.0, 4950.0), 4850.0, 1e-9), "150 ms reach");
    CHECK(near(stdComm_WSRelayHit_WindowFrom(5000.0, 300.0, 5200.0), 5000.0, 1e-9), "never after now");
}

static WSRelayHitClaim saberClaim(WSRelayHitVec target, WSRelayHitVec attacker, float damage)
{
    WSRelayHitClaim c;
    memset(&c, 0, sizeof(c));
    c.source = WSRELAYHIT_SABER;
    c.damage = damage;
    c.target = target;
    c.attacker = attacker;
    return c;
}

static WSRelayHitAttack swing(WSRelayHitVec attackerShown)
{
    WSRelayHitAttack a;
    memset(&a, 0, sizeof(a));
    a.known = 1;
    a.maxDamage = 50.0f;
    a.bladeLength = 0.15f;
    a.from = attackerShown;
    return a;
}

static void test_check_saber(void)
{
    run(1000.0, 2000.0, 16.0, 1.5f);
    WSRelayHitVec target = vec(0.75f, 0.05f, 0);      // where the attacker saw them at t = 1500
    WSRelayHitVec attacker = vec(0.75f, 0.25f, 0);
    WSRelayHitClaim c = saberClaim(target, attacker, 50.0f);
    WSRelayHitAttack a = swing(vec(0.8f, 0.3f, 0));
    double now = 1650.0;
    double from = stdComm_WSRelayHit_WindowFrom(now, 300.0, 1495.0);

    WSRelayHitVerdict v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_OK && near(v.damage, 50.0, 1e-6) && near(v.distance, 0.05, 1e-3), "saber hit: %d %f %f", v.result, v.damage, v.distance);

    // Damage past the swing's is cut to it.
    c.damage = 500.0f;
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_OK && near(v.damage, 50.0, 1e-6), "damage cut: %d %f", v.result, v.damage);
    c.damage = 50.0f;

    // Dead now.
    v = stdComm_WSRelayHit_Check(&record, 1, 0, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_DEAD, "dead: %d", v.result);

    // No such swing.
    a.known = 0;
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_UNKNOWN, "unknown swing: %d", v.result);
    a.known = 1;

    // The blade can't reach that far.
    c.attacker = vec(0.75f, 0.7f, 0);
    a.from = c.attacker;
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_OUT_OF_REACH, "blade too short: %d", v.result);

    // The attacker says they were somewhere this game never showed them.
    c.attacker = attacker;
    a.from = vec(3.0f, 0.3f, 0);
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_OUT_OF_REACH, "attacker elsewhere: %d", v.result);
    a.from = vec(0.8f, 0.3f, 0);

    // The player had moved on before the window: 300 ms reach, and the claim
    // came 600 ms after. Moving 1.5 JKU/s, they were 0.45 JKU on by the
    // window's start.
    now = 2100.0;
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, stdComm_WSRelayHit_WindowFrom(now, 300.0, -1.0));
    CHECK(v.result == WSRELAYHIT_NOT_THERE && near(v.distance, 0.45, 0.01), "past the reach: %d %f", v.result, v.distance);

    // With 500 ms, only 0.15 JKU: within the tolerance, which lets a hit
    // count up to about 130 ms more at a run.
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, stdComm_WSRelayHit_WindowFrom(now, 500.0, -1.0));
    CHECK(v.result == WSRELAYHIT_OK && near(v.distance, 0.15, 0.01), "600 ms with 500 ms reach: %d %f", v.result, v.distance);
    now = 1950.0;
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, stdComm_WSRelayHit_WindowFrom(now, 150.0, -1.0));
    CHECK(v.result == WSRELAYHIT_NOT_THERE, "450 ms with 150 ms reach: %d %f", v.result, v.distance);

    // The update number starts the window later than the reach: an update
    // sent at 1800 can't have shown them at 1500.
    now = 1750.0;
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, stdComm_WSRelayHit_WindowFrom(now, 300.0, 1800.0));
    CHECK(v.result == WSRELAYHIT_NOT_THERE, "update after the spot: %d %f", v.result, v.distance);

    // Died and came back since.
    v = stdComm_WSRelayHit_Check(&record, 2, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_LATE, "new life: %d", v.result);

    // Nonsense.
    c.damage = NAN;
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_BAD, "NaN damage: %d", v.result);
    c.damage = -5.0f;
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_BAD, "negative damage: %d", v.result);
    c.damage = 50.0f;
    c.source = 9;
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_BAD, "no such source: %d", v.result);
}

static void test_check_shots(void)
{
    run(1000.0, 3000.0, 16.0, 1.5f);
    WSRelayHitClaim c;
    memset(&c, 0, sizeof(c));
    c.source = WSRELAYHIT_SHOT;
    c.damage = 30.0f;
    c.target = vec(1.5f, 0, 0);             // at t = 2000
    c.attacker = vec(1.5f, 2.0f, 0);

    // A bryar bolt, 4 JKU/s, fired 2 JKU away; its fire message came at 1500.
    WSRelayHitAttack a;
    memset(&a, 0, sizeof(a));
    a.known = 1;
    a.at = 1500.0;
    a.maxDamage = 30.0f;
    a.from = vec(1.5f, 2.0f, 0);
    a.speed = 4.0f;
    double now = 2100.0;
    double from = stdComm_WSRelayHit_WindowFrom(now, 300.0, -1.0);
    WSRelayHitVerdict v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_OK, "bolt hit: %d %f", v.result, v.distance);

    // A bolt that couldn't have got there yet: 6 JKU away after 0.6 s at 4 JKU/s.
    a.from = vec(1.5f, 6.0f, 0);
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_OUT_OF_REACH, "too far for the bolt: %d", v.result);

    // A shot that hits at once reaches its range.
    a.speed = 0.0f;
    a.range = 0.15f;
    a.from = vec(1.5f, 0.2f, 0);
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_OK, "punch in reach: %d", v.result);
    a.from = vec(1.5f, 1.0f, 0);
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_OUT_OF_REACH, "punch out of reach: %d", v.result);

    // A falling grenade goes farther with gravity.
    a.range = 0.0f;
    a.speed = 2.5f;
    a.from = vec(1.5f, 4.0f, 0);
    a.at = 1000.0;
    now = 2100.0;
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_OUT_OF_REACH, "4 JKU in 1.2 s at 2.5 JKU/s without gravity: %d", v.result);
    a.gravity = 4.0f;
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_OK, "with gravity: %d", v.result);

    // A blast: where it went off has to be in its range of the player.
    c.source = WSRELAYHIT_SPLASH;
    c.blast = vec(1.5f, 0.5f, 0);
    a.blastRange = 0.6f;
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_OK, "blast in range: %d", v.result);
    c.blast = vec(1.5f, 1.2f, 0);
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, from);
    CHECK(v.result == WSRELAYHIT_OUT_OF_REACH, "blast out of range: %d", v.result);
}

// A target strafing 700 ms each way at 1.5 JKU/s, as ?nettest's does, with
// position updates every 20 ms. The attacker's game shows each update when it
// arrives, one way later; its claim takes one way back. Every hit on the
// target as shown passes up to a round trip of about 250 ms, with 300 ms of
// reach.
static float strafeX(double t)
{
    double phase = fmod(t, 1400.0);
    return (float)(phase < 700.0 ? phase / 1000.0 * 1.5 : (1400.0 - phase) / 1000.0 * 1.5);
}

static void test_strafe(void)
{
    stdComm_WSRelayHit_ClearRecord(&record);
    for (double t = 0.0; t <= 10000.0; t += 16.0)
        stdComm_WSRelayHit_Record(&record, t, vec(strafeX(t), 0, 0), 1, 1);

    int rtts[] = { 0, 60, 120, 200, 260 };
    for (int r = 0; r < 5; r++)
    {
        double oneWay = rtts[r] / 2.0;
        int passed = 0, total = 0;
        for (double sent = 8000.0; sent < 9400.0; sent += 20.0)
        {
            // The update sent at `sent` is on the attacker's screen until the
            // next arrives; it swings 10 ms into that.
            double hitAt = sent + oneWay + 10.0;
            double now = hitAt + oneWay;
            WSRelayHitClaim c = saberClaim(vec(strafeX(sent), 0, 0), vec(strafeX(sent), 0.2f, 0), 50.0f);
            WSRelayHitAttack a = swing(vec(strafeX(sent), 0.2f, 0));
            WSRelayHitVerdict v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, stdComm_WSRelayHit_WindowFrom(now, 300.0, sent));
            total++;
            if (v.result == WSRELAYHIT_OK)
                passed++;
        }
        CHECK(passed == total, "strafing, %d ms round trip: %d of %d passed", rtts[r], passed, total);
    }

    // With the update number unknown, past the reach a hit fails once the
    // target has moved more than the tolerance: shown at 0.15 JKU, moving
    // away, and the window starts 200 ms on, at 0.45.
    double sent = 7100.0;
    double now = sent + 500.0;
    WSRelayHitClaim c = saberClaim(vec(strafeX(sent), 0, 0), vec(strafeX(sent), 0.2f, 0), 50.0f);
    WSRelayHitAttack a = swing(vec(strafeX(sent), 0.2f, 0));
    WSRelayHitVerdict v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, stdComm_WSRelayHit_WindowFrom(now, 300.0, -1.0));
    CHECK(v.result == WSRELAYHIT_NOT_THERE, "400 ms late, 300 ms reach: %d %f", v.result, v.distance);
}

// The window a claim that names the moment r it drew the target at is
// checked in, from the times the claim gives as the hit ledger
// (stdComm_WSRelayHits.c) sends them: ms from the update before r, and to
// the one after (none when tAfter < 0), checked 150 ms later with 300 ms of
// reach.
static void momentWindow(double r, double tBefore, double tAfter, double* pFrom, double* pTo)
{
    double after = tAfter < 0.0 ? -1.0 : ceil(tAfter - r);
    int result = stdComm_WSRelayHit_MomentWindow(r + 150.0, 300.0, r, ceil(r - tBefore), after, pFrom, pTo);
    CHECK(result == WSRELAYHIT_OK, "window at %f: %d", r, result);
}

// The attacker's game draws the strafing target from its timed updates,
// sent every third 150 Hz step, and names the moment it drew them at. The
// target's game checks only between the two updates that moment fell
// between, on its record of every step: each hit at the moment passes, and
// a place the target was 200 ms from it doesn't, though the window by the
// reach would take it.
static void test_moment(void)
{
    stdComm_WSRelayHit_ClearRecord(&record);
    for (int step = 0; step <= 1500; step++)
    {
        double t = floor(step * 1000.0 / 150.0 + 0.5);
        stdComm_WSRelayHit_Record(&record, t, vec(strafeX(t), 0, 0), 1, 1);
    }

    int passed = 0, total = 0;
    double worst = 0.0;
    for (double r = 8500.0; r < 9700.0; r += 7.0)
    {
        double tBefore = 20.0 * floor(r / 20.0), tAfter = tBefore + 20.0;
        float x = strafeX(tBefore) + (strafeX(tAfter) - strafeX(tBefore)) * (float)((r - tBefore) / 20.0);
        WSRelayHitClaim c = saberClaim(vec(x, 0, 0), vec(x, 0.2f, 0), 50.0f);
        WSRelayHitAttack a = swing(vec(x, 0.2f, 0));
        double from, to;
        momentWindow(r, tBefore, tAfter, &from, &to);
        WSRelayHitVerdict v = stdComm_WSRelayHit_CheckIn(&record, 1, 1, &c, &a, r + 150.0, from, to);
        total++;
        if (v.result == WSRELAYHIT_OK)
            passed++;
        if (v.distance > worst)
            worst = v.distance;
    }
    CHECK(passed == total && worst < 0.01, "at the moment: %d of %d passed, %f JKU off at most", passed, total, worst);

    // Shown where they were 200 ms before the moment, 0.3 JKU back along
    // their path, and 0.28 from where they were 1 ms before the update
    // before it. The window by the reach, from 300 ms before the claim,
    // takes it.
    double r = 9450.0, now = r + 150.0, from, to;
    WSRelayHitClaim c = saberClaim(vec(strafeX(r - 200.0), 0, 0), vec(strafeX(r - 200.0), 0.2f, 0), 50.0f);
    WSRelayHitAttack a = swing(vec(strafeX(r - 200.0), 0.2f, 0));
    momentWindow(r, 9440.0, 9460.0, &from, &to);
    WSRelayHitVerdict v = stdComm_WSRelayHit_CheckIn(&record, 1, 1, &c, &a, now, from, to);
    CHECK(v.result == WSRELAYHIT_NOT_THERE && near(v.distance, 0.2835, 0.001), "200 ms from the moment: %d %f", v.result, v.distance);
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, stdComm_WSRelayHit_WindowFrom(now, 300.0, -1.0));
    CHECK(v.result == WSRELAYHIT_OK, "the same by the reach: %d %f", v.result, v.distance);

    // Carried on 60 ms past the newest update along its velocity.
    r = 9460.0;
    float x = strafeX(9400.0) - 1.5f * 0.06f;
    c = saberClaim(vec(x, 0, 0), vec(x, 0.2f, 0), 50.0f);
    a = swing(vec(x, 0.2f, 0));
    momentWindow(r, 9400.0, -1.0, &from, &to);
    CHECK(near(from, 9399.0, 1e-9) && near(to, 9461.0, 1e-9), "carried: window %f to %f", from, to);
    v = stdComm_WSRelayHit_CheckIn(&record, 1, 1, &c, &a, r + 150.0, from, to);
    CHECK(v.result == WSRELAYHIT_OK && v.distance < 0.01f, "carried 60 ms: %d %f", v.result, v.distance);

    // Died and came back since, or the record no longer goes back that far.
    v = stdComm_WSRelayHit_CheckIn(&record, 2, 1, &c, &a, r + 150.0, from, to);
    CHECK(v.result == WSRELAYHIT_LATE, "new life: %d", v.result);
    momentWindow(5000.0, 4980.0, 5000.0, &from, &to);
    v = stdComm_WSRelayHit_CheckIn(&record, 1, 1, &c, &a, 5150.0, from, to);
    CHECK(v.result == WSRELAYHIT_LATE, "older than the record: %d", v.result);
}

// How old a moment can be: no further back than the reach, and never ahead
// of this game's clock. One further back is checked as a claim without a
// moment (stdComm_WSRelayHits.c), anywhere in the reach: a player who stood
// still since is hit there, one who ran on isn't.
static void test_moment_reach(void)
{
    double from = 0.0, to = 0.0;
    int result = stdComm_WSRelayHit_MomentWindow(5000.0, 300.0, 4800.0, 12.0, 8.0, &from, &to);
    CHECK(result == WSRELAYHIT_OK && near(from, 4787.0, 1e-9) && near(to, 4809.0, 1e-9), "200 ms old: %d %f %f", result, from, to);
    result = stdComm_WSRelayHit_MomentWindow(5000.0, 300.0, 4800.0, 12.0, -1.0, &from, &to);
    CHECK(result == WSRELAYHIT_OK && near(to, 4801.0, 1e-9), "carried on: to the moment: %d %f", result, to);
    result = stdComm_WSRelayHit_MomentWindow(5000.0, 300.0, 4700.0, 12.0, 8.0, &from, &to);
    CHECK(result == WSRELAYHIT_OK, "300 ms old: %d", result);
    result = stdComm_WSRelayHit_MomentWindow(5000.0, 300.0, 4699.0, 12.0, 8.0, &from, &to);
    CHECK(result == WSRELAYHIT_LATE, "301 ms old: %d", result);
    result = stdComm_WSRelayHit_MomentWindow(5000.0, 500.0, 4600.0, 12.0, 8.0, &from, &to);
    CHECK(result == WSRELAYHIT_OK, "400 ms old, 500 ms reach: %d", result);
    result = stdComm_WSRelayHit_MomentWindow(5000.0, 150.0, 4800.0, 12.0, 8.0, &from, &to);
    CHECK(result == WSRELAYHIT_LATE, "200 ms old, 150 ms reach: %d", result);

    // Ahead of this game's clock, or nonsense.
    result = stdComm_WSRelayHit_MomentWindow(5000.0, 300.0, 5040.0, 12.0, 8.0, &from, &to);
    CHECK(result == WSRELAYHIT_OK, "40 ms ahead: %d", result);
    result = stdComm_WSRelayHit_MomentWindow(5000.0, 300.0, 5060.0, 12.0, 8.0, &from, &to);
    CHECK(result == WSRELAYHIT_BAD, "60 ms ahead: %d", result);
    result = stdComm_WSRelayHit_MomentWindow(5000.0, 300.0, NAN, 12.0, 8.0, &from, &to);
    CHECK(result == WSRELAYHIT_BAD, "NaN: %d", result);
    result = stdComm_WSRelayHit_MomentWindow(5000.0, 300.0, 4800.0, -1.0, 8.0, &from, &to);
    CHECK(result == WSRELAYHIT_BAD, "update before after the moment: %d", result);

    // 450 ms old at the moment, then checked in the reach: 0.225 JKU from
    // the nearest place the strafing player was in it.
    double r = 8500.0, now = 8950.0;
    result = stdComm_WSRelayHit_MomentWindow(now, 300.0, r, 7.0, 13.0, &from, &to);
    CHECK(result == WSRELAYHIT_LATE, "450 ms old: %d", result);
    stdComm_WSRelayHit_ClearRecord(&record);
    for (double t = 8000.0; t <= now; t += 1000.0 / 150.0)
        stdComm_WSRelayHit_Record(&record, floor(t + 0.5), vec(10.0f, 0, 0), 1, 1);
    WSRelayHitClaim c = saberClaim(vec(10.0f, 0, 0), vec(10.0f, 0.2f, 0), 50.0f);
    WSRelayHitAttack a = swing(vec(10.0f, 0.2f, 0));
    WSRelayHitVerdict v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, stdComm_WSRelayHit_WindowFrom(now, 300.0, -1.0));
    CHECK(v.result == WSRELAYHIT_OK && v.distance < 0.001f, "stood still since: %d %f", v.result, v.distance);
    stdComm_WSRelayHit_ClearRecord(&record);
    for (double t = 8000.0; t <= now; t += 1000.0 / 150.0)
        stdComm_WSRelayHit_Record(&record, floor(t + 0.5), vec(strafeX(floor(t + 0.5)), 0, 0), 1, 1);
    c = saberClaim(vec(strafeX(r), 0, 0), vec(strafeX(r), 0.2f, 0), 50.0f);
    a = swing(vec(strafeX(r), 0.2f, 0));
    v = stdComm_WSRelayHit_Check(&record, 1, 1, &c, &a, now, stdComm_WSRelayHit_WindowFrom(now, 300.0, -1.0));
    CHECK(v.result == WSRELAYHIT_NOT_THERE && near(v.distance, 0.225, 0.002), "ran on since: %d %f", v.result, v.distance);
}

// A targeted Force power (stdComm_WSRelayHits.c) only has the place the
// attacker's game showed the player at a moment to check, as a claim's last
// check (CheckThere). A Grip holds its victim still, so one aimed at them
// before that can be past the reach and still find them there.
static void test_there(void)
{
    stdComm_WSRelayHit_ClearRecord(&record);
    for (int step = 0; step <= 1500; step++)
    {
        double t = floor(step * 1000.0 / 150.0 + 0.5);
        stdComm_WSRelayHit_Record(&record, t, vec(strafeX(t), 0, 0), 1, 1);
    }

    int passed = 0, total = 0;
    double worst = 0.0;
    for (double r = 8500.0; r < 9700.0; r += 7.0)
    {
        double tBefore = 20.0 * floor(r / 20.0), tAfter = tBefore + 20.0;
        float x = strafeX(tBefore) + (strafeX(tAfter) - strafeX(tBefore)) * (float)((r - tBefore) / 20.0);
        double from, to;
        momentWindow(r, tBefore, tAfter, &from, &to);
        WSRelayHitVerdict v = stdComm_WSRelayHit_CheckThere(&record, 1, vec(x, 0, 0), from, to);
        total++;
        if (v.result == WSRELAYHIT_OK && v.damage == 0.0f)
            passed++;
        if (v.distance > worst)
            worst = v.distance;
    }
    CHECK(passed == total && worst < 0.01, "at the moment: %d of %d there, %f JKU off at most", passed, total, worst);

    // Shown 200 ms from the moment: as far off as test_moment's claim.
    double from, to;
    momentWindow(9450.0, 9440.0, 9460.0, &from, &to);
    WSRelayHitVerdict v = stdComm_WSRelayHit_CheckThere(&record, 1, vec(strafeX(9250.0), 0, 0), from, to);
    CHECK(v.result == WSRELAYHIT_NOT_THERE && near(v.distance, 0.2835, 0.001), "200 ms from the moment: %d %f", v.result, v.distance);

    // Died and came back since, or nonsense.
    v = stdComm_WSRelayHit_CheckThere(&record, 2, vec(strafeX(9450.0), 0, 0), from, to);
    CHECK(v.result == WSRELAYHIT_LATE && v.distance < 0.0f, "new life: %d %f", v.result, v.distance);
    v = stdComm_WSRelayHit_CheckThere(&record, 1, vec(NAN, 0, 0), from, to);
    CHECK(v.result == WSRELAYHIT_BAD, "NaN place: %d", v.result);
    v = stdComm_WSRelayHit_CheckThere(&record, 1, vec(strafeX(9450.0), 0, 0), from, INFINITY);
    CHECK(v.result == WSRELAYHIT_BAD, "endless window: %d", v.result);

    // Held still from 8450 on: a moment 450 ms old, further back than the
    // reach, is checked anywhere in the reach, and they're still there.
    double now = 8950.0;
    float held = strafeX(8450.0);
    stdComm_WSRelayHit_ClearRecord(&record);
    for (double t = 8000.0; t <= now; t += 1000.0 / 150.0)
    {
        double at = floor(t + 0.5);
        stdComm_WSRelayHit_Record(&record, at, vec(at < 8450.0 ? strafeX(at) : held, 0, 0), 1, 1);
    }
    int result = stdComm_WSRelayHit_MomentWindow(now, 300.0, 8500.0, 7.0, 13.0, &from, &to);
    CHECK(result == WSRELAYHIT_LATE, "450 ms old: %d", result);
    v = stdComm_WSRelayHit_CheckThere(&record, 1, vec(held, 0, 0), stdComm_WSRelayHit_WindowFrom(now, 300.0, -1.0), now);
    CHECK(v.result == WSRELAYHIT_OK && v.distance < 0.001f, "held still since: %d %f", v.result, v.distance);
}

static void test_seen(void)
{
    WSRelayHitSeen seen;
    stdComm_WSRelayHit_ClearSeen(&seen);
    CHECK(!stdComm_WSRelayHit_Seen(&seen, 1000.0, 2, WSRELAYHIT_SABER, 7), "nothing seen yet");
    stdComm_WSRelayHit_AddSeen(&seen, 1000.0, 2, WSRELAYHIT_SABER, 7);
    CHECK(stdComm_WSRelayHit_Seen(&seen, 1500.0, 2, WSRELAYHIT_SABER, 7) == 1, "seen once");
    CHECK(!stdComm_WSRelayHit_Seen(&seen, 1500.0, 3, WSRELAYHIT_SABER, 7), "another attacker");
    // A blast with two explosions (MotS's flash bomb) counts twice.
    stdComm_WSRelayHit_AddSeen(&seen, 1200.0, 2, WSRELAYHIT_SPLASH, 9);
    CHECK(stdComm_WSRelayHit_Seen(&seen, 1500.0, 2, WSRELAYHIT_SPLASH, 9) == 1, "one of a blast's explosions");
    stdComm_WSRelayHit_AddSeen(&seen, 1200.0, 2, WSRELAYHIT_SPLASH, 9);
    CHECK(stdComm_WSRelayHit_Seen(&seen, 1500.0, 2, WSRELAYHIT_SPLASH, 9) == 2, "both of them");
    CHECK(!stdComm_WSRelayHit_Seen(&seen, 1500.0, 2, WSRELAYHIT_SPLASH, 7), "another kind: a shot's blast after the shot");
    CHECK(!stdComm_WSRelayHit_Seen(&seen, 1000.0 + WSRELAYHIT_SEEN_MS + 1.0, 2, WSRELAYHIT_SABER, 7), "forgotten after a while");
    for (int i = 0; i < WSRELAYHIT_SEEN_LEN; i++)
        stdComm_WSRelayHit_AddSeen(&seen, 1100.0, 4, WSRELAYHIT_SHOT, 100 + i);
    CHECK(!stdComm_WSRelayHit_Seen(&seen, 1500.0, 2, WSRELAYHIT_SABER, 7), "pushed out by newer ones");
    CHECK(stdComm_WSRelayHit_Seen(&seen, 1500.0, 4, WSRELAYHIT_SHOT, 100 + WSRELAYHIT_SEEN_LEN - 1), "the newest stays");
}

int main(void)
{
    test_record();
    test_closest();
    test_window();
    test_check_saber();
    test_check_shots();
    test_strafe();
    test_moment();
    test_moment_reach();
    test_there();
    test_seen();
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
