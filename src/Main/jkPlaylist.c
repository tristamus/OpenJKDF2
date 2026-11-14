#include "jkPlaylist.h"

// Added: the host's level playlist (jkPlaylist.h says how it works).

#ifdef PLATFORM_WSRELAY

#include <string.h>
#include <emscripten.h>

#include "General/stdString.h"
#include "Dss/sithMulti.h"
#include "Main/jkRes.h"
#include "Platform/Networking/Imperial/stdComm_Imperial.h"
#include "stdPlatform.h"
#include "jk.h"

jkPlaylist jkPlaylist_list;
jkPlaylistNext jkPlaylist_next;

static int jkPlaylist_bHosting = 0;
static int jkPlaylist_cur = 0;          // the level playing now, while hosting
static int jkPlaylist_bPicked = 0;      // this level's end has named the next one
// The players the host waits for: those in the game when the level ended.
static int32_t jkPlaylist_aWaitIds[32];
static int jkPlaylist_numWaitIds = 0;
static char16_t jkPlaylist_wFailed[128];

// ---- The page: the playlist kept in this browser (localStorage), and the
// next level's GOB (maps.js, Module.jkMaps.next).

EM_JS(int, jkPlaylist_js_load, (int bMots), {
    var list = null;
    try {
        list = JSON.parse(localStorage.getItem('jkdf2mp-playlist-' + (bMots ? 'mots' : 'jk1')) || 'null');
    } catch (e) {}
    if (!list || list.v !== 1 || !Array.isArray(list.items))
        list = { v: 1, name: "", loop: 1, items: [] };
    Module.jkPlaylistLoaded = list;
    return Math.min(list.items.length, 6);
});

// What jkPlaylist_js_load read: field 0 the name, 1 whether it loops; of
// item i, 2 its GOB, 3 its level, 4 its title, 5 its type.
EM_JS(int, jkPlaylist_js_field, (int field, int i, void* pOut, int outLen), {
    var list = Module.jkPlaylistLoaded || { items: [] };
    var item = list.items[i] || {};
    var s;
    switch (field) {
    case 0: s = String(list.name || ""); break;
    case 1: return list.loop ? 1 : 0;
    case 2: s = String(item.gob || ""); break;
    case 3: s = String(item.jkl || ""); break;
    case 4: s = String(item.title || ""); break;
    case 5: return item.type | 0;
    default: return 0;
    }
    if (field === 2 || field === 3) {
        // Names of files: ASCII as the game keeps them.
        s = s.replace(/[^\x20-\x7e]/g, '_');
        stringToUTF8(s, pOut, outLen);
    } else {
        var n = Math.min(s.length, outLen - 1);
        for (var k = 0; k < n; k++)
            HEAPU16[(pOut >> 1) + k] = s.charCodeAt(k);
        HEAPU16[(pOut >> 1) + n] = 0;
    }
    return 1;
});

EM_JS(void, jkPlaylist_js_saveBegin, (const char16_t* pName, int bLoop), {
    var name = "";
    for (var i = 0; HEAPU16[(pName >> 1) + i]; i++)
        name += String.fromCharCode(HEAPU16[(pName >> 1) + i]);
    Module.jkPlaylistSaving = { v: 1, name: name, loop: bLoop ? 1 : 0, items: [] };
});

EM_JS(void, jkPlaylist_js_saveItem, (const char* pGob, const char* pJkl, const char16_t* pTitle, int type), {
    var title = "";
    for (var i = 0; HEAPU16[(pTitle >> 1) + i]; i++)
        title += String.fromCharCode(HEAPU16[(pTitle >> 1) + i]);
    if (Module.jkPlaylistSaving)
        Module.jkPlaylistSaving.items.push({ gob: UTF8ToString(pGob), jkl: UTF8ToString(pJkl), title: title, type: type });
});

EM_JS(void, jkPlaylist_js_saveEnd, (int bMots), {
    try {
        localStorage.setItem('jkdf2mp-playlist-' + (bMots ? 'mots' : 'jk1'), JSON.stringify(Module.jkPlaylistSaving));
    } catch (e) {
        console.warn('OpenJKDF2: the playlist could not be saved in this browser: ' + e);
    }
    Module.jkPlaylistSaving = null;
});

// The host: gets the next level's GOB ready for players who don't have it.
EM_JS(void, jkPlaylist_js_hostNext, (const char* pName, const char* pExt), {
    if (Module.jkMaps && Module.jkMaps.next)
        Module.jkMaps.next.host(UTF8ToString(pName), UTF8ToString(pExt));
});

// A joiner: starts getting the next level's GOB, if it doesn't have it.
EM_JS(void, jkPlaylist_js_fetchNext, (const char* pName, const char* pExt), {
    if (Module.jkMaps && Module.jkMaps.next)
        Module.jkMaps.next.fetch(UTF8ToString(pName), UTF8ToString(pExt));
});

