// Tests for joining a room with a password on the relay
// (src/Platform/Networking/WSRelay/stdComm_WSRelay.c): when the lobby that
// listed a game checks its password, the join takes the one the player
// typed for a game shown with the password box and for no other, the log
// never shows it, and the lobby's answers become the join screen's dialogs. The tree's own list, join and reply code runs against a
// made-up page and lobby. passwords-test.sh cuts that code out of the
// sources and builds this file around it. From the top of the source tree:
//
//   sh packaging/wasm/test/passwords-test.sh

// strtok_r
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>

// The session flags, GUID_idk, jkMultiEntry and STDCOMM_WSRELAY_*, from src/.
#include "passwords-types.inc"

typedef uint32_t DPID;
#define OPENJKDF2_RESTART_PATH (3)
// Only sizes wsrelay_aSwing here.
#define JKPLAYER_NUM_INFOS (32)

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

// ---- The engine around the code under test.

int Main_bVerboseNetworking = 1;
int Main_bMotsCompat = 0;
int stdComm_Imperial_bActive = 0;
int stdComm_dword_8321E8, stdComm_dword_8321E0, stdComm_bIsServer, stdComm_dword_8321DC;
DPID stdComm_dplayIdSelf;
char16_t jkPlayer_playerShortName[32];
int jkGuiMultiplayer_checksumSeed, jkGuiMultiplayer_dword_5564E8, dplay_dword_55D618;
jkMultiEntry jkGuiMultiplayer_aEntries[32];
GUID_idk jkGui_guid_556040;
char openjkdf2_aRestartPath[256];
int openjkdf2_restartMode, g_should_exit;
int sithMulti_multiModeFlags, sithNet_MultiModeFlags;

// Everything the engine logs.
static char logText[1 << 16];

static void stdPlatform_Printf(const char* fmt, ...)
{
    size_t n = strlen(logText);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(logText + n, sizeof(logText) - n, fmt, ap);
    va_end(ap);
}

static uint32_t now = 1000;
static uint32_t stdPlatform_GetTimeMsec(void) { return now; }

static void stdString_SafeStrCopy(char* pDst, const char* pSrc, uint32_t lenDst)
{
    strncpy(pDst, pSrc, lenDst - 1);
    pDst[lenDst - 1] = 0;
}

static int imperialOpens = 0;
static int stdComm_Imperial_Open(int idx, char16_t* pwPassword) { imperialOpens++; return 0x80004005; }
static int stdComm_Imperial_EnumSessions(void) { return 0; }
static void stdComm_Imperial_Startup(void) {}
static void stdComm_Imperial_CloseConnection(void) {}
static void DirectPlay_Close(void) {}
int jkGuiMultiplayer_numConnections;
struct { char16_t name[0x80]; } jkGuiMultiplayer_aConnections[1];
static int jk_snwprintf(char16_t* pOut, size_t n, const char16_t* pFmt, ...) { pOut[0] = 0; return 0; }
static DPID DirectPlay_CreatePlayer(char16_t* pwName, int idk) { return 1; }
static void Hack_ResetClients(void) {}
static int stdComm_WSRelay_ClampReach(int reachMs) { return reachMs; }
static void stdComm_WSRelay_SetHitRules(int rules, int reachMs) {}
static void stdComm_WSRelay_ResetHits(void) {}
static void stdComm_WSRelay_ResetMotion(void) {}
static void wsrelay_PeerLeft(int id) {}
static void wsrelay_WelcomePeer(int id) {}
static void wsrelay_UpdateLobby(void) {}
static void wsrelay_PeerLeftOnRelay(int id) {}
static int wsrelay_GetHostMod(void) { return 1; } // WSRELAY_MAP_READY
static int wsrelay_GetHostMap(const char* pEpisode) { return 1; }

// ---- The page (the EM_JS functions) and the lobby behind it.

// Lines from the lobby, waiting for the engine.
static char inbox[8][1024];
static int inHead = 0, inTail = 0;
// 0 connecting, 1 open, 2 closed (WSRELAY_STATE_*). A new socket is open
// the next time the engine looks.
static int socketState = 1;
static int connects = 0;
// Whether the lobby line of the socket a list comes in on said pw, which is
// what the page notes for that list (net.listPw).
static int lobbyPw = 0;
// The last line sent, and what join got: its room and the units passed.
static char lastSent[1024];
static int joins = 0;
static int joinRoom = -1;
static char16_t joinPassword[64];
static int joinHasPassword = 0;
// What the lobby answers the next join with.
static char joinAnswer[1024];

