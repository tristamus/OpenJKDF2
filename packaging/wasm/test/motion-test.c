// Tests for drawing other players from a buffer of their updates
// (src/Platform/Networking/WSRelay/stdComm_WSRelayPlayout.c), on its own, with
// made-up updates and links. From the top of the source tree:
//
//   cc -std=c11 -O2 -Wall -o motion-test packaging/wasm/test/motion-test.c
//      src/Platform/Networking/WSRelay/stdComm_WSRelayPlayout.c -lm
//   ./motion-test

#include "../../../src/Platform/Networking/WSRelay/stdComm_WSRelayPlayout.h"

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

static int near(double a, double b, double eps)
{
    return fabs(a - b) <= eps;
}

static WSRelayPlayoutVec vec(float x, float y, float z)
{
    WSRelayPlayoutVec v = { x, y, z };
    return v;
}

// A sender running along x at 2 JKU/s, whose clock reads 1,000,000 ms more
// than this game's.
#define SPEED (2.0)
#define CLOCK_AHEAD (1000000.0)

static WSRelayPlayoutSample sample(double t, double taken)
{
    WSRelayPlayoutSample s;
    memset(&s, 0, sizeof(s));
    s.t = t;
    s.taken = taken;
    s.pos = vec((float)(t / 1000.0 * SPEED), 1.0f, 0.0f);
    s.vel = vec((float)SPEED, 0.0f, 0.0f);
    s.angles = vec(0.0f, 90.0f, 0.0f);
    s.sector = 7;
    return s;
}

// The sender's x at its time t.
static double xAt(double t)
{
    return t / 1000.0 * SPEED;
}

static uint32_t rng = 12345;

static double rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return ((rng >> 8) & 0xFFFF) / 65536.0;
}

static WSRelayPlayout po;

// What a run of a link shows: the sender sends every gapMs from its time
// tFrom on, each update taking trip(n) ms to get here, and this game draws
// every 1000/150 ms from nowFrom to nowTo.
typedef struct Run
{
    int poses;
    int carried;
    int held;
    int backwards;          // poses earlier than the one before
    double maxJump;         // largest step in x between two poses, past
                            // what the speed explains
    double minDelay, maxDelay;
    double lastDelay;
    double lastR;
} Run;

typedef double (*TripFunc)(int n);

static Run drive(double tFrom, double gapMs, TripFunc trip, double nowFrom, double nowTo)
{
    Run run;
    memset(&run, 0, sizeof(run));
    run.minDelay = 1e18;
    run.maxDelay = -1e18;

    // Updates waiting to get here, in the order they arrive.
    static WSRelayPlayoutSample aPending[4096];
    int numPending = 0;
    int n = 0;
    double nextSend = tFrom;
    double prevX = 0.0, prevR = -1e18;
    int bPrev = 0;
    double step = 1000.0 / 150.0;

    for (double now = nowFrom; now <= nowTo; now += step)
    {
        // Everything sent up to now (on the sender's clock), on its way.
        while (nextSend - CLOCK_AHEAD <= now && numPending < 4096)
        {
            double t = nextSend;
            WSRelayPlayoutSample s = sample(t, t - CLOCK_AHEAD + trip(n++));
            // Kept in arrival order.
            int i = numPending++;
            while (i > 0 && aPending[i - 1].taken > s.taken)
            {
                aPending[i] = aPending[i - 1];
                i--;
            }
            aPending[i] = s;
            nextSend += gapMs;
        }
        // What has arrived by now.
        int taken = 0;
        while (taken < numPending && aPending[taken].taken <= now)
            stdComm_WSRelayPlayout_Add(&po, &aPending[taken++]);
        memmove(aPending, aPending + taken, (numPending - taken) * sizeof(aPending[0]));
        numPending -= taken;

        WSRelayPlayoutPose pose;
        if (!stdComm_WSRelayPlayout_Pose(&po, now, &pose))
            continue;
        run.poses++;
        if (pose.carried == 1)
            run.carried++;
        if (pose.carried == 2)
            run.held++;
        if (bPrev)
        {
            if (pose.r < prevR - 1e-6)
                run.backwards++;
            double moved = fabs(pose.pos.x - prevX);
            double explained = (pose.r - prevR) / 1000.0 * SPEED;
            if (moved - fabs(explained) > run.maxJump)
                run.maxJump = moved - fabs(explained);
        }
        prevX = pose.pos.x;
        prevR = pose.r;
        bPrev = 1;
        double delay = po.delay + CLOCK_AHEAD;
        run.lastDelay = delay;
        if (delay < run.minDelay)
            run.minDelay = delay;
        if (delay > run.maxDelay)
            run.maxDelay = delay;
        run.lastR = pose.r;
    }
    return run;
}

