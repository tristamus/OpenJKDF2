#include "stdDisplay.h"

#include "stdPlatform.h"
#include "jk.h"
#include "Win95/Video.h"
#include "Win95/Window.h"
#include "General/stdColor.h"

void stdDisplay_SetGammaTable(int len, flex_d_t *table)
{
    stdDisplay_gammaTableLen = len;
    stdDisplay_paGammaTable = table;
}

uint8_t* stdDisplay_GetPalette()
{
    return (uint8_t*)stdDisplay_gammaPalette;
}

#ifndef SDL2_RENDER

#else
#include "SDL2_helper.h"
#include <assert.h>

// SDL3: indexed surfaces have no palette by default, and CreateSurfacePalette()
// replaces any existing palette rather than being idempotent -- reuse the
// existing one (created once at surface-creation time) on hot paths like blits.
static SDL_Palette* stdDisplay_GetOrCreateSurfacePalette(SDL_Surface* surface)
{
    SDL_Palette* palette = SDL_GetSurfacePalette(surface);
    if (!palette)
        palette = SDL_CreateSurfacePalette(surface);
    return palette;
}

uint32_t Video_menuTexId = 0;
uint32_t Video_overlayTexId = 0;

// Added: whether anything was drawn into the overlay buffer (automap, force-power
// target circles) this frame and last frame. It is empty on most frames, so
// jkGame_Update only clears it after a frame that drew into it, and
// std3D_DrawMapOverlay skips the upload and the full-screen quad when neither did.
int Video_bOverlayDrawn = 0;
int Video_bOverlayDrawnLast = 0;
rdColor24 stdDisplay_masterPalette[256];
int Video_bModeSet = 0;

int stdDisplay_Startup()
{
    stdDisplay_bStartup = 1;
    return 1;
}

int stdDisplay_FindClosestDevice(void* a)
{
    Video_dword_866D78 = 0;
    return 0;
}

int stdDisplay_Open(int a)
{
    stdDisplay_pCurDevice = &stdDisplay_aDisplayDevices[0];
    stdDisplay_bOpen = 1;
    return 1;
}

void stdDisplay_Close()
{
    stdDisplay_bOpen = 0;
}

int stdDisplay_FindClosestMode(render_pair *a1, struct StdVideoMode *render_surface, unsigned int numModes)
{
    Video_curMode = 0;
    stdDisplay_bPaged = 1;
    stdDisplay_bModeSet = 1;
    return 0;
}

