// Tests the milliseconds that each fixed physics step stands for
// (sithTime_StepMs in src/Gameplay/sithTime.c), with the frame loop of
// sithUpdate (src/Main/sithMain.c) rebuilt around it and made-up frames.
// Built and run by steptime-test.sh, which cuts the function out of the
// sources.
//
// A step is 1/150 s of game time, 6.67 ms. The timers that count down whole
// milliseconds (a thing's msecLifeLeft: an item's respawn, among others) used
// to get (int)6.67 = 6 ms a step, so a 60 s respawn took 66.7 s. Checked here,
// at refresh rates from 30 to 1000 Hz, steady and uneven, with 150 and 50
// steps a second:
// - a 60 s and a 26 s countdown end 60 s and 26 s after they start, within a
//   frame and a step;
// - over ten minutes the steps' milliseconds add up to the time the steps
//   stand for, and that to the time that passed;
// - the game clock the steps move never goes back;
// - the old 6 ms steps took 66.7 s for 60 s at 150 steps a second, which
//   shows the test sees the fault.

#include <math.h>
#include <stdint.h>
#include <stdio.h>

typedef double flex_d_t;

#define SITHTIME_MINDELTA (1)
#define SITHTIME_MAXDELTA (500)
#define SITHTIME_MINDELTA_US (1)
#define SITHTIME_MAXDELTA_US (500 * 1000)

static uint32_t sithTime_g_msecGameTime;

#include "steptime-code.inc"

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

static uint32_t rng = 12345;
static double frand(void)
{
    rng = rng * 1103515245u + 12345u;
    return (double)((rng >> 8) & 0xFFFFFF) / (double)0x1000000;
}

typedef struct
{
    double hz;          // frames a second
    double jitter;      // each frame's length varies by up to this share
    int hitchEvery;     // a 50 ms frame every so many frames, or 0
    double stepsPerSec; // TARGET_PHYSTICK_FPS
    double usTick;      // the microsecond clock's resolution, or 0 for exact
    int bOld;           // (int)(DELTA_PHYSTICK_FPS * 1000.0) ms a step
} Run;

typedef struct
{
    double secCountdown[2]; // how long each countdown took, in real seconds
    double secReal;         // how long the run was
    uint64_t msecSteps;     // the steps' milliseconds, all told
    uint64_t nSteps;        // and the steps
    int nBack;              // times the steps' game clock went back
    int maxFrameUs;         // the longest frame
    double usMinDelta;      // what SITHTIME_MINDELTA_US added to frames the clock saw as 0
} Result;

// A countdown like a thing's msecLifeLeft in sithThing_Update.
typedef struct
{
    double secStart;  // starts at the first step at or after this real time
    uint32_t msecLen;
    uint32_t msecLeft;
    int bStarted;
    double secDone;
} Countdown;