static double trip40(int n)
{
    (void)n;
    return 40.0;
}

static double trip40to80(int n)
{
    (void)n;
    return 40.0 + 40.0 * rnd();
}

static double trip300(int n)
{
    (void)n;
    return 300.0;
}

static double trip100(int n)
{
    (void)n;
    return 100.0;
}

// One update in 20 comes 400 ms late, behind the ones after it.
static double tripLateOne(int n)
{
    return (n % 20 == 10) ? 440.0 : 40.0;
}

static void test_unwrap(void)
{
    stdComm_WSRelayPlayout_Clear(&po);
    CHECK(stdComm_WSRelayPlayout_Unwrap(&po, 1234u) == 1234.0, "no updates: as sent");

    WSRelayPlayoutSample s = sample(4294967000.0, 0.0);
    stdComm_WSRelayPlayout_Add(&po, &s);
    CHECK(stdComm_WSRelayPlayout_Unwrap(&po, 4294967100u) == 4294967100.0, "just after the newest");
    CHECK(stdComm_WSRelayPlayout_Unwrap(&po, 100u) == 4294967396.0, "past the 32-bit wrap: %f", stdComm_WSRelayPlayout_Unwrap(&po, 100u));
    CHECK(stdComm_WSRelayPlayout_Unwrap(&po, 4294966000u) == 4294966000.0, "a little before the newest");

    stdComm_WSRelayPlayout_Clear(&po);
    s = sample(4294967396.0, 0.0);
    stdComm_WSRelayPlayout_Add(&po, &s);
    CHECK(stdComm_WSRelayPlayout_Unwrap(&po, 4294967000u) == 4294967000.0, "back across the wrap: %f", stdComm_WSRelayPlayout_Unwrap(&po, 4294967000u));
}

static void test_since(void)
{
    CHECK(near(stdComm_WSRelayPlayout_Since(1000.4, 900u), 100.4, 1e-9), "100.4 ms before");
    CHECK(near(stdComm_WSRelayPlayout_Since(1000.4, 1000u), 0.4, 1e-9), "this ms");
    CHECK(near(stdComm_WSRelayPlayout_Since(1000.0, 1030u), -30.0, 1e-9), "after: negative");
    CHECK(near(stdComm_WSRelayPlayout_Since(4294967296.0 + 50.0, 4294967246u), 100.0, 1e-9),
          "sent before the 32-bit wrap, now after it: %f", stdComm_WSRelayPlayout_Since(4294967296.0 + 50.0, 4294967246u));
    CHECK(near(stdComm_WSRelayPlayout_Since(4294967246.5, 50u), -99.5, 1e-9), "now before the wrap, sent after it");
    CHECK(near(stdComm_WSRelayPlayout_Since(3.0 * 4294967296.0 + 7.25, 2u), 5.25, 1e-6), "a few wraps on");
}