int stdDisplay_SetMode(unsigned int modeIdx, const void *palette, int paged)
{
    uint32_t newW = Window_xSize;
    uint32_t newH = Window_ySize;

    //if (jkGame_isDDraw)
    {
        newW = (uint32_t)((flex_t)Window_xSize * ((480.0*2.0)/Window_ySize));
        newH = 480*2;
    }

    if (newW > Window_xSize)
    {
        newW = Window_xSize;
        newH = Window_ySize;
    }

    if (newW < 640)
        newW = 640;
    if (newH < 480)
        newH = 480;

    stdDisplay_pCurVideoMode = &Video_renderSurface[modeIdx];
    
    stdDisplay_pCurVideoMode->format.format.bpp = 8;
    stdDisplay_pCurVideoMode->format.rowWidth = newW;
    stdDisplay_pCurVideoMode->format.width = newW;
    stdDisplay_pCurVideoMode->format.height = newH;
    
    _memcpy(&Video_otherBuf.format, &stdDisplay_pCurVideoMode->format, sizeof(Video_otherBuf.format));
    _memcpy(&Video_menuBuffer.format, &stdDisplay_pCurVideoMode->format, sizeof(Video_menuBuffer.format));
    
    _memcpy(&Video_overlayMapBuffer.format, &stdDisplay_pCurVideoMode->format, sizeof(Video_overlayMapBuffer.format));
    

    if (Video_bModeSet)
    {
        glDeleteTextures(1, &Video_menuTexId);
        glDeleteTextures(1, &Video_overlayTexId);
        if (Video_otherBuf.sdlSurface)
            SDL_DestroySurface(Video_otherBuf.sdlSurface);
        if (Video_menuBuffer.sdlSurface)
            SDL_DestroySurface(Video_menuBuffer.sdlSurface);
        if (Video_overlayMapBuffer.sdlSurface)
            SDL_DestroySurface(Video_overlayMapBuffer.sdlSurface);

        Video_otherBuf.sdlSurface = 0;
        Video_menuBuffer.sdlSurface = 0;
        Video_overlayMapBuffer.sdlSurface = 0;
    }

    SDL_Surface* otherSurface = SDL_CreateSurface(newW, newH, SDL_GetPixelFormatForMasks(8, 0, 0, 0, 0));
    SDL_Surface* menuSurface = SDL_CreateSurface(newW, newH, SDL_GetPixelFormatForMasks(8, 0, 0, 0, 0));
    SDL_Surface* overlaySurface = SDL_CreateSurface(newW, newH, SDL_GetPixelFormatForMasks(8, 0, 0, 0, 0));

    if (palette)
    {
        memcpy(stdDisplay_gammaPalette, palette, 0x300);
        const rdColor24* pal24 = (const rdColor24*)palette;
        SDL_Color* tmp = (SDL_Color*)malloc(sizeof(SDL_Color) * 256);
        for (int i = 0; i < 256; i++)
        {
            tmp[i].r = pal24[i].r;
            tmp[i].g = pal24[i].g;
            tmp[i].b = pal24[i].b;
            tmp[i].a = 0xFF;
        }

        SDL_SetPaletteColors(stdDisplay_GetOrCreateSurfacePalette(otherSurface), tmp, 0, 256);
        SDL_SetPaletteColors(stdDisplay_GetOrCreateSurfacePalette(menuSurface), tmp, 0, 256);
        SDL_SetPaletteColors(stdDisplay_GetOrCreateSurfacePalette(overlaySurface), tmp, 0, 256);
        free(tmp);
    }
    
    //SDL_SetSurfacePalette(otherSurface, palette);
    //SDL_SetSurfacePalette(menuSurface, palette);
    
    Video_otherBuf.sdlSurface = otherSurface;
    Video_menuBuffer.sdlSurface = menuSurface;
    Video_overlayMapBuffer.sdlSurface = overlaySurface;

    // Added: treat the new overlay buffer as drawn into, so the next frame clears it
    Video_bOverlayDrawn = 1;

    Video_menuBuffer.format.rowSize = menuSurface->pitch;
    Video_otherBuf.format.rowSize = otherSurface->pitch;
    Video_overlayMapBuffer.format.rowSize = overlaySurface->pitch;
    
    Video_menuBuffer.format.rowWidth = menuSurface->pitch;
    Video_otherBuf.format.rowWidth = otherSurface->pitch;
    Video_overlayMapBuffer.format.rowWidth = overlaySurface->pitch;
    Video_menuBuffer.format.width = newW;
    Video_otherBuf.format.width = newW;
    Video_overlayMapBuffer.format.width = newW;
    Video_menuBuffer.format.height = newH;
    Video_otherBuf.format.height = newH;
    Video_overlayMapBuffer.format.height = newH;
    
    Video_menuBuffer.format.format.bpp = 8;
    Video_otherBuf.format.format.bpp = 8;
    Video_overlayMapBuffer.format.format.bpp = 8;
    
    glGenTextures(1, &Video_menuTexId);
    glBindTexture(GL_TEXTURE_2D, Video_menuTexId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, newW, newH, 0, GL_RED, GL_UNSIGNED_BYTE, Video_menuBuffer.sdlSurface->pixels);
    
    glGenTextures(1, &Video_overlayTexId);
    glBindTexture(GL_TEXTURE_2D, Video_overlayTexId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, newW, newH, 0, GL_RED, GL_UNSIGNED_BYTE, Video_overlayMapBuffer.sdlSurface->pixels);
    

    Video_bModeSet = 1;
    
    return 1;
}