static void lobbySays(const char* pLine)
{
    snprintf(inbox[inTail++ % 8], sizeof(inbox[0]), "%s", pLine);
}

static void emscripten_sleep(unsigned int ms) { now += ms; }
static void wsrelay_js_connect(int netcode) { connects++; socketState = 0; }

static int wsrelay_js_state(void)
{
    int state = socketState;
    if (state == 0)
        socketState = 1;
    return state;
}
static int wsrelay_js_list_pw(void) { return lobbyPw; }
static void wsrelay_js_rtc_begin(int isHost) {}
static void wsrelay_js_rtc_end(void) {}
static void wsrelay_js_map_unhost(void) {}
static void wsrelay_js_mod_unhost(void) {}
static int wsrelay_js_direct_open(int id) { return 0; }
static void wsrelay_js_mod_restart(char* pOut, int maxLen) { pOut[0] = 0; }
static int wsrelay_js_take_join_room(void) { return 0; }
static void wsrelay_js_presence(char16_t* pName, int* pMots) {}

static int wsrelay_js_send_text(const char* pText)
{
    snprintf(lastSent, sizeof(lastSent), "%s", pText);
    return 1;
}

static int wsrelay_js_join(int room, const char16_t* pPassword)
{
    joins++;
    joinRoom = room;
    joinHasPassword = pPassword != NULL;
    memset(joinPassword, 0, sizeof(joinPassword));
    for (int i = 0; pPassword && pPassword[i] && i < 63; i++)
        joinPassword[i] = pPassword[i];
    snprintf(lastSent, sizeof(lastSent), "join %d%s", room, pPassword ? " <password>" : "");
    if (joinAnswer[0])
        lobbySays(joinAnswer);
    return 1;
}

static int wsrelay_js_recv(void* pOut, int maxLen, int* pIsText, int32_t* pSeq)
{
    if (inHead == inTail)
        return -1;
    snprintf((char*)pOut, maxLen, "%s", inbox[inHead++ % 8]);
    *pIsText = 1;
    return (int)strlen((char*)pOut);
}

// The tree's own code (passwords-test.sh).
#include "passwords-code.inc"

// ---- Helpers.

static void put16(char16_t* pDst, const char16_t* pSrc)
{
    while (*pSrc)
        *pDst++ = *pSrc++;
    *pDst = 0;
}