static void test_add(void)
{
    stdComm_WSRelayPlayout_Clear(&po);
    WSRelayPlayoutSample s = sample(1000.0, 10.0);
    CHECK(stdComm_WSRelayPlayout_Add(&po, &s) == 1, "first update taken");
    CHECK(stdComm_WSRelayPlayout_Add(&po, &s) == 0, "the same one again dropped");
    s = sample(980.0, 11.0);
    CHECK(stdComm_WSRelayPlayout_Add(&po, &s) == 0, "an older one dropped");
    s = sample(1020.0, 12.0);
    CHECK(stdComm_WSRelayPlayout_Add(&po, &s) == 1 && po.count == 2, "a newer one taken");

    for (int i = 0; i < 100; i++)
    {
        s = sample(1040.0 + i * 20.0, 13.0 + i);
        stdComm_WSRelayPlayout_Add(&po, &s);
    }
    CHECK(po.count == WSRELAYPLAYOUT_SAMPLES, "keeps the last %d, has %d", WSRELAYPLAYOUT_SAMPLES, po.count);
    CHECK(stdComm_WSRelayPlayout_Newest(&po)->t == 1040.0 + 99 * 20.0, "newest kept");

    // The sender's clock jumps: the buffer starts over.
    s = sample(1040.0 + 99 * 20.0 + 5000.0, 200.0);
    CHECK(stdComm_WSRelayPlayout_Add(&po, &s) == 1 && po.count == 1 && po.restarts == 1, "5 s ahead starts over (%d, %u)", po.count, po.restarts);
    s = sample(10.0, 201.0);
    CHECK(stdComm_WSRelayPlayout_Add(&po, &s) == 1 && po.count == 1 && po.restarts == 2, "far behind starts over (%d, %u)", po.count, po.restarts);

    s = sample(NAN, 0.0);
    CHECK(stdComm_WSRelayPlayout_Add(&po, &s) == 0, "not a time: dropped");
}

static void test_at(void)
{
    WSRelayPlayoutPose pose;
    stdComm_WSRelayPlayout_Clear(&po);
    CHECK(stdComm_WSRelayPlayout_At(&po, 0.0, &pose) == 0, "no updates, no pose");

    WSRelayPlayoutSample a = sample(1000.0, 0.0), b = sample(1020.0, 0.0), c = sample(1040.0, 0.0);
    a.angles = vec(10.0f, 350.0f, 0.0f);
    b.angles = vec(20.0f, 10.0f, 0.0f);
    a.headPitch = 0.0f;
    b.headPitch = 10.0f;
    b.sector = 8;
    a.attachFlags = 0;
    b.attachFlags = 1;
    stdComm_WSRelayPlayout_Add(&po, &a);
    stdComm_WSRelayPlayout_Add(&po, &b);

    stdComm_WSRelayPlayout_At(&po, 1010.0, &pose);
    CHECK(near(pose.pos.x, xAt(1010.0), 1e-5) && pose.carried == 0, "halfway: x %f", pose.pos.x);
    CHECK(near(pose.angles.y, 0.0, 1e-4) || near(pose.angles.y, 360.0, 1e-4), "yaw the shorter way round: %f", pose.angles.y);
    CHECK(near(pose.angles.x, 15.0, 1e-4) && near(pose.headPitch, 5.0, 1e-4), "pitch %f, head %f", pose.angles.x, pose.headPitch);
    CHECK(pose.sector == 7 && pose.sectorNext == 8 && pose.attachFlags == 0, "sector and flags from the one before");
    CHECK(pose.tBefore == 1000.0 && pose.tAfter == 1020.0, "around it: %f %f", pose.tBefore, pose.tAfter);

    stdComm_WSRelayPlayout_At(&po, 1020.0, &pose);
    CHECK(near(pose.pos.x, xAt(1020.0), 1e-5) && pose.attachFlags == 1 && pose.carried == 0, "at the newest: as it is");

    stdComm_WSRelayPlayout_At(&po, 990.0, &pose);
    CHECK(near(pose.pos.x, xAt(1000.0), 1e-5) && pose.carried == 0, "before the oldest: there");

    stdComm_WSRelayPlayout_At(&po, 1100.0, &pose);
    CHECK(near(pose.pos.x, xAt(1100.0), 1e-4) && pose.carried == 1 && pose.tAfter < 0.0, "80 ms past the newest: carried on, x %f", pose.pos.x);
    stdComm_WSRelayPlayout_At(&po, 1500.0, &pose);
    CHECK(near(pose.pos.x, xAt(1020.0 + WSRELAYPLAYOUT_CARRY_MS), 1e-4) && pose.carried == 2 && pose.vel.x == 0.0f,
          "long past it: held where the carry stopped, x %f", pose.pos.x);

    // A jump between two updates: never drawn partway.
    c.warp = 1;
    c.pos = vec(100.0f, 100.0f, 0.0f);
    stdComm_WSRelayPlayout_Add(&po, &c);
    stdComm_WSRelayPlayout_At(&po, 1039.0, &pose);
    CHECK(near(pose.pos.x, xAt(1020.0), 1e-5) && pose.warp == 0, "before a jump: where the one before was, x %f", pose.pos.x);
    stdComm_WSRelayPlayout_At(&po, 1040.0, &pose);
    CHECK(near(pose.pos.x, 100.0, 1e-5) && pose.warp == 1, "at it: where it went");
}

