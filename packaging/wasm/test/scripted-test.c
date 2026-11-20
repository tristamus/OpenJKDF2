// Tests for a level whose script keeps the score: what the engine tells the
// match record when a script sets a player's score, kills, deaths or suicides
// (src/Cog/sithCogFunctionPlayer.c), and what the match record then sends
// with the host's scoreboard (src/Platform/Networking/WSRelay/stdComm_WSRelay.c).
// A co-op level gives a player "kills" for the enemies they shoot, so the
// game's scoreboard is the level's own; the lobby is told, and shows the
// match by that scoreboard instead of by the kills between its players.
// scripted-test.sh cuts the code out of the sources and builds this file
// around it. From the top of the source tree:
//
//   sh packaging/wasm/test/scripted-test.sh

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>

// What the lobby is told of (STDCOMM_WSRELAY_SCRIPTED_*), and the mode flags.
#include "scripted-types.inc"

// ---- What the code reads of the engine.

typedef struct SithPlayer
{
    uint32_t flags;
    uint32_t playerNetId;
    int32_t numKills;
    int32_t numKilled;
    int32_t numSuicides;
    int32_t teamNum;
    int32_t score;
} SithPlayer;

#define SITH_THING_PLAYER (10)
typedef struct SithThing
{
    int type;
    struct { SithPlayer* pPlayer; } actorParams;
} SithThing;

typedef struct sithCog { int unused; } sithCog;
typedef struct sithWorld { char map_jkl_fname[32]; } sithWorld;

static int sithNet_isMulti = 1;
static int sithNet_isServer = 1;
static int sithNet_MultiModeFlags = MULTIMODEFLAG_SCORELIMIT;
static int sithNet_scorelimit = 20;
static int sithNet_multiplayer_timelimit = 0;
static int sithNet_teamScore[5];
static int stdComm_Imperial_bActive = 0;
// A game watching the match keeps no match record of its own: not here.
static int stdComm_WSRelay_bViewer = 0;
static int jkGuiNetHost_bIsDedicated = 0;
static const char* openjkdf2_aReleaseCommitShort = "abc1234";
static sithWorld world = { "thief.jkl" };
static sithWorld* sithWorld_g_pCurrentWorld = &world;

#define NUM_PLAYERS (3)
static SithPlayer jkPlayer_playerInfos[32];
static int jkPlayer_maxPlayers = NUM_PLAYERS;
static SithThing things[NUM_PLAYERS];

static int syncs = 0;
static void sithMulti_SyncScores(void) { syncs++; }

// A script's call: its arguments are popped last first.
static int popInt;
static SithThing* popThing;
static int sithCogExec_PopInt(sithCog* pCog) { return popInt; }
static SithThing* sithCogExec_PopThing(sithCog* pCog) { return popThing; }

static char16_t* wsrelay_matchTitle(char* pJklFname) { return NULL; }

// ---- The page (the EM_JS functions): what it was last given.

static int starts = 0, ends = 0, scores = 0;
static int lastMode = -1, lastScripted = -1, lastRows = -1, lastKills0 = -1;

static void wsrelay_js_match_start(const char* pMap, const char16_t* pTitle, int mode, int scoreLimit, int timeLimitMin,
                                   int bHost, int bIdle, const char* pBuild)
{
    starts++;
}

static void wsrelay_js_match_scores(const int32_t* pRows, int n, const int32_t* pTeams, int mode, int scripted)
{
    scores++;
    lastRows = n;
    lastKills0 = n ? pRows[2] : -1;
    lastMode = mode;
    lastScripted = scripted;
}

static void wsrelay_js_match_end(int bLeft) { ends++; }

// Not hosting a playlist: the level's place in none.
static int jkPlaylist_Position(int* pCount, int* pbLoop, const char16_t** ppNext) { return -1; }
static void wsrelay_js_match_playlist(int idx, int count, int bLoop, const char16_t* pNext) {}

// The tree's own code.
#include "scripted-code.inc"

// ---- The tests.

static int failed = 0;
#define CHECK(what, ok) do { if (!(ok)) { failed++; printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)

static sithCog cog;

static void set(void (*fn)(sithCog*), int player, int value)
{
    popInt = value;
    popThing = &things[player];
    fn(&cog);
}

static void newGame(int bMulti, int bServer)
{
    memset(jkPlayer_playerInfos, 0, sizeof(jkPlayer_playerInfos));
    for (int i = 0; i < NUM_PLAYERS; i++)
    {
        jkPlayer_playerInfos[i].flags = 1;
        jkPlayer_playerInfos[i].playerNetId = (uint32_t)(i + 1);
        things[i].type = SITH_THING_PLAYER;
        things[i].actorParams.pPlayer = &jkPlayer_playerInfos[i];
    }
    sithNet_isMulti = bMulti;
    sithNet_isServer = bServer;
    sithNet_MultiModeFlags = MULTIMODEFLAG_SCORELIMIT;
    stdComm_Imperial_bActive = 0;
    wsrelay_bMatchOn = 0;
    wsrelay_matchScripted = 0;
    starts = ends = scores = syncs = 0;
    lastMode = lastScripted = lastRows = lastKills0 = -1;
}