static int same16(const char16_t* a, const char16_t* b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

// An entry for room as the lobby lists it, in hex: its password and flags.
static void entryHex(char* pOut, const char16_t* pPassword, int flags)
{
    jkMultiEntry e;
    memset(&e, 0, sizeof(e));
    e.maxPlayers = 4;
    e.numPlayers = 1;
    put16(e.serverName, u"t_Private");
    strcpy(e.episodeGobName, "JK1MP");
    strcpy(e.mapJklFname, "01narshadda.jkl");
    put16(e.wPassword, pPassword);
    e.sessionFlags = flags;
    e.field_E0 = 10;
    const uint8_t* b = (const uint8_t*)&e;
    for (size_t i = 0; i < sizeof(e); i++)
        sprintf(pOut + i * 2, "%02x", b[i]);
}

// The join screen asking for its list: rooms 3 (with a password) and 4
// (open), as a lobby that does or doesn't check passwords lists them.
static void listFrom(int bLobbyChecks)
{
    static char line[2048];
    char a[sizeof(jkMultiEntry) * 2 + 1], b[sizeof(jkMultiEntry) * 2 + 1];
    // A lobby that checks lists no password, and the box for one that has one.
    entryHex(a, bLobbyChecks ? u"" : u"sekrit9", SESSIONFLAG_PASSWORD | 8);
    entryHex(b, u"", 8);
    snprintf(line, sizeof(line), "list 3 1 %s 4 1 %s", a, b);
    lobbyPw = bLobbyChecks;
    lobbySays(line);
    stdComm_EnumSessions(1, NULL);
}

// stdComm_Open for listed game idx with box typed, the lobby answering its
// join with answer (a welcome when NULL).
static int join(int idx, const char16_t* box, const char* answer)
{
    static char16_t typed[32];
    put16(typed, box);
    if (answer)
    {
        snprintf(joinAnswer, sizeof(joinAnswer), "%s", answer);
    }
    else
    {
        char hex[sizeof(jkMultiEntry) * 2 + 1];
        entryHex(hex, u"", SESSIONFLAG_PASSWORD | 8);
        snprintf(joinAnswer, sizeof(joinAnswer), "welcome 2 %s", hex);
    }
    logText[0] = 0;
    joinRoom = -1;
    joinHasPassword = 0;
    return stdComm_Open(idx, typed);
}

// ---- Tests.

// A lobby that checks passwords: the join takes the one typed, and the log
// doesn't show it.
static void test_lobby_checks(void)
{
    listFrom(1);
    CHECK(!strcmp(lastSent, "list jk1"), "the join screen asks for the list: %s", lastSent);
    CHECK(wsrelay_numShown == 2 && wsrelay_aShownRooms[0] == 3 && wsrelay_aShownRooms[1] == 4, "rooms 3 and 4 shown");
    CHECK(stdComm_WSRelay_LobbyChecksPassword(0) && stdComm_WSRelay_LobbyChecksPassword(1),
          "the lobby checks both games' passwords, so the join screen doesn't");

    int r = join(0, u"pässwörd", NULL);
    CHECK(r == 0, "the right password joins: %x", r);
    CHECK(joinRoom == 3 && joinHasPassword && same16(joinPassword, u"pässwörd"), "join 3 with the password typed");
    CHECK(strstr(logText, "Relay << join 3 <hidden>\n") != NULL, "the log says <hidden>: %s", logText);
    CHECK(!strstr(logText, "p\xc3\xa4ssw") && !strstr(logText, "7000e400"), "the log has no password");

    // An open game shows no password box, which may still hold what was
    // typed for another game: that stays here.
    r = join(1, u"p\u00e4ssw\u00f6rd", NULL);
    CHECK(r == 0, "an open game joins: %x", r);
    CHECK(joinRoom == 4 && !joinHasPassword, "with a bare join, whatever is left in the box");
    CHECK(strstr(logText, "Relay << join 4\n") != NULL, "logged as a bare join: %s", logText);

    // The lobby's answers.
    CHECK(join(0, u"wrong", "error password") == 0x88770154, "error password: the wrong password dialog");
    CHECK(!strcmp(lastSent, "leave"), "and the game leaves, as for any refusal: %s", lastSent);
    CHECK(strstr(logText, "Relay error: password\n") != NULL, "logged: %s", logText);
    CHECK(join(0, u"pässwörd", "error password wait") == STDCOMM_WSRELAY_PASSWORD_WAIT,
          "error password wait: its own dialog, not the wrong password one");
    CHECK(STDCOMM_WSRELAY_PASSWORD_WAIT != 0x88770154 && STDCOMM_WSRELAY_PASSWORD_WAIT != STDCOMM_WSRELAY_NEWER &&
          STDCOMM_WSRELAY_PASSWORD_WAIT != STDCOMM_WSRELAY_OLDER && STDCOMM_WSRELAY_PASSWORD_WAIT != STDCOMM_WSRELAY_RESTART &&
          STDCOMM_WSRELAY_PASSWORD_WAIT != 0x80004005 && STDCOMM_WSRELAY_PASSWORD_WAIT != 0x8877014A,
          "which no other answer shares");

    // Nothing typed: a bare join, which the lobby turns down for a room with
    // a password.
    CHECK(join(0, u"", "error password") == 0x88770154, "an empty box: wrong password");
    CHECK(joinRoom == 3 && !joinHasPassword, "a bare join");
    CHECK(strstr(logText, "Relay << join 3\n") != NULL, "logged as a bare join: %s", logText);

    // The other refusals are as before.
    CHECK(join(0, u"x", "error version newer") == STDCOMM_WSRELAY_NEWER, "newer");
    CHECK(join(0, u"x", "error version older") == STDCOMM_WSRELAY_OLDER, "older");
    CHECK(join(0, u"x", "error full") == 0x8877014A, "full");
    CHECK(join(0, u"x", "error no-room") == 0x80004005, "no room");
    CHECK(join(0, u"x", "error bad-request") == 0x80004005, "bad request");
}

// What was listed is what counts: a socket that comes back, to a lobby that
// says something else, doesn't change what joining a shown game sends until
// the join screen lists again.
static void test_snapshot(void)
{
    listFrom(1);
    // The socket drops (the lobby is updated). Its next one is to a lobby
    // that doesn't check, whose list comes in while the player joins.
    socketState = 2;
    lobbyPw = 0;
    int was = connects;
    char hex[sizeof(jkMultiEntry) * 2 + 1];
    entryHex(hex, u"sekrit9", SESSIONFLAG_PASSWORD | 8);
    static char late[1024];
    snprintf(late, sizeof(late), "list 3 1 %s", hex);
    lobbySays(late);
    CHECK(join(0, u"sekrit9", "error bad-request") == 0x80004005, "that lobby can't take a password");
    CHECK(connects == was + 1, "the join connected again");
    CHECK(joinRoom == 3 && joinHasPassword, "and sent the password, as the list it was shown said");
    CHECK(wsrelay_aListPw[0] == 0 && wsrelay_aShownPw[0] == 1, "the late list is noted, the shown one kept");
    CHECK(stdComm_WSRelay_LobbyChecksPassword(0), "still checked by the lobby that listed it");

    // The join screen lists again: now the game checks the password itself.
    listFrom(0);
    CHECK(!stdComm_WSRelay_LobbyChecksPassword(0), "listed by a lobby that doesn't check");
    CHECK(join(0, u"sekrit9", NULL) == 0, "joins");
    CHECK(joinRoom == 3 && !joinHasPassword, "with a bare join");
    CHECK(strstr(logText, "Relay << join 3\n") != NULL && !strstr(logText, "<hidden>"), "logged as before");

    // And back to one that checks.
    listFrom(1);
    CHECK(stdComm_WSRelay_LobbyChecksPassword(0), "checked again");
}

// The join screen doesn't wait for its list (it held the screen still for a
// round trip to the lobby every 2 s): stdComm_EnumSessions asks, and
// stdComm_WSRelay_ListReady, each frame, says when what came back is in
// jkGuiMultiplayer_aEntries, as the wait put it there.
static void test_list_later(void)
{
    char hex[sizeof(jkMultiEntry) * 2 + 1];
    static char line[1024];
    entryHex(hex, u"", 8);

    listFrom(1);
    CHECK(wsrelay_numShown == 2 && dplay_dword_55D618 == 2, "an answer already in is shown at once");
    CHECK(!stdComm_WSRelay_ListReady(), "and nothing waits");

    // Asked; the answer comes two frames later.
    int r = stdComm_EnumSessions(1, NULL);
    CHECK(r == STDCOMM_WSRELAY_LIST_LATER, "asked, not waited for: %x", r);
    CHECK(!strcmp(lastSent, "list jk1"), "the ask went out: %s", lastSent);
    CHECK(wsrelay_numShown == 2 && dplay_dword_55D618 == 2 && wsrelay_aShownRooms[0] == 3, "what's shown stays as it was");
    now += 16;
    CHECK(!stdComm_WSRelay_ListReady(), "nothing yet");
    snprintf(line, sizeof(line), "list 7 2 %s", hex);
    lobbySays(line);
    now += 16;
    CHECK(stdComm_WSRelay_ListReady(), "it came");
    CHECK(wsrelay_numShown == 1 && dplay_dword_55D618 == 1 && wsrelay_aShownRooms[0] == 7 &&
          jkGuiMultiplayer_aEntries[0].numPlayers == 2, "and it's shown");
    CHECK(!stdComm_WSRelay_ListReady(), "once");

    // No answer: after WSRELAY_LIST_MS the list it had, as the wait gave up.
    // That's the one up already, which isn't put up again: a rebuild would
    // undo a click on a row half done (test_give_up).
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked again");
    now += WSRELAY_LIST_MS - 1;
    CHECK(!stdComm_WSRelay_ListReady() && !wsrelay_bListGaveUp, "still waiting at %d ms", WSRELAY_LIST_MS - 1);
    now += 1;
    CHECK(!stdComm_WSRelay_ListReady() && wsrelay_bListGaveUp && wsrelay_numShown == 1 && wsrelay_aShownRooms[0] == 7,
          "gave up, the list it had, which stays up");

    // Its answer comes late: it goes up when it comes (test_list_late).
    snprintf(line, sizeof(line), "list 8 3 %s", hex);
    lobbySays(line);
    now += 200;
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 8, "a late answer goes up when it comes");
    CHECK(!stdComm_WSRelay_ListReady(), "with nothing left waiting");

    // The socket goes while it waits: no games.
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    socketState = 2;
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_numShown == 0 && dplay_dword_55D618 == 0, "the socket went: none shown");
    int was = connects;
    CHECK(stdComm_EnumSessions(1, NULL) == 0 && connects == was + 1 && dplay_dword_55D618 == 0, "the next ask connects, none shown");

    // The join screen is done with it (Cancel, or joining a game).
    listFrom(1);
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    DirectPlay_EnumSessions2();
    snprintf(line, sizeof(line), "list 9 1 %s", hex);
    lobbySays(line);
    CHECK(!stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 3, "what comes after isn't shown");
    // Back on the join screen, the next ask doesn't take that old answer for
    // its own (which would put every list after it up an ask late).
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER && wsrelay_aShownRooms[0] == 3,
          "nor at the next ask, which waits for its own");
    snprintf(line, sizeof(line), "list 10 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 10, "and shows that when it comes");
    // Without it, the latest there is, as the wait gave up with.
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    DirectPlay_EnumSessions2();
    snprintf(line, sizeof(line), "list 11 1 %s", hex);
    lobbySays(line);
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER && wsrelay_aShownRooms[0] == 10, "asked again");
    now += WSRELAY_LIST_MS;
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 11, "gave up: the latest it had");
    // One dropped while joining was read by the join's own wait.
    listFrom(1);
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    snprintf(line, sizeof(line), "list 12 1 %s", hex);
    lobbySays(line);
    CHECK(join(1, u"", "error full") == 0x8877014A && wsrelay_aListRooms[0] == 12 && wsrelay_aShownRooms[1] == 4,
          "the join read it, and what's shown stays");
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "the next ask waits for its own");
    snprintf(line, sizeof(line), "list 13 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 13, "and shows it");

    // Not on the Imperial Network Protocol connection.
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    stdComm_Imperial_bActive = 1;
    lobbySays(line);
    CHECK(!stdComm_WSRelay_ListReady(), "the relay's list isn't Imperial's");
    stdComm_Imperial_bActive = 0;
    CHECK(stdComm_WSRelay_ListReady(), "the relay's again");
}

