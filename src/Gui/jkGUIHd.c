#include "Gui/jkGUIHd.h"

#include "General/stdBitmap.h"
#include "Gui/jkGUI.h"
#include "Main/Main.h"
#include "Platform/wuRegistry.h"
#include "stdPlatform.h"
#include "jk.h"

// Added: HD menu pictures (see jkGUIHd.h). The engine side: the player's choice, which of the
// game's menu backgrounds are Jedi Knight's own art, and which one the menu buffer is showing.
// The page (index.html) names the pictures (Module.jkHdMenus.pictures); the present
// (std3D_DrawMenu, std3D.c) fetches and draws them.

#if defined(ARCH_WASM) && defined(SDL2_RENDER)

#include <emscripten.h>
#include "SDL2_helper.h"

typedef struct jkGuiHdPictureInfo
{
    const char* name;
    int stdW, stdH;   // the 4:3 picture's size
    int wideW, wideH; // the wide one's
    float part[4];    // the part of the wide one under the 640x480 menu: x, y, w, h
} jkGuiHdPictureInfo;

// jkGuiHd_aPictureInfo and jkGuiHd_aThumbs, made by packaging/wasm/make-hd-menus.py
#include "Gui/jkGUIHd_data.h"

// The game's menu backgrounds (jkGui_stdBitmaps) and the picture each one gets. bkEsc.bm is
// loaded twice (the Esc menu's and the decision dialog's); bkDialog.bm, the dialogs' frame, has
// no picture and stays as it is, over whatever background is under it.
static const int jkGuiHd_aBitmapPicture[][2] = {
    { JKGUI_BM_BK_MAIN,        0 },
    { JKGUI_BM_BK_SINGLE,      1 },
    { JKGUI_BM_BK_MULTI,       2 },
    { JKGUI_BM_BK_SETUP,       3 },
    { JKGUI_BM_BK_ESC,         4 },
    { JKGUI_BM_BK_DECISION,    4 },
    { JKGUI_BM_BK_LOADING,     5 },
    { JKGUI_BM_BK_FIELD_LOG,   6 },
    { JKGUI_BM_BK_FORCE,       7 },
    { JKGUI_BM_BK_TALLY,       8 },
    { JKGUI_BM_BK_BUILD_MULTI, 9 },
    { JKGUI_BM_BK_BUILD_LOAD, 10 },
};
#define JKGUIHD_NUM_BITMAPS ((int)(sizeof(jkGuiHd_aBitmapPicture) / sizeof(jkGuiHd_aBitmapPicture[0])))

// How far (0..255, the mean of 576 colour values) a background may be from its HD picture's
// 16x12 average colours and still count as Jedi Knight's own. Measured 2026-11-22: Jedi Knight's
// own (Steam, Res1hi.gob) 0.5 to 13.3 (bkMulti, whose HD picture is a little brighter); the SBX
// mod's bkMain and bkMulti 47 and 53; Mysteries of the Sith's 20 to 55 (it's left out anyway).
#define JKGUIHD_MAX_THUMB_DIFF (16.0)

// A background counts as shown when at least this many of the sample points of the menu
// buffer still have its pixel: the menu's own drawing (text, buttons, a dialog's frame) covers
// the rest. Sampled every 16 pixels: 40x30 points.
#define JKGUIHD_SAMPLE_STEP (16)
#define JKGUIHD_MIN_SHOWN (0.5)

typedef struct jkGuiHdBitmapState
{
    const stdBitmap* pChecked; // the bitmap the check below was made on
    const void* pPixels;       // and its pixels then
    uint32_t sample;           // and a sum of some of them (jkGuiHd_Sample)
    uint32_t gen;              // jkGuiHd_gen when the check was made
    int bOwn;                  // it's Jedi Knight's own art
} jkGuiHdBitmapState;

static jkGuiHdBitmapState jkGuiHd_aState[JKGUIHD_NUM_BITMAPS];
static uint32_t jkGuiHd_gen = 0; // counts the checks made
static int jkGuiHd_bEnabled = -1; // -1: not asked yet
static int jkGuiHd_lastFound = -1; // the jkGuiHd_aBitmapPicture entry found last

// Whether the page names both sets of pictures (index.html's HD_MENU_PICTURES).
EM_JS(int, jkGuiHd_wasmOffered, (int n), {
  var hd = Module.jkHdMenus, p = hd && hd.pictures;
  return p && Array.isArray(p['43']) && p['43'].length === n && Array.isArray(p.wide) && p.wide.length === n ? 1 : 0;
});

// Turned off: the pictures' textures and files go (Module.jkHdTex, std3D.c).
EM_JS(void, jkGuiHd_wasmFree, (), {
  if (Module.jkHdTex && Module.jkHdTex.free) Module.jkHdTex.free();
});

int jkGuiHd_IsOffered(void)
{
    return !Main_bMotsCompat && !Main_bDroidWorks && jkGuiHd_wasmOffered(JKGUIHD_NUM_PICTURES);
}