static void test_steady(void)
{
    // 50 updates a second, 40 ms every time.
    stdComm_WSRelayPlayout_Clear(&po);
    Run run = drive(CLOCK_AHEAD, 20.0, trip40, 0.0, 10000.0);
    CHECK(run.carried == 0 && run.held == 0, "steady link: never ran out (%d carried, %d held)", run.carried, run.held);
    CHECK(run.backwards == 0, "never back in time");
    CHECK(near(run.lastDelay, 60.0, 1.0), "delay settles at trip + gap: %f", run.lastDelay);
    CHECK(near(stdComm_WSRelayPlayout_Buffer(&po), 20.0, 1.0), "buffer: the gap, %f", stdComm_WSRelayPlayout_Buffer(&po));
    CHECK(run.maxJump < 1e-3, "smooth: largest jump %f", run.maxJump);
}

static void test_jitter(void)
{
    // 40 to 80 ms: the delay covers nearly all of it.
    rng = 12345;
    stdComm_WSRelayPlayout_Clear(&po);
    Run run = drive(CLOCK_AHEAD, 20.0, trip40to80, 0.0, 20000.0);
    double carried = (double)run.carried / run.poses;
    CHECK(carried < 0.02, "jittery link: carried on %.2f%% of the time", carried * 100.0);
    CHECK(run.held == 0, "never held still (%d)", run.held);
    CHECK(run.backwards == 0, "never back in time");
    // An update that comes after the one sent after it is dropped, which
    // makes some gaps 40 ms: the delay allows for those too.
    CHECK(run.lastDelay > 95.0 && run.lastDelay < 125.0, "delay: trip + jitter + gap, %f", run.lastDelay);
    CHECK(run.maxJump < 0.01, "smooth: largest jump %f", run.maxJump);
}

static void test_late_one(void)
{
    // One update in 20 is 400 ms late: dropped when it comes, and the rest
    // carry the player on.
    stdComm_WSRelayPlayout_Clear(&po);
    Run run = drive(CLOCK_AHEAD, 20.0, tripLateOne, 0.0, 10000.0);
    CHECK(run.held == 0, "never held still (%d)", run.held);
    CHECK(run.lastDelay < 70.0, "the late ones don't hold the rest back: delay %f", run.lastDelay);
    CHECK(run.maxJump < 1e-3, "smooth: largest jump %f", run.maxJump);
}