// An answer that comes after the 500 ms give-up still goes up when it comes,
// so the next ask doesn't take it for its own (every list after it an ask
// late); and a list Jedi Knight asked for isn't shown after a restart into
// Mysteries of the Sith, nor the other way round.
static void test_list_late(void)
{
    char hex[sizeof(jkMultiEntry) * 2 + 1];
    static char line[1024];
    entryHex(hex, u"", 8);

    listFrom(1);
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    now += WSRELAY_LIST_MS;
    CHECK(!stdComm_WSRelay_ListReady() && wsrelay_bListGaveUp && wsrelay_aShownRooms[0] == 3,
          "gave up: the list it had stays up");
    snprintf(line, sizeof(line), "list 20 1 %s", hex);
    lobbySays(line);
    now += 200;
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 20, "the late answer goes up when it comes");
    CHECK(!stdComm_WSRelay_ListReady(), "once");
    now += 1300;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "the next ask waits for its own");
    snprintf(line, sizeof(line), "list 21 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 21, "and shows it");

    // Given up on, then the socket goes: no games, as when it goes before.
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    now += WSRELAY_LIST_MS;
    CHECK(!stdComm_WSRelay_ListReady() && wsrelay_bListGaveUp && wsrelay_aShownRooms[0] == 21, "gave up");
    socketState = 2;
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_numShown == 0 && dplay_dword_55D618 == 0, "the socket went: none shown");
    CHECK(!stdComm_WSRelay_ListReady(), "once");
    CHECK(stdComm_EnumSessions(1, NULL) == 0 && dplay_dword_55D618 == 0, "the next ask connects");
    listFrom(1);

    // Given up on, then the screen closes: its answer isn't the next ask's.
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    now += WSRELAY_LIST_MS;
    CHECK(!stdComm_WSRelay_ListReady() && wsrelay_bListGaveUp, "gave up");
    DirectPlay_EnumSessions2();
    snprintf(line, sizeof(line), "list 22 1 %s", hex);
    lobbySays(line);
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "back: the next ask waits for its own");
    snprintf(line, sizeof(line), "list 23 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 23, "and shows it");

    // The engine restarts into Mysteries of the Sith while Jedi Knight's ask
    // is out, whose answer comes in meanwhile, and the first answer it gets
    // is late: no Jedi Knight games meanwhile.
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked for jk1");
    stdComm_WSRelay_Startup();
    socketState = 1;
    Main_bMotsCompat = 1;
    snprintf(line, sizeof(line), "list 24 1 %s", hex);
    lobbySays(line);
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER && !strcmp(lastSent, "list mots"), "asked for mots");
    now += WSRELAY_LIST_MS;
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_numShown == 0, "gave up: none, not Jedi Knight's");
    snprintf(line, sizeof(line), "list 25 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 25, "then its own");

    // Back to Jedi Knight with nothing out: the last list it read is
    // Mysteries of the Sith's.
    stdComm_WSRelay_Startup();
    socketState = 1;
    Main_bMotsCompat = 0;
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER && !strcmp(lastSent, "list jk1"), "asked for jk1");
    now += WSRELAY_LIST_MS;
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_numShown == 0, "gave up: none, not Mysteries of the Sith's");
    listFrom(1);
}

