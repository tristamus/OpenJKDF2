#ifndef _JKGUI_LIBRARY_H
#define _JKGUI_LIBRARY_H

#include "types.h"

// Added: the Level Library, the community levels the site keeps a copy of,
// as a game menu over the Host screen (browser build only; the page's half is
// packaging/wasm/library.js), and the Mod Library in the same menu.
#ifdef PLATFORM_WSRELAY

// Whether the site serves the library for this game.
int jkGuiLibrary_IsAvailable();

// Shows the library. Returns 1 with the episode it installed in pOutName
// (a GOB or GOO name without its extension), or 0 when the player left it
// without one.
int jkGuiLibrary_Show(int bMots, char* pOutName, int outLen);

// The same, from the Playlist screen (Gui/jkGUIPlaylist.c): Use Level is Add
// to Playlist.
int jkGuiLibrary_ShowToAdd(int bMots, char* pOutName, int outLen);

// Added: the Mod Library, the community mods the site keeps a copy of, from
// the main menu (Gui/jkGUIMain.c; the page's half is
// packaging/wasm/modlibrary.js). Whether the site serves it for the game
// that's running.
int jkGuiLibrary_ModsAvailable();

// Shows the Mod Library, over the main menu with the Host screen's menu
// mode and palette up. Returns 1 after Restart With It, with the mod folder
// to start the engine again in (pOutPath; empty for none, when the mod was a
// graphics pack), or 0 when the player left it without restarting (a mod
// they got is saved for Choose mods either way).
int jkGuiLibrary_ShowMods(int bMots, char* pOutPath, int outLen);

#endif // PLATFORM_WSRELAY

#endif // _JKGUI_LIBRARY_H