int stdDisplay_ClearRect(tVBuffer *buf, int fillColor, rdRect *rect)
{
    return stdDisplay_VBufferFill(buf, fillColor, rect);
}



int stdDisplay_DDrawGdiSurfaceFlip()
{
    Window_SdlUpdate();
    return 1;
}

int stdDisplay_ddraw_waitforvblank()
{
    Window_SdlVblank();
    return 1;
}

int stdDisplay_SetMasterPalette(uint8_t* pal)
{
    rdColor24* pal24 = (rdColor24*)pal;
    
    memcpy(stdDisplay_masterPalette, pal24, sizeof(stdDisplay_masterPalette));
    
    return 1;
}

tVBuffer* stdDisplay_VBufferNew(tRasterInfo *fmt, int create_ddraw_surface, int gpu_mem, const void* palette)
{
    STD_ASSERTREL((fmt->format.bpp % 8) == 0); // Added
    tVBuffer* out = (tVBuffer*)STD_ALLOC(sizeof(tVBuffer));
    
    _memset(out, 0, sizeof(*out));
    
    _memcpy(&out->format, fmt, sizeof(out->format));
    
    // force 0 reads
    //out->format.width = 0;
    //out->format.rowSize = 0;
    //out->surface_lock_alloc = STD_ALLOC(size);
    
    //if (fmt->format.g_bits == 6) // RGB565
    {
        fmt->format.r_bits = 0;
        fmt->format.g_bits = 0;
        fmt->format.b_bits = 0;
        fmt->format.r_shift = 0;
        fmt->format.g_shift = 0;
        fmt->format.b_shift = 0;
    }

    uint32_t rbitmask = ((1 << fmt->format.r_bits) - 1) << fmt->format.r_shift;
    uint32_t gbitmask = ((1 << fmt->format.g_bits) - 1) << fmt->format.g_shift;
    uint32_t bbitmask = ((1 << fmt->format.b_bits) - 1) << fmt->format.b_shift;
    uint32_t abitmask = 0;//((1 << fmt->format.a_bits) - 1) << fmt->format.a_shift;
    if (fmt->format.bpp == 8)
    {
        rbitmask = 0;
        gbitmask = 0;
        bbitmask = 0;
        abitmask = 0;
    }

    SDL_Surface* surface = SDL_CreateSurface(fmt->width, fmt->height, SDL_GetPixelFormatForMasks(fmt->format.bpp, rbitmask, gbitmask, bbitmask, abitmask));
    
    if (surface)
    {
        static int num = 0;
        //printf("Allocated VBuffer %u, w %u h %u bpp %u %x %x %x\n", num++, fmt->width, fmt->height, fmt->format.bpp, rbitmask, gbitmask, bbitmask);
        out->format.rowSize = surface->pitch;
        out->format.rowWidth = fmt->width;
        out->format.size = surface->pitch * fmt->height;
    }
    else
    {
        //printf("asdf\n");
        stdPlatform_Printf("Failed to allocate VBuffer! %s, w %u h %u bpp %u, rmask %x gmask %x bmask %x amask %x, %x %x %x, %x %x %x\n", SDL_GetError(), fmt->width, fmt->height, fmt->format.bpp, rbitmask, gbitmask, bbitmask, abitmask, fmt->format.r_bits, fmt->format.g_bits, fmt->format.b_bits, fmt->format.r_shift, fmt->format.g_shift, fmt->format.b_shift);
        assert(0);
    }
    //printf("Failed to allocate VBuffer! %s, w %u h %u bpp %u, rmask %x gmask %x bmask %x amask %x, %x %x %x, %x %x %x\n", SDL_GetError(), fmt->width, fmt->height, fmt->format.bpp, rbitmask, gbitmask, bbitmask, abitmask, fmt->format.r_bits, fmt->format.g_bits, fmt->format.b_bits, fmt->format.r_shift, fmt->format.g_shift, fmt->format.b_shift);
    
    out->sdlSurface = surface;
    
    return out;
}