// The join screen opening (jkGuiMultiplayer_Show), after it clears the last
// visit's selection: passwords-test.sh cuts it out of
// src/Gui/jkGUIMultiplayer.c.
static void joinScreenOpens(void)
{
#include "passwords-open.inc"
}

// Back on the join screen, its list box is empty until this visit's list is
// up, and a click on one of its rows picks the game at that row if
// dplay_dword_55D618 says there are that many (jkGuiMultiplayer_sub_413E00):
// none of the last visit's. Nor are they put up when the lobby answers
// later than WSRELAY_LIST_MS: the give-up puts up no games then, as on the
// first visit.
static void test_screen_opens(void)
{
    char hex[sizeof(jkMultiEntry) * 2 + 1];
    static char line[1024];
    entryHex(hex, u"", 8);

    listFrom(1);
    CHECK(dplay_dword_55D618 == 2, "two games up");
    DirectPlay_EnumSessions2();
    stdComm_CloseConnection();
    now += 3000;
    joinScreenOpens();
    CHECK(dplay_dword_55D618 == 0, "back on the join screen: none of them to pick");
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER && dplay_dword_55D618 == 0, "nor while it asks");
    snprintf(line, sizeof(line), "list 40 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && dplay_dword_55D618 == 1 && wsrelay_aShownRooms[0] == 40, "this visit's");

    // The lobby is slow. Back on the join screen, the give-up puts up no
    // games, and this visit's when they come.
    snprintf(line, sizeof(line), "list 41 1 %s 42 1 %s", hex, hex);
    lobbySays(line);
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == 0 && dplay_dword_55D618 == 2 && wsrelay_aShownRooms[1] == 42, "two games up");
    DirectPlay_EnumSessions2();
    stdComm_CloseConnection();
    now += 60000;
    joinScreenOpens();
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER && dplay_dword_55D618 == 0, "back: asked");
    CHECK(wsrelay_numListed == 0 && wsrelay_bListUnshown, "the last visit's list is gone, and nothing's up");
    now += WSRELAY_LIST_MS - 1;
    CHECK(!stdComm_WSRelay_ListReady(), "the box stays empty till the give-up");
    now += 1;
    CHECK(stdComm_WSRelay_ListReady() && dplay_dword_55D618 == 0 && wsrelay_numShown == 0,
          "gave up: no games, not the last visit's");
    CHECK(!stdComm_WSRelay_ListReady(), "once");
    snprintf(line, sizeof(line), "list 43 1 %s", hex);
    lobbySays(line);
    now += 200;
    CHECK(stdComm_WSRelay_ListReady() && dplay_dword_55D618 == 1 && wsrelay_aShownRooms[0] == 43, "then this visit's");
    // The next ask's give-up keeps it up (test_give_up).
    now += 1300;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked again");
    now += WSRELAY_LIST_MS;
    CHECK(!stdComm_WSRelay_ListReady() && dplay_dword_55D618 == 1 && wsrelay_aShownRooms[0] == 43, "gave up: it stays up");
    snprintf(line, sizeof(line), "list 44 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 44, "and the answer goes up");

    // Esc while an ask is out, whose answer comes in meanwhile: the next
    // visit's first ask reads it first (wsrelay_DropListAsk), and doesn't
    // give up with it either.
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    DirectPlay_EnumSessions2();
    stdComm_CloseConnection();
    snprintf(line, sizeof(line), "list 45 1 %s 46 1 %s", hex, hex);
    lobbySays(line);
    now += 5000;
    joinScreenOpens();
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER && dplay_dword_55D618 == 0, "back: asked");
    CHECK(wsrelay_aListRooms[0] == 45 && wsrelay_numListed == 0, "the last ask's answer was read, and is gone");
    now += WSRELAY_LIST_MS;
    CHECK(stdComm_WSRelay_ListReady() && dplay_dword_55D618 == 0 && wsrelay_numShown == 0,
          "gave up: no games, not the last ask's");
    snprintf(line, sizeof(line), "list 47 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && dplay_dword_55D618 == 1 && wsrelay_aShownRooms[0] == 47, "then this visit's");

    // The socket went meanwhile: the first ask connects, and the first one
    // on the new socket gives up with no games.
    DirectPlay_EnumSessions2();
    stdComm_CloseConnection();
    socketState = 2;
    now += 3000;
    joinScreenOpens();
    int was = connects;
    CHECK(stdComm_EnumSessions(1, NULL) == 0 && connects == was + 1 && dplay_dword_55D618 == 0, "back: it connects");
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked on the new socket");
    now += WSRELAY_LIST_MS;
    CHECK(stdComm_WSRelay_ListReady() && dplay_dword_55D618 == 0 && wsrelay_numShown == 0,
          "gave up: no games, not the last visit's");
    snprintf(line, sizeof(line), "list 48 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 48, "then this visit's");

    // The last visit had no games: the give-up puts up "No games to join"
    // (the box is empty till then).
    lobbySays("list");
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == 0 && dplay_dword_55D618 == 0 && wsrelay_numShown == 0, "no games up");
    DirectPlay_EnumSessions2();
    stdComm_CloseConnection();
    now += 3000;
    joinScreenOpens();
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "back: asked");
    now += WSRELAY_LIST_MS;
    CHECK(stdComm_WSRelay_ListReady() && dplay_dword_55D618 == 0, "gave up: no games");
    snprintf(line, sizeof(line), "list 49 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 49, "then this visit's");
}