static Result run(Run r, double secLen)
{
    Result res = {0};
    Countdown cd[2] = {
        { 1.0, 60000, 0, 0, -1.0 }, // a picked-up 60 s item
        { 2.0, 26000, 0, 0, -1.0 }, // a force pulled item's 26 s
    };
    double stepSecs = 1.0 / r.stepsPerSec;
    double rollover = 0.0;
    double usNow = 5000.0;
    uint32_t msecClock = (uint32_t)(usNow / 1000.0);
    double usClock = usNow;
    uint32_t msecInLoopLast = 0;
    int nFrames = 0;

    sithTime_physicsRolloverMs = 0.0;
    sithTime_g_msecGameTime = 0;

    while (usNow < secLen * 1e6)
    {
        double usFrame = 1e6 / r.hz * (1.0 + r.jitter * (2.0 * frand() - 1.0));
        if (r.hitchEvery && nFrames && nFrames % r.hitchEvery == 0)
            usFrame = 50000.0;
        if (usFrame > res.maxFrameUs)
            res.maxFrameUs = (int)usFrame;
        usNow += usFrame;
        nFrames++;

        // sithTime_Advance and sithTime_SetFrameTime, whole milliseconds from
        // stdPlatform_GetTimeMsec and seconds from the microsecond clock.
        uint32_t msecNow = (uint32_t)(usNow / 1000.0);
        int frameTime = (int)(msecNow - msecClock);
        msecClock = msecNow;
        if (frameTime < SITHTIME_MINDELTA) frameTime = SITHTIME_MINDELTA;
        if (frameTime > SITHTIME_MAXDELTA) frameTime = SITHTIME_MAXDELTA;
        sithTime_g_msecGameTime += frameTime;
        double usRead = r.usTick ? floor(usNow / r.usTick) * r.usTick : usNow;
        double usDelta = usRead - usClock;
        usClock = usRead;
        if (usDelta < SITHTIME_MINDELTA_US)
        {
            res.usMinDelta += SITHTIME_MINDELTA_US - usDelta;
            usDelta = SITHTIME_MINDELTA_US;
        }
        if (usDelta > SITHTIME_MAXDELTA_US) usDelta = SITHTIME_MAXDELTA_US;
        double frameTimeFlex = usDelta * 0.001 * 0.001;

        // sithUpdate's fixed steps.
        double rolloverCombine = frameTimeFlex + rollover;
        double framesToApply = rolloverCombine * r.stepsPerSec;
        uint32_t wholeFramesToApply = (uint32_t)(float)round((float)framesToApply);
        rollover = rolloverCombine - ((double)wholeFramesToApply * stepSecs);

        uint32_t tmp6 = sithTime_g_msecGameTime;
        sithTime_g_msecGameTime -= frameTime;

        for (uint32_t i = 0; i < wholeFramesToApply; i++)
        {
            uint32_t msecStep = r.bOld ? (uint32_t)(int)(stepSecs * 1000.0) : sithTime_StepMs(stepSecs);

            if (sithTime_g_msecGameTime < msecInLoopLast)
                res.nBack++;
            msecInLoopLast = sithTime_g_msecGameTime;

            for (int c = 0; c < 2; c++)
            {
                Countdown* p = &cd[c];
                if (!p->bStarted && usNow >= p->secStart * 1e6)
                {
                    // Taken in this step: msecLifeLeft is set, and the step's
                    // sithThing_Update counts it down from the next one.
                    p->bStarted = 1;
                    p->msecLeft = p->msecLen;
                    p->secStart = usNow * 1e-6;
                    continue;
                }
                if (p->bStarted && p->msecLeft)
                {
                    if (p->msecLeft > msecStep)
                        p->msecLeft -= msecStep;
                    else
                    {
                        p->msecLeft = 0;
                        p->secDone = usNow * 1e-6;
                    }
                }
            }

            res.msecSteps += msecStep;
            res.nSteps++;
            sithTime_g_msecGameTime += msecStep;
            if (!r.bOld && sithTime_g_msecGameTime > tmp6)
                sithTime_g_msecGameTime = tmp6;
        }
        if (sithTime_g_msecGameTime > tmp6)
            res.nBack++;
        msecInLoopLast = sithTime_g_msecGameTime;
        sithTime_g_msecGameTime = tmp6;
    }

    for (int c = 0; c < 2; c++)
        res.secCountdown[c] = cd[c].secDone >= 0 ? cd[c].secDone - cd[c].secStart : -1.0;
    res.secReal = usNow * 1e-6;
    return res;
}

static void check_run(const char* name, Run r)
{
    Result res = run(r, 600.0);
    // A countdown ends at the step that takes its last millisecond, which
    // runs at most a frame (or the 500 ms most a frame counts for) and a step
    // after that moment, plus a millisecond for the clocks' rounding.
    double slack = res.maxFrameUs * 1e-6 + 1.0 / r.stepsPerSec + 0.002;
    double lens[2] = { 60.0, 26.0 };
    for (int c = 0; c < 2; c++)
    {
        double s = res.secCountdown[c];
        CHECK(s >= lens[c] - slack && s <= lens[c] + slack,
              "%s: %.0f s countdown took %.3f s (allowed %.3f to %.3f)", name, lens[c], s,
              lens[c] - slack, lens[c] + slack);
    }
    // The steps' milliseconds against the time the steps stand for, and that
    // against the time that passed (less the 5 ms the run started at, plus a
    // microsecond for each frame a 1 ms clock saw as taking none).
    double msecStood = (double)res.nSteps * 1000.0 / r.stepsPerSec;
    double off = (double)res.msecSteps - msecStood;
    CHECK(off <= 0.0 && off > -1.0, "%s: %llu steps counted %llu ms for %.1f ms", name,
          (unsigned long long)res.nSteps, (unsigned long long)res.msecSteps, msecStood);
    double msecReal = res.secReal * 1000.0 - 5.0 + res.usMinDelta / 1000.0;
    CHECK(fabs(msecStood - msecReal) <= 1000.0 / r.stepsPerSec + (r.usTick ? r.usTick / 1000.0 : 0.0) + 0.01,
          "%s: the steps stood for %.1f ms of %.1f ms", name, msecStood, msecReal);
    CHECK(res.nBack == 0, "%s: the steps' game clock went back %d times", name, res.nBack);
    printf("  %-34s 60 s took %7.3f s, 26 s took %7.3f s\n", name, res.secCountdown[0], res.secCountdown[1]);
}

