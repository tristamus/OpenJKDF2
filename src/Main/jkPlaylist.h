#ifndef _JKPLAYLIST_H
#define _JKPLAYLIST_H

#include "types.h"

// Added: the host's level playlist, for the browser build's games on the
// lobby's relay. The host picks up to JKPLAYLIST_MAX levels, from any
// episodes it has (the game's own, the Level Library's, its own GOBs), on
// the Playlist screen (src/Gui/jkGUIPlaylist.c), and hosts the playlist from
// the Host screen. When a level ends, the host's game names the next one in
// its end-of-level message (jkDSS_SendEndLevel), episode and all, and every
// game goes on to that one instead of working the next level out from its
// own copy of the episode. Players who don't have the next level's GOB start
// getting it then (packaging/wasm/maps.js, KIND_NEXT): from the site when
// it's a Level Library level, else from the host. The score screen says
// what's next and waits up to JKPLAYLIST_WAIT_MS for them.
#ifdef PLATFORM_WSRELAY

#define JKPLAYLIST_MAX (6)
#define JKPLAYLIST_NAME_LEN (32)
#define JKPLAYLIST_GOB_LEN (32)
#define JKPLAYLIST_JKL_LEN (64)
#define JKPLAYLIST_TITLE_LEN (48)

// The longest the host's score screen waits for players still getting the
// next level, from when it came up.
#define JKPLAYLIST_WAIT_MS (60000)

// A game's kind: a playlist has levels of one kind only (team scoring,
// flags and saber rules don't change between levels).
#define JKPLAYLIST_KIND_DM (0)
#define JKPLAYLIST_KIND_CTF (1)
#define JKPLAYLIST_KIND_SABER (2)
#define JKPLAYLIST_KIND_COOP (3)

typedef struct jkPlaylistEntry
{
    char gob[JKPLAYLIST_GOB_LEN];       // the episode's GOB (or GOO), without its extension
    char jkl[JKPLAYLIST_JKL_LEN];       // the level's file, as the Host screen gives it
    char16_t title[JKPLAYLIST_TITLE_LEN];
    int32_t type;                       // the episode's type flags (JK_EPISODE_*)
} jkPlaylistEntry;

typedef struct jkPlaylist
{
    char16_t name[JKPLAYLIST_NAME_LEN];
    int32_t count;
    int32_t bLoop;
    jkPlaylistEntry entries[JKPLAYLIST_MAX];
} jkPlaylist;

// The next level, as the host named it when this level ended.
typedef struct jkPlaylistNext
{
    int bNamed;     // named at this level's end, and not yet gone on to
    int bEnd;       // the playlist is over: the game ends after the score screen
    int idx;        // its place in the playlist, from 0
    int count;
    int bLoop;
    char gob[JKPLAYLIST_GOB_LEN];
    char jkl[JKPLAYLIST_JKL_LEN];
    char16_t title[JKPLAYLIST_TITLE_LEN];
} jkPlaylistNext;

extern jkPlaylist jkPlaylist_list;      // the host's playlist, as the Playlist screen keeps it
extern jkPlaylistNext jkPlaylist_next;

// The playlist this browser keeps for the game (JK or MotS).
void jkPlaylist_Load(int bMots);
void jkPlaylist_Save(int bMots);

int jkPlaylist_KindOf(int type);
const char16_t* jkPlaylist_KindName(int kind);

// Host screen: this game hosts the playlist from level idx, or (bOn 0) the
// episode's own order as before.
void jkPlaylist_SetHosting(int bOn, int idx);
int jkPlaylist_IsHosting(void);

// A game is hosted or joined: nothing is named yet.
void jkPlaylist_GameStarted(void);

// A level begins (every game): what was named at the last one's end is used.
void jkPlaylist_LevelStarted(void);

// The host, as its level ends: works the next level out (once a level) and
// gets it ready for players who don't have it. 1 if it named one.
int jkPlaylist_HostPick(void);

// A joiner, from the host's end-of-level message: the level it named, which
// this game starts getting if it doesn't have it.
void jkPlaylist_Named(const jkPlaylistNext* pNext);

// The score screen: whether it may close at msElapsed (bAsked: the player
// pressed Ok), and its two lines about what's next. 0 when nothing was named.
int jkPlaylist_TallyMayEnd(uint32_t msElapsed, uint32_t msDelay, int bAsked);
int jkPlaylist_TallyText(uint32_t msElapsed, uint32_t msDelay, char16_t* pLine1, char16_t* pLine2, int len);

// This game couldn't get the next level: why, for the error after the score
// screen ("" when it could).
const char16_t* jkPlaylist_FailedText(void);

#endif // PLATFORM_WSRELAY

#endif // _JKPLAYLIST_H