int jkGuiHd_IsEnabled(void)
{
    if (jkGuiHd_bEnabled < 0)
    {
        jkGuiHd_bEnabled = jkGuiHd_IsOffered() && wuRegistry_GetBool("bHdMenus", 0);
        stdPlatform_Printf("OpenJKDF2: HD menu pictures %s\n", jkGuiHd_bEnabled ? "on" : "off");
    }
    return jkGuiHd_bEnabled;
}

void jkGuiHd_SetEnabled(int bEnabled)
{
    bEnabled = !!bEnabled && jkGuiHd_IsOffered();
    if (bEnabled == jkGuiHd_IsEnabled())
        return;
    jkGuiHd_bEnabled = bEnabled;
    jkGuiHd_lastFound = -1;
    wuRegistry_SaveBool("bHdMenus", bEnabled);
    stdPlatform_Printf("OpenJKDF2: HD menu pictures turned %s\n", bEnabled ? "on" : "off");
    if (!bEnabled)
        jkGuiHd_wasmFree();
}

int jkGuiHd_GetPictureInfo(int picture, int bWide, int* pW, int* pH, float aPart[4])
{
    if (picture < 0 || picture >= JKGUIHD_NUM_PICTURES)
        return 0;
    const jkGuiHdPictureInfo* pInfo = &jkGuiHd_aPictureInfo[picture];
    if (bWide)
    {
        *pW = pInfo->wideW;
        *pH = pInfo->wideH;
        for (int i = 0; i < 4; i++)
            aPart[i] = pInfo->part[i];
    }
    else
    {
        *pW = pInfo->stdW;
        *pH = pInfo->stdH;
        aPart[0] = 0.0f;
        aPart[1] = 0.0f;
        aPart[2] = (float)pInfo->stdW;
        aPart[3] = (float)pInfo->stdH;
    }
    return 1;
}

const char* jkGuiHd_GetName(int picture)
{
    if (picture < 0 || picture >= JKGUIHD_NUM_PICTURES)
        return "";
    return jkGuiHd_aPictureInfo[picture].name;
}

// The bitmap's 640x480 8-bit pixels, or NULL.
static const uint8_t* jkGuiHd_BitmapPixels(const stdBitmap* pBitmap, int* pPitch)
{
    if (!pBitmap || !pBitmap->mipSurfaces || pBitmap->numMips < 1)
        return NULL;
    const tVBuffer* pBuf = pBitmap->mipSurfaces[0];
    if (!pBuf || !pBuf->sdlSurface || !pBuf->sdlSurface->pixels)
        return NULL;
    if (pBuf->format.width != 640 || pBuf->format.height != 480 || pBuf->format.format.bpp != 8)
        return NULL;
    if ((int)pBuf->format.rowSize < 640)
        return NULL;
    *pPitch = (int)pBuf->format.rowSize;
    return (const uint8_t*)pBuf->sdlSurface->pixels;
}

// Whether the bitmap is Jedi Knight's own picture: its 16x12 average colours against the HD
// picture's (jkGUIHd_data.h).
static int jkGuiHd_IsOwnArt(const stdBitmap* pBitmap, int bitmapIdx, int picture)
{
    int pitch = 0;
    const uint8_t* pPixels = jkGuiHd_BitmapPixels(pBitmap, &pitch);
    if (!pPixels || !pBitmap->palette)
        return 0;

    const rdColor24* pPal = (const rdColor24*)pBitmap->palette;
    static uint32_t aSums[16*12*3];
    memset(aSums, 0, sizeof(aSums));
    for (int y = 0; y < 480; y++)
    {
        const uint8_t* pRow = pPixels + y * pitch;
        uint32_t* pBlockRow = &aSums[(y / 40) * 16 * 3];
        for (int x = 0; x < 640; x++)
        {
            const rdColor24* c = &pPal[pRow[x]];
            uint32_t* pBlock = &pBlockRow[(x / 40) * 3];
            pBlock[0] += c->r;
            pBlock[1] += c->g;
            pBlock[2] += c->b;
        }
    }
    double diff = 0.0;
    for (int i = 0; i < 16*12*3; i++)
    {
        double d = (double)aSums[i] / 1600.0 - (double)jkGuiHd_aThumbs[picture][i];
        diff += d < 0 ? -d : d;
    }
    diff /= 16*12*3;
    int bOwn = diff <= JKGUIHD_MAX_THUMB_DIFF;
    stdPlatform_Printf("OpenJKDF2: HD menu pictures: menu background %d is %.1f from %s's picture: %s\n",
                       bitmapIdx, diff, jkGuiHd_aPictureInfo[picture].name,
                       bOwn ? "Jedi Knight's own, covered" : "not Jedi Knight's own, left as it is");
    return bOwn;
}