// The give-up (WSRELAY_LIST_MS) doesn't put up again the list that's up
// already. Each rebuild of the list box sets its selection back from
// jkGui_guid_556040 (jkGuiMultiplayer_sub_4140B0), so a click whose button
// went down before one and up after it picked the row's old game: a lobby
// slower than that rebuilt it twice an ask, at the give-up and the answer.
// A list read since, or none, still goes up then.
static void test_give_up(void)
{
    char hex[sizeof(jkMultiEntry) * 2 + 1];
    static char line[1024];
    entryHex(hex, u"", 8);

    // Answers 700 ms late: one rebuild an ask, the answer's.
    listFrom(1);
    int rebuilds = 0;
    for (int i = 0; i < 5; i++)
    {
        now += 2000;
        CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "ask %d", i);
        for (int t = 0; t < 700; t += 16)
        {
            now += 16;
            rebuilds += stdComm_WSRelay_ListReady();
        }
        snprintf(line, sizeof(line), "list %d 1 %s 4 1 %s", 50 + i, hex, hex);
        lobbySays(line);
        now += 16;
        rebuilds += stdComm_WSRelay_ListReady();
    }
    CHECK(rebuilds == 5 && wsrelay_aShownRooms[0] == 54, "a lobby 700 ms late: one rebuild an ask, not %d", rebuilds);

    // A list read meanwhile, by a join's own wait, goes up at the give-up.
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    snprintf(line, sizeof(line), "list 60 1 %s", hex);
    lobbySays(line);
    CHECK(join(1, u"", "error full") == 0x8877014A && wsrelay_aListRooms[0] == 60 && wsrelay_aShownRooms[0] == 54,
          "the join read a list");
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    now += WSRELAY_LIST_MS;
    CHECK(stdComm_WSRelay_ListReady() && dplay_dword_55D618 == 1 && wsrelay_aShownRooms[0] == 60,
          "gave up: that one goes up");
    CHECK(!stdComm_WSRelay_ListReady(), "once");
    snprintf(line, sizeof(line), "list 61 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 61, "then the answer");

    // No games up: the give-up puts "No games to join" up again, which has
    // no row to click.
    lobbySays("list");
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == 0 && dplay_dword_55D618 == 0, "no games up");
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    now += WSRELAY_LIST_MS;
    CHECK(stdComm_WSRelay_ListReady() && dplay_dword_55D618 == 0, "gave up: none");
    listFrom(1);
}