int stdDisplay_VBufferLock(tVBuffer *buf)
{
    if (!buf) return 0;

    SDL_LockSurface(buf->sdlSurface);
    buf->surface_lock_alloc = (char*)buf->sdlSurface->pixels;
    return 1;
}

void stdDisplay_VBufferUnlock(tVBuffer *buf)
{
    if (!buf) return;
    
    buf->surface_lock_alloc = NULL;
    SDL_UnlockSurface(buf->sdlSurface);
}

// Added: narrows [*pLo, *pHi) to the i for which 0 <= i + offset < limit. That is
// the test the old per-pixel loops made with unsigned compares, done once per rect.
static void stdDisplay_ClipSpan(int64_t* pLo, int64_t* pHi, int32_t offset, int32_t limit)
{
    int64_t lo = -(int64_t)offset;
    int64_t hi = (int64_t)(uint32_t)limit - offset;
    if (*pLo < lo) *pLo = lo;
    if (*pHi > hi) *pHi = hi;
}

int stdDisplay_VBufferCopy(tVBuffer *vbuf, tVBuffer *vbuf2, unsigned int blit_x, int blit_y, rdRect *rect, int alpha_maybe)
{
    if (!vbuf || !vbuf2) return 1;
    
    rdRect fallback = {0,0,vbuf2->format.width, vbuf2->format.height};
    if (!rect)
    {
        rect = &fallback;
        //memcpy(vbuf->sdlSurface->pixels, vbuf2->sdlSurface->pixels, 640*480);
        //return;
    }
    
    //if (vbuf == &Video_menuBuffer)
    //    stdPlatform_Printf("Vbuffer copy to menu %u,%u %ux%u %u,%u\n", rect->x, rect->y, rect->width, rect->height, blit_x, blit_y);
    
    if (vbuf->palette)
    {
        rdColor24* pal24 = (rdColor24*)vbuf->palette;
        SDL_Color* tmp = (SDL_Color*)malloc(sizeof(SDL_Color) * 256);
        for (int i = 0; i < 256; i++)
        {
            tmp[i].r = pal24[i].r;
            tmp[i].g = pal24[i].g;
            tmp[i].b = pal24[i].b;
            tmp[i].a = 0xFF;
        }
    
        SDL_SetPaletteColors(stdDisplay_GetOrCreateSurfacePalette(vbuf->sdlSurface), tmp, 0, 256);
        free(tmp);
    }

    if (vbuf2->palette)
    {
        rdColor24* pal24 = (rdColor24*)vbuf2->palette;
        SDL_Color* tmp = (SDL_Color*)malloc(sizeof(SDL_Color) * 256);
        for (int i = 0; i < 256; i++)
        {
            tmp[i].r = pal24[i].r;
            tmp[i].g = pal24[i].g;
            tmp[i].b = pal24[i].b;
            tmp[i].a = 0xFF;
        }
        
        SDL_SetPaletteColors(stdDisplay_GetOrCreateSurfacePalette(vbuf2->sdlSurface), tmp, 0, 256);
        free(tmp);
    }

    SDL_Rect dstRect = {(int)blit_x, (int)blit_y, (int)rect->width, (int)rect->height};
    SDL_Rect srcRect = {(int)rect->x, (int)rect->y, (int)rect->width, (int)rect->height};
    
    uint8_t* srcPixels = (uint8_t*)vbuf2->sdlSurface->pixels;
    uint8_t* dstPixels = (uint8_t*)vbuf->sdlSurface->pixels;
    uint32_t srcStride = vbuf2->format.rowSize;
    uint32_t dstStride = vbuf->format.rowSize;

    int self_copy = 0;

    if (dstPixels == srcPixels)
    {
        size_t buf_len = srcStride * dstRect.w * dstRect.h;
        uint8_t* dstPixels = (uint8_t*)malloc(buf_len);
        int has_alpha = 0;//!(rect->width == 640);

        SDL_Rect dstRect_inter = {0, 0, rect->width, rect->height};

        // Added: rows outside, columns inside, so memory is read in order
        for (int j = 0; j < rect->height; j++)
        {
            for (int i = 0; i < rect->width; i++)
            {
                if ((uint32_t)(i + srcRect.x) > (uint32_t)vbuf2->format.width) continue;
                if ((uint32_t)(j + srcRect.y) > (uint32_t)vbuf2->format.height) continue;
                
                uint8_t pixel = srcPixels[(i + srcRect.x) + ((j + srcRect.y)*srcStride)];

                if (!pixel && has_alpha) continue;
                if ((uint32_t)(i + dstRect_inter.x) > (uint32_t)vbuf->format.width) continue;
                if ((uint32_t)(j + dstRect_inter.y) > (uint32_t)vbuf->format.height) continue;

                dstPixels[(i + dstRect_inter.x) + ((j + dstRect_inter.y)*srcStride)] = pixel;
            }
        }
        
        

        srcPixels = dstPixels;
        srcRect.x = 0;
        srcRect.y = 0;

        self_copy = 1;
    }
    
    int once = 0;
    int has_alpha = !(rect->width == 640) && (alpha_maybe & 1);

    // Added: copy row by row. This used to go column by column with four bounds
    // checks per pixel. Working out once which part of the rect lies inside both
    // buffers copies exactly the same pixels, and a row with no colour key is a
    // plain memcpy. With the key (has_alpha), pixels of colour 0 are still skipped.
    int64_t i0 = 0, i1 = rect->width;
    int64_t j0 = 0, j1 = rect->height;
    stdDisplay_ClipSpan(&i0, &i1, srcRect.x, vbuf2->format.width);
    stdDisplay_ClipSpan(&i0, &i1, dstRect.x, vbuf->format.width);
    stdDisplay_ClipSpan(&j0, &j1, srcRect.y, vbuf2->format.height);
    stdDisplay_ClipSpan(&j0, &j1, dstRect.y, vbuf->format.height);

    if (i0 < i1)
    {
        size_t rowLen = (size_t)(i1 - i0);
        for (int64_t j = j0; j < j1; j++)
        {
            const uint8_t* srcRow = srcPixels + (size_t)(i0 + srcRect.x) + (size_t)(j + srcRect.y) * srcStride;
            uint8_t* dstRow = dstPixels + (size_t)(i0 + dstRect.x) + (size_t)(j + dstRect.y) * dstStride;

            if (!has_alpha)
            {
                memcpy(dstRow, srcRow, rowLen);
                continue;
            }

            for (size_t k = 0; k < rowLen; k++)
            {
                uint8_t pixel = srcRow[k];
                if (pixel)
                    dstRow[k] = pixel;
            }
        }
    }

    if (self_copy)
    {
        free(srcPixels);
    }
    
    //SDL_BlitSurface(vbuf2->sdlSurface, &srcRect, vbuf->sdlSurface, &dstRect); //TODO error check
    return 1;
}

