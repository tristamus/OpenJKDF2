#ifndef _JKGUI_PLAYLIST_H
#define _JKGUI_PLAYLIST_H

#include "types.h"

// Added: the Playlist screen, opened with Playlist... on the Host screen
// (browser build only). The host picks up to JKPLAYLIST_MAX levels of one
// kind from the episodes the Host screen lists (and the Level Library), puts
// them in order and says whether they loop; Main/jkPlaylist.c keeps the
// playlist in this browser and plays it.
#ifdef PLATFORM_WSRELAY

// Shows the screen, listing the episodes of the types in typeMask (as the
// Host screen lists them). Returns 1 when the player pressed Use Playlist
// (jkPlaylist_list is the new playlist, saved), 0 for Cancel.
int jkGuiPlaylist_Show(int bMots, int typeMask);

#endif // PLATFORM_WSRELAY

#endif // _JKGUI_PLAYLIST_H
