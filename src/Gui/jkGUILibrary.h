#ifndef _JKGUI_LIBRARY_H
#define _JKGUI_LIBRARY_H

#include "types.h"

// Added: the Level Library, the community levels the site keeps a copy of,
// as a game menu over the Host screen (browser build only; the page's half is
// packaging/wasm/library.js).
#ifdef PLATFORM_WSRELAY

// Whether the site serves the library for this game.
int jkGuiLibrary_IsAvailable();

// Shows the library. Returns 1 with the episode it installed in pOutName
// (a GOB or GOO name without its extension), or 0 when the player left it
// without one.
int jkGuiLibrary_Show(int bMots, char* pOutName, int outLen);

#endif // PLATFORM_WSRELAY

#endif // _JKGUI_LIBRARY_H