int stdDisplay_VBufferFill(tVBuffer *vbuf, int fillColor, rdRect *rect)
{
    STD_ASSERTREL(vbuf != NULL); // Added
    rdRect fallback = {0,0,vbuf->format.width, vbuf->format.height};
    if (!rect)
    {
        rect = &fallback;
    }
    
    //if (vbuf == &Video_menuBuffer)
    //    stdPlatform_Printf("Vbuffer fill to menu %u,%u %ux%u\n", rect->x, rect->y, rect->width, rect->height);

    //printf("%x; %u %u %u %u\n", fillColor, rect->x, rect->y, rect->width, rect->height);

    // Added: the HUD and overlay buffers are cleared with this every frame, and the
    // old loop went column by column, one byte at a time, with a bounds check per
    // pixel: several ms per frame at 1080p. Now each row is one memset.
    // It writes the same bytes as before. The old loop wrote byte
    // (x + i) + (y + j) * stride for every pixel of the rect and skipped any outside
    // the buffer, so a rect sticking out on the left or right wraps into the row
    // before or after, and the parts above or below the buffer are dropped.
    // (It also wrote the one byte just past the end; that byte is not touched now.)
    uint8_t* dstPixels = (uint8_t*)vbuf->sdlSurface->pixels;
    int64_t stride = vbuf->format.rowSize;
    int64_t bufLen = stride * vbuf->format.height;
    int64_t w = rect->width;
    int64_t h = rect->height;
    if (w <= 0 || h <= 0 || bufLen <= 0)
        return 1;

    int64_t start = (int64_t)rect->x + (int64_t)rect->y * stride;
    if (w >= stride)
    {
        // Rows touch or overlap (a whole-buffer clear when the pitch is the width),
        // so the rect is one run of bytes
        int64_t end = start + (h - 1) * stride + w;
        if (start < 0) start = 0;
        if (end > bufLen) end = bufLen;
        if (start < end)
            memset(dstPixels + start, fillColor, (size_t)(end - start));
        return 1;
    }

    // Skip rows that end before the buffer starts
    int64_t j = 0;
    if (start + w <= 0)
        j = (-(start + w)) / stride + 1;
    for (; j < h; j++)
    {
        int64_t rowStart = start + j * stride;
        int64_t rowEnd = rowStart + w;
        if (rowStart >= bufLen)
            break;
        if (rowStart < 0) rowStart = 0;
        if (rowEnd > bufLen) rowEnd = bufLen;
        memset(dstPixels + rowStart, fillColor, (size_t)(rowEnd - rowStart));
    }

    //SDL_FillRect(vbuf, &dstRect, fillColor); //TODO error check

    return 1;
}