// 0 still getting it, 1 has it, 2 couldn't get it.
EM_JS(int, jkPlaylist_js_nextState, (), {
    return Module.jkMaps && Module.jkMaps.next ? Module.jkMaps.next.state() : 1;
});

// How far along, in thousandths, or -1 when it isn't known yet.
EM_JS(int, jkPlaylist_js_nextProgress, (), {
    var p = Module.jkMaps && Module.jkMaps.next ? Module.jkMaps.next.progress() : -1;
    return p >= 0 ? Math.floor(Math.min(1, p) * 1000) : -1;
});

EM_JS(void, jkPlaylist_js_nextError, (char16_t* pOut, int outLen), {
    var s = Module.jkMaps && Module.jkMaps.next ? String(Module.jkMaps.next.error() || "") : "";
    var n = Math.min(s.length, outLen - 1);
    for (var k = 0; k < n; k++)
        HEAPU16[(pOut >> 1) + k] = s.charCodeAt(k);
    HEAPU16[(pOut >> 1) + n] = 0;
});

// The host: how many of these players are still getting the next level (they
// say how far along they are, maps.js T_STATUS), and the least of how far.
EM_JS(int, jkPlaylist_js_waiting, (const int32_t* pIds, int n, int32_t* pLeast), {
    var ids = [];
    for (var i = 0; i < n; i++)
        ids.push(HEAP32[(pIds >> 2) + i]);
    var w = Module.jkMaps && Module.jkMaps.next ? Module.jkMaps.next.waiting(ids) : { count: 0, least: -1 };
    HEAP32[pLeast >> 2] = w.least >= 0 ? Math.floor(Math.min(1, w.least) * 1000) : -1;
    return w.count | 0;
});

// ---- The playlist itself

static void jkPlaylist_ToWide(char16_t* pOut, const char* pIn, int len)
{
    stdString_CharToWchar(pOut, pIn, len - 1);
    pOut[len - 1] = 0;
}

void jkPlaylist_Load(int bMots)
{
    memset(&jkPlaylist_list, 0, sizeof(jkPlaylist_list));
    int n = jkPlaylist_js_load(bMots);
    jkPlaylist_js_field(0, 0, jkPlaylist_list.name, JKPLAYLIST_NAME_LEN);
    jkPlaylist_list.bLoop = jkPlaylist_js_field(1, 0, NULL, 0);
    for (int i = 0; i < n && jkPlaylist_list.count < JKPLAYLIST_MAX; i++)
    {
        jkPlaylistEntry* pEntry = &jkPlaylist_list.entries[jkPlaylist_list.count];
        jkPlaylist_js_field(2, i, pEntry->gob, sizeof(pEntry->gob));
        jkPlaylist_js_field(3, i, pEntry->jkl, sizeof(pEntry->jkl));
        jkPlaylist_js_field(4, i, pEntry->title, JKPLAYLIST_TITLE_LEN);
        pEntry->type = jkPlaylist_js_field(5, i, NULL, 0);
        if (!pEntry->gob[0] || !pEntry->jkl[0])
            continue;
        if (!pEntry->title[0])
            jkPlaylist_ToWide(pEntry->title, pEntry->jkl, JKPLAYLIST_TITLE_LEN);
        jkPlaylist_list.count++;
    }
}

void jkPlaylist_Save(int bMots)
{
    jkPlaylist_js_saveBegin(jkPlaylist_list.name, jkPlaylist_list.bLoop);
    for (int i = 0; i < jkPlaylist_list.count; i++)
    {
        jkPlaylistEntry* pEntry = &jkPlaylist_list.entries[i];
        jkPlaylist_js_saveItem(pEntry->gob, pEntry->jkl, pEntry->title, pEntry->type);
    }
    jkPlaylist_js_saveEnd(bMots);
}

int jkPlaylist_KindOf(int type)
{
    if (type & JK_EPISODE_SPECIAL_CTF)
        return JKPLAYLIST_KIND_CTF;
    if (type & JKM_EPISODE_SABER)
        return JKPLAYLIST_KIND_SABER;
    if (type & (JK_EPISODE_DEATHMATCH | JK_EPISODE_4_UNK))
        return JKPLAYLIST_KIND_DM;
    return JKPLAYLIST_KIND_COOP;
}

const char16_t* jkPlaylist_KindName(int kind)
{
    switch (kind)
    {
        case JKPLAYLIST_KIND_CTF: return Main_bMotsCompat ? u"Kill the Fool" : u"Capture the Flag";
        case JKPLAYLIST_KIND_SABER: return u"saber only";
        case JKPLAYLIST_KIND_COOP: return u"co-op";
        default: return u"deathmatch";
    }
}