int main(void)
{
    // Steps of 1/150 s: 6, 7, 7 ms, and 20 ms every three steps.
    {
        sithTime_physicsRolloverMs = 0.0;
        uint32_t a = sithTime_StepMs(1.0 / 150.0), b = sithTime_StepMs(1.0 / 150.0), c = sithTime_StepMs(1.0 / 150.0);
        CHECK(a + b + c == 20 && a >= 6 && a <= 7 && b >= 6 && b <= 7 && c >= 6 && c <= 7,
              "three steps of 1/150 s: %u %u %u ms", a, b, c);
        uint64_t sum = 0;
        for (int i = 0; i < 150 * 3600; i++)
            sum += sithTime_StepMs(1.0 / 150.0);
        CHECK(sum >= 3600000 - 1 && sum <= 3600000, "an hour of steps of 1/150 s counted %llu ms",
              (unsigned long long)sum);
        // Steps of 1/50 s are 20 ms each, as they were.
        sithTime_physicsRolloverMs = 0.0;
        int bAll20 = 1;
        for (int i = 0; i < 50 * 3600; i++)
            if (sithTime_StepMs(1.0 / 50.0) != 20 && i > 0)
                bAll20 = 0;
        CHECK(bAll20, "steps of 1/50 s weren't all 20 ms");
    }

    printf("Countdowns, 150 steps a second (a room's tick rate under 100 ms):\n");
    double rates[] = { 30, 60, 75, 120, 144, 165, 240, 360, 480, 1000 };
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
    {
        char name[64];
        Run r = { rates[i], 0.0, 0, 150.0, 0.0, 0 };
        snprintf(name, sizeof(name), "%.0f Hz steady", rates[i]);
        check_run(name, r);
        r.jitter = 0.4;
        r.hitchEvery = 997;
        snprintf(name, sizeof(name), "%.0f Hz uneven, with hitches", rates[i]);
        check_run(name, r);
        // A browser's clock that only tells time to 0.1 ms or to 1 ms.
        r.usTick = 100.0;
        snprintf(name, sizeof(name), "%.0f Hz uneven, 0.1 ms clock", rates[i]);
        check_run(name, r);
        r.usTick = 1000.0;
        snprintf(name, sizeof(name), "%.0f Hz uneven, 1 ms clock", rates[i]);
        check_run(name, r);
    }

    printf("Countdowns, 50 steps a second (a tick rate of 100 ms or more):\n");
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
    {
        char name[64];
        Run r = { rates[i], 0.3, 0, 50.0, 0.0, 0 };
        snprintf(name, sizeof(name), "%.0f Hz uneven", rates[i]);
        check_run(name, r);
    }

    // The old 6 ms steps, to show the test catches what they did.
    {
        Run r = { 144, 0.0, 0, 150.0, 0.0, 1 };
        Result res = run(r, 120.0);
        printf("Old 6 ms steps at 144 Hz: 60 s took %.3f s, 26 s took %.3f s\n", res.secCountdown[0],
               res.secCountdown[1]);
        CHECK(res.secCountdown[0] > 66.0 && res.secCountdown[0] < 67.5, "old steps: 60 s took %.3f s",
              res.secCountdown[0]);
    }

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
