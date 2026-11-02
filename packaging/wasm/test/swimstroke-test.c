// Tests for the swim stroke rule (src/World/sithSoundClassSwim.c), on its
// own: which sound classes' left surface stroke gets Kyle's stroke1.wav.
// From the top of the source tree:
//
//   cc -std=c11 -O2 -Wall -o swimstroke-test packaging/wasm/test/swimstroke-test.c
//      src/World/sithSoundClassSwim.c
//   ./swimstroke-test

#include "../../../src/World/sithSoundClassSwim.h"

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

// The lswimsurface and rswimsurface lines of each player sound class in
// Jedi Knight's Res2.gob (misc\snd), and which skins use it (misc\models.dat).
typedef struct RetailClass
{
    const char* pName;
    const char* pLeft;
    const char* pRight;
    int bSwap;
} RetailClass;

static const RetailClass aRetail[] = {
    { "ky.snd",         "stroke1.wav", "stroke2.wav", 0 },  // Katarn
    { "kyMP1.snd",      "stroke1.wav", "stroke2.wav", 0 },  // Red1Eye, Silver, Commando, Yellow
    { "kyMP2.snd",      "stroke1.wav", "stroke2.wav", 0 },  // DarkKyle, Angry, Green, Blue
    { "kyMP3.snd",      "stroke1.wav", "stroke2.wav", 0 },  // Trainer, Admiral, Officer, Purple, Orange
    { "kyMP4.snd",      "stroke1.wav", "stroke2.wav", 0 },  // Cyborg, Scarface, Red
    { "kyBobaFett.snd", "stroke1.wav", "stroke2.wav", 0 },  // BobaFett
    { "kyJerec.snd",    "stroke1.wav", "stroke2.wav", 0 },  // DarkRahn, Jerec
    { "kyTrooper.snd",  "stroke1.wav", "stroke2.wav", 0 },  // Trooper
    { "kyYun.snd",      "stroke1.wav", "stroke2.wav", 0 },  // Yun
    { "kybossk.snd",    "stroke2.wav", "stroke2.wav", 1 },  // Bossk
    { "kytusken.snd",   "stroke2.wav", "stroke2.wav", 1 },  // Tusken
    { "ky3po.snd",      "stroke2.wav", "stroke2.wav", 1 },  // C3PO
    { "kygreedo.snd",   "stroke2.wav", "stroke2.wav", 1 },  // Greedo
    { "kyfemale.snd",   "stroke2.wav", "stroke2.wav", 1 },  // Sariss, JanOrs
};

int main(void)
{
    // The retail classes: exactly the five with the same splash on both.
    for (size_t i = 0; i < sizeof(aRetail) / sizeof(aRetail[0]); i++)
    {
        const RetailClass* c = &aRetail[i];
        int got = sithSoundClassSwim_NeedsLeftStroke(c->pLeft, 1, c->pRight, 1);
        CHECK(got == c->bSwap, "%s: got %d, want %d", c->pName, got, c->bSwap);
    }

    // Any case, as a .snd file may spell it.
    CHECK(sithSoundClassSwim_NeedsLeftStroke("STROKE2.WAV", 1, "Stroke2.wav", 1), "upper case");

    // Only that pair: one of them something else, or the other way round.
    CHECK(!sithSoundClassSwim_NeedsLeftStroke("stroke2.wav", 1, "stroke1.wav", 1), "left stroke2, right stroke1");
    CHECK(!sithSoundClassSwim_NeedsLeftStroke("stroke1.wav", 1, "stroke1.wav", 1), "both stroke1");
    CHECK(!sithSoundClassSwim_NeedsLeftStroke("splash1.wav", 1, "splash1.wav", 1), "both another sound");
    CHECK(!sithSoundClassSwim_NeedsLeftStroke("stroke2.wav.bak", 1, "stroke2.wav", 1), "longer name");
    CHECK(!sithSoundClassSwim_NeedsLeftStroke("stroke2.wa", 1, "stroke2.wav", 1), "shorter name");
    CHECK(!sithSoundClassSwim_NeedsLeftStroke("sound\\stroke2.wav", 1, "stroke2.wav", 1), "with a folder");

    // A line missing or "none", or a line with more than one sound to pick
    // from: left as the author wrote it.
    CHECK(!sithSoundClassSwim_NeedsLeftStroke(NULL, 0, "stroke2.wav", 1), "no left line");
    CHECK(!sithSoundClassSwim_NeedsLeftStroke("stroke2.wav", 1, NULL, 0), "no right line");
    CHECK(!sithSoundClassSwim_NeedsLeftStroke("stroke2.wav", 2, "stroke2.wav", 1), "two left sounds");
    CHECK(!sithSoundClassSwim_NeedsLeftStroke("stroke2.wav", 1, "stroke2.wav", 3), "three right sounds");

    // What it swaps in.
    CHECK(strcmp(SITHSOUNDCLASSSWIM_LEFT, "stroke1.wav") == 0, "left stroke is %s", SITHSOUNDCLASSSWIM_LEFT);

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