static void test_routes(void)
{
    // A quick link, then a slow one: the player is held still while the
    // delay grows, never sent back in time, and then plays on.
    stdComm_WSRelayPlayout_Clear(&po);
    Run run = drive(CLOCK_AHEAD, 20.0, trip40, 0.0, 5000.0);
    double r = run.lastR;
    run = drive(CLOCK_AHEAD + 5000.0, 20.0, trip300, 5000.0, 15000.0);
    CHECK(run.backwards == 0, "slower route: never back in time");
    CHECK(near(run.lastDelay, 320.0, 1.0), "slower route: delay %f", run.lastDelay);
    CHECK(run.lastR > r, "plays on");
    CHECK((double)run.carried / run.poses < 0.1, "slower route: carried on %.1f%% of the time", 100.0 * run.carried / run.poses);

    // Back to the quick one: once most of the last 64 updates have come the
    // quick way, the delay jumps down.
    run = drive(CLOCK_AHEAD + 15000.0, 20.0, trip40, 15000.0, 17000.0);
    CHECK(near(run.lastDelay, 60.0, 1.0), "quicker route: delay down to %f", run.lastDelay);
    CHECK(run.held == 0, "quicker route: never held still (%d)", run.held);

    // A little slower: the delay eases up, unseen.
    stdComm_WSRelayPlayout_Clear(&po);
    drive(CLOCK_AHEAD, 20.0, trip40, 0.0, 5000.0);
    run = drive(CLOCK_AHEAD + 5000.0, 20.0, trip100, 5000.0, 5100.0);
    CHECK(run.maxDelay < 70.0, "60 ms slower: eases up, %f after 100 ms", run.maxDelay);
    run = drive(CLOCK_AHEAD + 5100.0, 20.0, trip100, 5100.0, 10000.0);
    CHECK(near(run.lastDelay, 120.0, 1.0), "and settles, %f", run.lastDelay);
    CHECK(run.backwards == 0, "never back in time");
}

static void test_stalls(void)
{
    // This game stalls for 400 ms: updates got here on time all the same
    // (the browser notes when), so the delay doesn't grow.
    stdComm_WSRelayPlayout_Clear(&po);
    Run run = drive(CLOCK_AHEAD, 20.0, trip40, 0.0, 5000.0);
    double before = run.lastDelay;
    for (int i = 0; i < 20; i++)
    {
        WSRelayPlayoutSample s = sample(CLOCK_AHEAD + 5000.0 + i * 20.0, 5040.0 + i * 20.0);
        stdComm_WSRelayPlayout_Add(&po, &s);
    }
    WSRelayPlayoutPose pose;
    stdComm_WSRelayPlayout_Pose(&po, 5400.0, &pose);
    CHECK(near(po.delay + CLOCK_AHEAD, before, 1e-6) && pose.carried == 0, "after this game stalls: delay %f, carried %d",
          po.delay + CLOCK_AHEAD, pose.carried);

    // The sender stalls for a second: it's held still, and the gap doesn't
    // make the delay grow.
    stdComm_WSRelayPlayout_Clear(&po);
    drive(CLOCK_AHEAD, 20.0, trip40, 0.0, 5000.0);
    for (int i = 0; i < 200; i++)
    {
        WSRelayPlayoutSample s = sample(CLOCK_AHEAD + 6000.0 + i * 20.0, 6040.0 + i * 20.0);
        stdComm_WSRelayPlayout_Add(&po, &s);
        stdComm_WSRelayPlayout_Pose(&po, 6040.0 + i * 20.0, &pose);
    }
    CHECK(near(po.delay + CLOCK_AHEAD, 60.0, 1.0), "after the sender stalls: delay %f", po.delay + CLOCK_AHEAD);
}

static void test_restart_counts(void)
{
    stdComm_WSRelayPlayout_Clear(&po);
    drive(CLOCK_AHEAD, 20.0, trip40, 0.0, 1000.0);
    uint32_t poses = po.poses;
    WSRelayPlayoutSample s = sample(CLOCK_AHEAD + 10000.0, 1040.0);
    stdComm_WSRelayPlayout_Add(&po, &s);
    CHECK(po.poses == poses && po.restarts == 1, "counts kept across a restart");
    WSRelayPlayoutPose pose;
    CHECK(stdComm_WSRelayPlayout_Pose(&po, 1050.0, &pose) == 1 && near(pose.pos.x, xAt(CLOCK_AHEAD + 10000.0), 1e-3),
          "draws from the new start");
}