int stdDisplay_VBufferSetColorKey(tVBuffer *vbuf, int color)
{
    //DDCOLORKEY v3; // [esp+0h] [ebp-8h] BYREF

    if ( vbuf->lockRefCount )
    {
        /*if ( vbuf->lockRefCount == 1 )
        {
            v3.dwColorSpaceLowValue = color;
            v3.dwColorSpaceHighValue = color;
            vbuf->ddraw_surface->lpVtbl->SetColorKey(vbuf->ddraw_surface, 8, &v3);
            return 1;
        }*/
        vbuf->transparent_color = color;
    }
    else
    {
        vbuf->transparent_color = color;
    }
    return 1;
}

void stdDisplay_VBufferFree(tVBuffer *vbuf)
{
    // Added: Safety fallbacks
    if (!vbuf) {
        return;
    }
    stdDisplay_VBufferUnlock(vbuf);
    SDL_DestroySurface(vbuf->sdlSurface);
    STD_FREE(vbuf);
}

void stdDisplay_ddraw_surface_flip2()
{
}

void stdDisplay_RestoreDisplayMode()
{

}

tVBuffer* stdDisplay_VBufferConvertColorFormat(void* a, tVBuffer* b)
{
    return b;
}

// TODO: actually correct gamma
int stdDisplay_GammaCorrect3(int a1)
{
    (void)a1;
    _memcpy(stdDisplay_gammaPalette, stdDisplay_masterPalette, sizeof(stdDisplay_gammaPalette));
    return 1;
}

int stdDisplay_SetCooperativeLevel(uint32_t a){return 0;}
int stdDisplay_DrawAndFlipGdi(uint32_t a){return 0;}
void stdDisplay_422A50(){}
#endif

void stdDisplay_GammaCorrect(const void *pPal)
{
    _memcpy(stdDisplay_tmpGammaPal, pPal, sizeof(stdDisplay_tmpGammaPal));
    if ( stdDisplay_paGammaTable && stdDisplay_gammaIdx )
        stdColor_GammaCorrect((uint8_t *)stdDisplay_gammaPalette, (uint8_t *)stdDisplay_tmpGammaPal, 256, stdDisplay_paGammaTable[stdDisplay_gammaIdx - 1]);
    else
        _memcpy(stdDisplay_gammaPalette, pPal, sizeof(stdDisplay_gammaPalette));
}