int main(void)
{
    // A deathmatch on the host: nothing is set by a script, and the
    // scoreboard goes with the mode and nothing to say.
    newGame(1, 1);
    stdComm_WSRelay_MatchStart();
    CHECK("the start is told, with the scoreboard", starts == 1 && scores == 1 && lastRows == NUM_PLAYERS);
    CHECK("a level that only counts its kills says nothing", lastScripted == 0 && lastMode == MULTIMODEFLAG_SCORELIMIT);

    // A script gives the first player two "kills" for an enemy.
    set(sithCogFunctionPlayer_SetPlayerKills, 0, 2);
    CHECK("the kills are set, and the scores are to be sent", jkPlayer_playerInfos[0].numKills == 2 && syncs == 1);
    stdComm_WSRelay_MatchScores();
    CHECK("the scoreboard says a script set kills", lastScripted == STDCOMM_WSRELAY_SCRIPTED_KILLS && lastKills0 == 2);

    // Setting what's there already isn't the script's doing.
    newGame(1, 1);
    stdComm_WSRelay_MatchStart();
    set(sithCogFunctionPlayer_SetPlayerKills, 0, 0);
    set(sithCogFunctionPlayer_SetPlayerScore, 1, 0);
    set(sithCogFunctionPlayer_SetPlayerKilled, 2, 0);
    set(sithCogFunctionPlayer_SetPlayerSuicides, 2, 0);
    stdComm_WSRelay_MatchScores();
    CHECK("a value set to itself says nothing", lastScripted == 0);

    // Each of the four says its own, and they add up.
    set(sithCogFunctionPlayer_SetPlayerScore, 0, 5);
    stdComm_WSRelay_MatchScores();
    CHECK("score", lastScripted == STDCOMM_WSRELAY_SCRIPTED_SCORE && jkPlayer_playerInfos[0].score == 5);
    set(sithCogFunctionPlayer_SetPlayerKilled, 1, 3);
    stdComm_WSRelay_MatchScores();
    CHECK("deaths", lastScripted == (STDCOMM_WSRELAY_SCRIPTED_SCORE | STDCOMM_WSRELAY_SCRIPTED_DEATHS) &&
                    jkPlayer_playerInfos[1].numKilled == 3);
    set(sithCogFunctionPlayer_SetPlayerSuicides, 2, 1);
    set(sithCogFunctionPlayer_SetPlayerKills, 2, 7);
    // And the mode as the script left it (scored by the level).
    sithNet_MultiModeFlags |= MULTIMODEFLAG_4;
    stdComm_WSRelay_MatchScores();
    CHECK("all four", lastScripted == 15 && jkPlayer_playerInfos[2].numSuicides == 1 && jkPlayer_playerInfos[2].numKills == 7);
    CHECK("with the mode as it is now", lastMode == (MULTIMODEFLAG_SCORELIMIT | MULTIMODEFLAG_4));
    // Setting a value back doesn't take it back: the script keeps the score.
    set(sithCogFunctionPlayer_SetPlayerKills, 2, 0);
    stdComm_WSRelay_MatchScores();
    CHECK("it stays said for the level", lastScripted == 15);

    // The level's end sends the last scoreboard with it, and the next level
    // starts with nothing set.
    scores = 0;
    stdComm_WSRelay_MatchEnd(0);
    CHECK("the end goes after a last scoreboard", ends == 1 && scores == 1 && lastScripted == 15);
    stdComm_WSRelay_MatchStart();
    CHECK("the next level says nothing", lastScripted == 0 && starts == 2);

    // A script that sets a score as the level opens, before its start is
    // told (a startup message): told with the level's first scoreboard.
    newGame(1, 1);
    set(sithCogFunctionPlayer_SetPlayerKills, 1, 10);
    stdComm_WSRelay_MatchStart();
    CHECK("set as the level opened", starts == 1 && lastScripted == STDCOMM_WSRELAY_SCRIPTED_KILLS);

    // A game that joined isn't the one whose scoreboard counts: a script
    // there sets nothing of the three the host keeps, and says nothing.
    newGame(1, 0);
    stdComm_WSRelay_MatchStart();
    set(sithCogFunctionPlayer_SetPlayerKills, 0, 4);
    set(sithCogFunctionPlayer_SetPlayerScore, 0, 4);
    set(sithCogFunctionPlayer_SetPlayerKilled, 0, 4);
    set(sithCogFunctionPlayer_SetPlayerSuicides, 0, 4);
    stdComm_WSRelay_MatchScores();
    CHECK("a joiner's script changes only what the game lets it", jkPlayer_playerInfos[0].numKills == 0 &&
          jkPlayer_playerInfos[0].score == 0 && jkPlayer_playerInfos[0].numKilled == 0);
    CHECK("and its game says nothing", lastScripted == 0);

    // Not in multiplayer at all: nothing is kept for a later match.
    newGame(0, 0);
    set(sithCogFunctionPlayer_SetPlayerKills, 0, 4);
    set(sithCogFunctionPlayer_SetPlayerScore, 0, 4);
    CHECK("single player sets them", jkPlayer_playerInfos[0].numKills == 4 && jkPlayer_playerInfos[0].score == 4);
    CHECK("and nothing is noted", wsrelay_matchScripted == 0);

    // A level the lobby isn't told of (the Imperial relay's): what its
    // script set is dropped at its end, not carried into the next match.
    newGame(1, 1);
    stdComm_Imperial_bActive = 1;
    stdComm_WSRelay_MatchStart();
    CHECK("no start is told", starts == 0 && scores == 0);
    set(sithCogFunctionPlayer_SetPlayerKills, 0, 9);
    stdComm_WSRelay_MatchEnd(0);
    CHECK("nor an end", ends == 0 && scores == 0);
    stdComm_Imperial_bActive = 0;
    jkPlayer_playerInfos[0].numKills = 0;
    stdComm_WSRelay_MatchStart();
    CHECK("the next match starts with nothing set", starts == 1 && lastScripted == 0);

    if (failed)
        printf("%d failed\n", failed);
    else
        printf("all passed\n");
    return failed ? 1 : 0;
}