// The other ways the join screen is done with its ask: Ok with no game
// picked (stdComm_CloseConnection) and an engine restart
// (stdComm_WSRelay_Startup). Neither takes the answer still to come for the
// next ask's, nor puts it up on the way back.
static void test_list_dropped(void)
{
    char hex[sizeof(jkMultiEntry) * 2 + 1];
    static char line[1024];
    entryHex(hex, u"", 8);

    listFrom(1);
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    stdComm_CloseConnection();
    snprintf(line, sizeof(line), "list 30 1 %s", hex);
    lobbySays(line);
    CHECK(!stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 3, "closed: what comes after isn't shown");
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "back: the next ask waits for its own");
    snprintf(line, sizeof(line), "list 31 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 31, "and shows it");

    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "asked");
    stdComm_WSRelay_Startup();
    socketState = 1;
    snprintf(line, sizeof(line), "list 32 1 %s", hex);
    lobbySays(line);
    CHECK(!stdComm_WSRelay_ListReady() && dplay_dword_55D618 == 0, "restarted: nothing shown");
    now += 2000;
    CHECK(stdComm_EnumSessions(1, NULL) == STDCOMM_WSRELAY_LIST_LATER, "after the restart: the next ask waits for its own");
    snprintf(line, sizeof(line), "list 33 1 %s", hex);
    lobbySays(line);
    CHECK(stdComm_WSRelay_ListReady() && wsrelay_aShownRooms[0] == 33, "and shows it");
}