// A sum of every 16th pixel of every 8th row (2400 of them, with their places), so that
// pixels loaded again at the same address are seen.
static uint32_t jkGuiHd_Sample(const uint8_t* pPixels, int pitch)
{
    uint32_t sum = 0;
    for (int y = 3; y < 480; y += 8)
        for (int x = 5; x < 640; x += 16)
            sum = sum * 31u + pPixels[y * pitch + x];
    return sum;
}

// The entry's bitmap and pixels if it's Jedi Knight's own art, checked again whenever the
// bitmap or its pixels change.
static const uint8_t* jkGuiHd_OwnPixels(int entry, int* pPitch, uint32_t* pGen)
{
    const stdBitmap* pBitmap = jkGui_stdBitmaps[jkGuiHd_aBitmapPicture[entry][0]];
    const uint8_t* pPixels = jkGuiHd_BitmapPixels(pBitmap, pPitch);
    if (!pPixels)
        return NULL;
    jkGuiHdBitmapState* pState = &jkGuiHd_aState[entry];
    uint32_t sample = jkGuiHd_Sample(pPixels, *pPitch);
    if (pState->pChecked != pBitmap || pState->pPixels != pPixels || pState->sample != sample || !pState->gen)
    {
        pState->pChecked = pBitmap;
        pState->pPixels = pPixels;
        pState->sample = sample;
        pState->gen = ++jkGuiHd_gen ? jkGuiHd_gen : ++jkGuiHd_gen;
        pState->bOwn = jkGuiHd_IsOwnArt(pBitmap, jkGuiHd_aBitmapPicture[entry][0], jkGuiHd_aBitmapPicture[entry][1]);
    }
    *pGen = pState->gen;
    return pState->bOwn ? pPixels : NULL;
}

// How many of the sample points of the menu buffer have the background's pixel.
static int jkGuiHd_CountShown(const uint8_t* pMenu, int menuPitch, const uint8_t* pOrig, int origPitch)
{
    int n = 0;
    for (int y = JKGUIHD_SAMPLE_STEP / 2; y < 480; y += JKGUIHD_SAMPLE_STEP)
    {
        const uint8_t* a = pMenu + y * menuPitch;
        const uint8_t* b = pOrig + y * origPitch;
        for (int x = JKGUIHD_SAMPLE_STEP / 2; x < 640; x += JKGUIHD_SAMPLE_STEP)
            n += a[x] == b[x];
    }
    return n;
}

int jkGuiHd_FindBackground(const uint8_t* pMenu, int menuPitch, int menuW, int menuH,
                           const uint8_t** ppOrig, int* pOrigPitch, uint32_t* pOrigGen)
{
    if (!pMenu || menuW < 640 || menuH < 480 || menuPitch < 640 || !jkGuiHd_IsEnabled())
        return -1;

    const int total = (640 / JKGUIHD_SAMPLE_STEP) * (480 / JKGUIHD_SAMPLE_STEP);
    const int need = (int)(total * JKGUIHD_MIN_SHOWN);

    // The one found last, then the rest: the best one with enough of its pixels showing
    int best = -1, bestCount = need - 1, bestPitch = 0;
    uint32_t bestGen = 0;
    const uint8_t* pBest = NULL;
    for (int k = -1; k < JKGUIHD_NUM_BITMAPS; k++)
    {
        int entry = k < 0 ? jkGuiHd_lastFound : k;
        if (entry < 0 || (k >= 0 && entry == jkGuiHd_lastFound))
            continue;
        int pitch = 0;
        uint32_t gen = 0;
        const uint8_t* pOrig = jkGuiHd_OwnPixels(entry, &pitch, &gen);
        if (!pOrig)
            continue;
        int count = jkGuiHd_CountShown(pMenu, menuPitch, pOrig, pitch);
        if (count > bestCount)
        {
            best = entry;
            bestCount = count;
            pBest = pOrig;
            bestPitch = pitch;
            bestGen = gen;
            // Most of it still there: no other background is (they all differ by far more)
            if (count >= total * 3 / 4)
                break;
        }
    }
    jkGuiHd_lastFound = best;
    if (best < 0)
        return -1;
    *ppOrig = pBest;
    *pOrigPitch = bestPitch;
    *pOrigGen = bestGen;
    return jkGuiHd_aBitmapPicture[best][1];
}

#else // !(ARCH_WASM && SDL2_RENDER)

int jkGuiHd_IsEnabled(void) { return 0; }
void jkGuiHd_SetEnabled(int bEnabled) { (void)bEnabled; }
int jkGuiHd_IsOffered(void) { return 0; }
int jkGuiHd_FindBackground(const uint8_t* pMenu, int menuPitch, int menuW, int menuH,
                           const uint8_t** ppOrig, int* pOrigPitch, uint32_t* pOrigGen)
{
    return -1;
}
int jkGuiHd_GetPictureInfo(int picture, int bWide, int* pW, int* pH, float aPart[4]) { return 0; }
const char* jkGuiHd_GetName(int picture) { return ""; }

#endif // ARCH_WASM && SDL2_RENDER
