#ifndef _JKGUIHD_H
#define _JKGUIHD_H

#include "types.h"

// Added: HD menu pictures (Setup > Display > Advanced > HD Menus and Backgrounds), the browser
// build only. With the box ticked, the menu backgrounds the game draws at 640x480 are drawn from
// sharper pictures the game fetches from beside the page (index.html's HD_MENU_PICTURES), in
// the 4:3 set or, on a wider window, the wide set, which reaches past the 4:3 menu to the
// window's sides (up to 2.39:1). Only Jedi Knight's own art is covered: a background that a mod
// (or Mysteries of the Sith) changed keeps its own picture. See jkGUIHd.c.

#define JKGUIHD_NUM_PICTURES (11)

// The box's state: the player's choice, kept with the game's settings (wuRegistry, "bHdMenus").
int jkGuiHd_IsEnabled(void);
void jkGuiHd_SetEnabled(int bEnabled);
// Whether the box is offered: in the browser build, in Jedi Knight, with the page's pictures.
int jkGuiHd_IsOffered(void);

// For the menu present (std3D_DrawMenu): which HD picture the 640x480 part of the menu buffer
// is showing the background of, or -1 (the box is off, it's not a menu the set covers, or the
// background isn't Jedi Knight's own). *ppOrig is then that background's 640x480 8-bit pixels,
// a row every *pOrigPitch bytes, which the present compares pixel by pixel with the menu buffer
// (the menu's own drawing stays on top; where the menu buffer still shows the background, the
// picture shows instead). *pOrigGen (never 0) changes when those pixels might have.
int jkGuiHd_FindBackground(const uint8_t* pMenu, int menuPitch, int menuW, int menuH,
                           const uint8_t** ppOrig, int* pOrigPitch, uint32_t* pOrigGen);

// A picture's size in the 4:3 set or the wide set, and the part of it under the 640x480 menu
// (x, y, w, h in its pixels: all of it for a 4:3 one). 0 for no such picture.
int jkGuiHd_GetPictureInfo(int picture, int bWide, int* pW, int* pH, float aPart[4]);
const char* jkGuiHd_GetName(int picture);

#endif // _JKGUIHD_H