// Which games the lobby checks for: none on the Imperial Network Protocol
// connection, and none past the list.
static void test_lobby_checks_password(void)
{
    listFrom(1);
    CHECK(!stdComm_WSRelay_LobbyChecksPassword(-1), "-1");
    CHECK(!stdComm_WSRelay_LobbyChecksPassword(2), "past the list");
    stdComm_Imperial_bActive = 1;
    CHECK(!stdComm_WSRelay_LobbyChecksPassword(0), "not on Imperial");
    int joinsBefore = joins;
    CHECK(join(0, u"sekrit9", NULL) == 0x80004005 && imperialOpens == 1 && joins == joinsBefore,
          "an Imperial join goes to Imperial, and nothing to the lobby");
    stdComm_Imperial_bActive = 0;
    CHECK(stdComm_WSRelay_LobbyChecksPassword(0), "the relay's again");

    // A list the lobby doesn't send yet: nothing shown.
    socketState = 2;
    stdComm_EnumSessions(1, NULL);
    CHECK(wsrelay_numShown == 0 && !stdComm_WSRelay_LobbyChecksPassword(0), "no list, nothing checked");
    socketState = 1;
    // Not constants, which the compiler would see past the array with.
    volatile int most = WSRELAY_MAX_LISTED, far = 1000000;
    CHECK(!stdComm_WSRelay_LobbyChecksPassword(most), "past the most listed");
    CHECK(!stdComm_WSRelay_LobbyChecksPassword(far), "far past it");
}

int main(void)
{
    // Each line as it's printed, should a test crash.
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_lobby_checks();
    test_snapshot();
    test_lobby_checks_password();
    test_list_later();
    test_list_late();
    test_list_dropped();
    test_screen_opens();
    test_give_up();

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