void jkPlaylist_SetHosting(int bOn, int idx)
{
    jkPlaylist_bHosting = bOn && jkPlaylist_list.count > 0;
    jkPlaylist_cur = (idx >= 0 && idx < jkPlaylist_list.count) ? idx : 0;
    jkPlaylist_bPicked = 0;
    memset(&jkPlaylist_next, 0, sizeof(jkPlaylist_next));
    if (jkPlaylist_bHosting)
        stdPlatform_Printf("OpenJKDF2: hosting the playlist, %d levels%s, from level %d\n", jkPlaylist_list.count,
                           jkPlaylist_list.bLoop ? " that loop" : "", jkPlaylist_cur + 1);
}

int jkPlaylist_IsHosting(void)
{
    // Only on the lobby's relay: games on the Imperial Network Protocol have
    // desktop players, whose games work the next level out by themselves.
    return jkPlaylist_bHosting && sithNet_isMulti && sithNet_isServer && !stdComm_Imperial_bActive;
}

void jkPlaylist_GameStarted(void)
{
    memset(&jkPlaylist_next, 0, sizeof(jkPlaylist_next));
    jkPlaylist_bPicked = 0;
    jkPlaylist_numWaitIds = 0;
    jkPlaylist_wFailed[0] = 0;
}

void jkPlaylist_LevelStarted(void)
{
    if (jkPlaylist_next.bNamed && !jkPlaylist_next.bEnd && jkPlaylist_IsHosting())
        jkPlaylist_cur = jkPlaylist_next.idx;
    jkPlaylist_next.bNamed = 0;
    jkPlaylist_bPicked = 0;
    jkPlaylist_numWaitIds = 0;
    jkPlaylist_wFailed[0] = 0;
}

int jkPlaylist_HostPick(void)
{
    if (!jkPlaylist_IsHosting())
        return 0;
    if (jkPlaylist_bPicked)
        return jkPlaylist_next.bNamed;
    jkPlaylist_bPicked = 1;

    jkPlaylistNext* pNext = &jkPlaylist_next;
    memset(pNext, 0, sizeof(*pNext));
    pNext->bNamed = 1;
    pNext->count = jkPlaylist_list.count;
    pNext->bLoop = jkPlaylist_list.bLoop;
    int idx = jkPlaylist_cur + 1;
    if (idx >= jkPlaylist_list.count)
    {
        if (!jkPlaylist_list.bLoop)
        {
            pNext->bEnd = 1;
            pNext->idx = jkPlaylist_list.count;
            stdPlatform_Printf("OpenJKDF2: that was the playlist's last level\n");
            return 1;
        }
        idx = 0;
    }
    jkPlaylistEntry* pEntry = &jkPlaylist_list.entries[idx];
    pNext->idx = idx;
    stdString_SafeStrCopy(pNext->gob, pEntry->gob, sizeof(pNext->gob));
    stdString_SafeStrCopy(pNext->jkl, pEntry->jkl, sizeof(pNext->jkl));
    stdString_SafeWStrCopy(pNext->title, pEntry->title, JKPLAYLIST_TITLE_LEN);

    // Who's in the game now, to wait for on the score screen.
    jkPlaylist_numWaitIds = 0;
    for (int i = 0; i < jkPlayer_maxPlayers && i < 32; i++)
    {
        SithPlayer* pPlayer = &jkPlayer_playerInfos[i];
        if ((pPlayer->flags & 1) && pPlayer->playerNetId && pPlayer->playerNetId != sithNet_serverNetId)
            jkPlaylist_aWaitIds[jkPlaylist_numWaitIds++] = (int32_t)pPlayer->playerNetId;
    }
    stdPlatform_Printf("OpenJKDF2: the playlist goes on to %s:%s (%d of %d)\n", pNext->gob, pNext->jkl, idx + 1,
                       pNext->count);
    jkPlaylist_js_hostNext(pNext->gob, JKRES_GOB_EXT);
    return 1;
}

void jkPlaylist_Named(const jkPlaylistNext* pNext)
{
    if (sithNet_isServer)
        return;
    jkPlaylist_next = *pNext;
    jkPlaylist_next.bNamed = 1;
    jkPlaylist_next.gob[JKPLAYLIST_GOB_LEN - 1] = 0;
    jkPlaylist_next.jkl[JKPLAYLIST_JKL_LEN - 1] = 0;
    jkPlaylist_next.title[JKPLAYLIST_TITLE_LEN - 1] = 0;
    jkPlaylist_wFailed[0] = 0;
    if (jkPlaylist_next.bEnd)
    {
        stdPlatform_Printf("OpenJKDF2: the host's playlist is over\n");
        return;
    }
    stdPlatform_Printf("OpenJKDF2: the host goes on to %s:%s (%d of %d)\n", jkPlaylist_next.gob, jkPlaylist_next.jkl,
                       jkPlaylist_next.idx + 1, jkPlaylist_next.count);
    jkPlaylist_js_fetchNext(jkPlaylist_next.gob, JKRES_GOB_EXT);
}