// Swing and animation messages wait for the moment drawn to reach the time
// they were sent with, in order, and no longer than 300 ms.
static void test_holds(void)
{
    WSRelayPlayoutHolds h;
    memset(&h, 0, sizeof(h));
    WSRelayPlayoutHeld out;
    CHECK(!stdComm_WSRelayPlayout_MustHold(&h, 1000.0, 1000.0), "due at once: plays");
    CHECK(stdComm_WSRelayPlayout_MustHold(&h, 1000.5, 1000.0), "ahead of the moment: waits");
    CHECK(stdComm_WSRelayPlayout_MustHold(&h, NAN, 1000.0), "no time: waits (the caller doesn't hold those)");

    uint8_t a[3] = { 1, 2, 3 }, b[2] = { 4, 5 }, c[1] = { 6 };
    CHECK(stdComm_WSRelayPlayout_Hold(&h, 1050.0, 5000.0, a, 3), "held");
    CHECK(stdComm_WSRelayPlayout_MustHold(&h, 900.0, 1000.0), "one behind a waiting one waits too, to keep the order");
    CHECK(stdComm_WSRelayPlayout_Hold(&h, 1020.0, 5010.0, b, 2), "held");
    CHECK(stdComm_WSRelayPlayout_Hold(&h, 1100.0, 5020.0, c, 1), "held");

    CHECK(!stdComm_WSRelayPlayout_TakeHeld(&h, 1049.0, 5100.0, &out), "the first isn't due: none is");
    CHECK(stdComm_WSRelayPlayout_TakeHeld(&h, 1050.0, 5100.0, &out) && out.len == 3 && out.data[2] == 3 && near(out.t, 1050.0, 0.0),
          "the first, when the moment reaches it");
    CHECK(stdComm_WSRelayPlayout_TakeHeld(&h, 1050.0, 5100.0, &out) && out.len == 2 && out.data[1] == 5, "then the next, already due");
    CHECK(!stdComm_WSRelayPlayout_TakeHeld(&h, 1099.0, 5319.0, &out), "the last isn't due, and hasn't waited 300 ms");
    CHECK(stdComm_WSRelayPlayout_TakeHeld(&h, 1099.0, 5320.0, &out) && out.data[0] == 6, "300 ms is the most it waits");
    CHECK(h.count == 0 && !stdComm_WSRelayPlayout_TakeHeld(&h, 1e9, 1e9, &out), "none left");

    uint8_t big[WSRELAYPLAYOUT_HOLD_BYTES + 1];
    memset(big, 0, sizeof(big));
    CHECK(!stdComm_WSRelayPlayout_Hold(&h, 2000.0, 6000.0, big, sizeof(big)), "too long to keep");
    for (int i = 0; i < WSRELAYPLAYOUT_HOLDS; i++)
        stdComm_WSRelayPlayout_Hold(&h, 2000.0 + i, 6000.0, c, 1);
    CHECK(!stdComm_WSRelayPlayout_Hold(&h, 3000.0, 6000.0, c, 1), "full");
    CHECK(stdComm_WSRelayPlayout_TakeHeld(&h, -1e9, INFINITY, &out) && near(out.t, 2000.0, 0.0), "the oldest, played to make room");
    CHECK(stdComm_WSRelayPlayout_Hold(&h, 3000.0, 6000.0, c, 1), "then it fits");

    // One that waits only to keep the order.
    memset(&h, 0, sizeof(h));
    CHECK(!stdComm_WSRelayPlayout_MustHold(&h, -INFINITY, 1000.0), "behind nothing: plays");
    stdComm_WSRelayPlayout_Hold(&h, 1050.0, 7000.0, a, 3);
    CHECK(stdComm_WSRelayPlayout_MustHold(&h, -INFINITY, 1000.0), "behind one that waits: waits");
    stdComm_WSRelayPlayout_Hold(&h, -INFINITY, 7000.0, b, 2);
    CHECK(!stdComm_WSRelayPlayout_TakeHeld(&h, 1000.0, 7010.0, &out), "not before the one ahead of it");
    CHECK(stdComm_WSRelayPlayout_TakeHeld(&h, 1050.0, 7010.0, &out) && out.len == 3, "the one ahead");
    CHECK(stdComm_WSRelayPlayout_TakeHeld(&h, 1050.0, 7010.0, &out) && out.len == 2 && h.count == 0, "then it, at once");
}

int main(void)
{
    test_unwrap();
    test_since();
    test_add();
    test_at();
    test_steady();
    test_jitter();
    test_late_one();
    test_routes();
    test_stalls();
    test_restart_counts();
    test_holds();

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