// ---- The score screen

// The host: players still getting the next level, and the least of how far
// they are (thousandths, -1 not known).
static int jkPlaylist_Waiting(int* pLeast)
{
    int32_t least = -1;
    int n = jkPlaylist_numWaitIds ? jkPlaylist_js_waiting(jkPlaylist_aWaitIds, jkPlaylist_numWaitIds, &least) : 0;
    if (pLeast)
        *pLeast = least;
    return n;
}

int jkPlaylist_TallyMayEnd(uint32_t msElapsed, uint32_t msDelay, int bAsked)
{
    if (!jkPlaylist_next.bNamed || jkPlaylist_next.bEnd)
        return bAsked || msElapsed > msDelay;
    if (sithNet_isServer)
    {
        // The host goes on when it asks, else after the usual wait once nobody
        // is still getting the level, or after JKPLAYLIST_WAIT_MS at the most.
        if (bAsked)
            return 1;
        if (msElapsed <= msDelay)
            return 0;
        return msElapsed > JKPLAYLIST_WAIT_MS || !jkPlaylist_Waiting(NULL);
    }
    // A joiner goes on once it has the level (or couldn't get it).
    if (jkPlaylist_js_nextState() == 0)
        return 0;
    return bAsked || msElapsed > msDelay;
}

static int jkPlaylist_Seconds(uint32_t msLeft)
{
    return (int)((msLeft + 999) / 1000);
}

int jkPlaylist_TallyText(uint32_t msElapsed, uint32_t msDelay, char16_t* pLine1, char16_t* pLine2, int len)
{
    pLine1[0] = 0;
    pLine2[0] = 0;
    if (!jkPlaylist_next.bNamed)
        return 0;
    if (jkPlaylist_next.bEnd)
    {
        jk_snwprintf(pLine1, len, u"That was the playlist's last level.");
        return 1;
    }
    jk_snwprintf(pLine1, len, u"Up next: %s (%d of %d)", jkPlaylist_next.title, jkPlaylist_next.idx + 1,
                 jkPlaylist_next.count);

    if (sithNet_isServer)
    {
        int least = -1;
        int n = jkPlaylist_Waiting(&least);
        uint32_t until = msDelay;
        if (n && msElapsed >= msDelay)
            until = JKPLAYLIST_WAIT_MS;
        int secs = jkPlaylist_Seconds(until > msElapsed ? until - msElapsed : 0);
        if (!n)
            jk_snwprintf(pLine2, len, u"Next level in %d", secs);
        else if (msElapsed < msDelay)
            jk_snwprintf(pLine2, len, u"%d %s getting it. Next level in %d", n, n == 1 ? u"player is" : u"players are", secs);
        else if (least >= 0)
            jk_snwprintf(pLine2, len, u"Waiting for %d %s (%d%%), %d s at most", n, n == 1 ? u"player" : u"players",
                         least / 10, secs);
        else
            jk_snwprintf(pLine2, len, u"Waiting for %d %s, %d s at most", n, n == 1 ? u"player" : u"players", secs);
        return 1;
    }

    int state = jkPlaylist_js_nextState();
    int secs = jkPlaylist_Seconds(msDelay > msElapsed ? msDelay - msElapsed : 0);
    if (state == 2)
    {
        jkPlaylist_js_nextError(jkPlaylist_wFailed, sizeof(jkPlaylist_wFailed) / sizeof(char16_t));
        jk_snwprintf(pLine2, len, u"Couldn't get it: %s", jkPlaylist_wFailed[0] ? jkPlaylist_wFailed : u"it failed");
    }
    else if (state == 0)
    {
        int progress = jkPlaylist_js_nextProgress();
        if (progress >= 0)
            jk_snwprintf(pLine2, len, u"Getting it: %d%%", progress / 10);
        else
            jk_snwprintf(pLine2, len, u"Getting it...");
    }
    else
    {
        jk_snwprintf(pLine2, len, u"Next level in %d", secs);
    }
    return 1;
}

const char16_t* jkPlaylist_FailedText(void)
{
    if (sithNet_isServer || !jkPlaylist_next.bNamed || jkPlaylist_next.bEnd || jkPlaylist_js_nextState() != 2)
        return u"";
    jkPlaylist_js_nextError(jkPlaylist_wFailed, sizeof(jkPlaylist_wFailed) / sizeof(char16_t));
    return jkPlaylist_wFailed;
}

#endif // PLATFORM_WSRELAY
