// Tests for level playlists without the game: the playlist itself
// (src/Main/jkPlaylist.c: what's kept in the browser, which level comes
// next, what the score screen says and how long it waits) and the Playlist
// screen (src/Gui/jkGUIPlaylist.c), built for the browser as the engine is,
// on stand-ins for the game's menu system and episodes (below) and for the
// page (playlist-test-mock.js: localStorage and maps.js's next level).
// From the top of the source tree, after the engine has been built once
// (for build_emcc/generated):
//
//   sh packaging/wasm/test/playlist-test.sh

#include "types.h"
#include "globals.h"
#include "Gui/jkGUIRend.h"
#include "Gui/jkGUI.h"
#include "General/stdFont.h"
#include "Main/jkPlaylist.h"
#include "Gui/jkGUIPlaylist.h"
#include "wprintf.h"

#include <emscripten.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The element numbers in jkGUIPlaylist.c.
enum
{
    E_HINT, E_TITLE, E_INTRO, E_EPISODE_LABEL, E_LIBRARY, E_EPISODES, E_LEVEL_LABEL, E_ADDALL, E_LEVELS, E_ADD,
    E_NAME_LABEL, E_NAME, E_LIST_LABEL, E_COUNT, E_LIST, E_UP, E_DOWN, E_REMOVE, E_LOOP, E_INFO, E_MESSAGE,
    E_CANCEL, E_CLEAR, E_USE, E_END
};

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

// ---- What the game has that the code uses

int32_t sithNet_isMulti = 0;
int32_t sithNet_isServer = 0;
int32_t sithNet_serverNetId = 1;
SithPlayer jkPlayer_playerInfos[JKPLAYER_NUM_INFOS];
uint32_t jkPlayer_maxPlayers = 32;
int stdComm_Imperial_bActive = 0;
int32_t Main_bMotsCompat = 0;
jkEpisode jkEpisode_aEpisodes[JKEPISODE_MAX];
uint32_t jkEpisode_var2 = 0;
jkEpisodeLoad jkGui_episodeLoad;
char jkRes_episodeGobName[32];

static stdFont fonts[16];
stdFont* jkGui_stdFonts[16];
stdBitmap* jkGui_stdBitmaps[0x27];

static int allocs = 0;
static void* testAlloc(uint32_t n) { allocs++; return malloc(n); }
static void testFree(void* p) { if (p) allocs--; free(p); }
static HostServices hs;
HostServices* pHS = &hs;

static int verbose = 0;
int stdPlatform_Printf(const char *fmt, ...)
{
    if (!verbose)
        return 0;
    va_list args;
    va_start(args, fmt);
    int n = vprintf(fmt, args);
    va_end(args);
    return n;
}

int jk_snwprintf(char16_t *a1, size_t a2, const char16_t *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vsnwprintf_(a1, a2, fmt, args);
    va_end(args);
    return ret;
}

int __strnicmp(const char *a, const char *b, size_t c)
{
    int ca = 0, cb = 0;
    for (size_t n = 0; n < c; n++)
    {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca)
            break;
    }
    return ca - cb;
}

int _iswspace(int a)
{
    return a == ' ' || (a >= 9 && a <= 13);
}

static int wlen(const char16_t* s)
{
    int n = 0;
    while (s && s[n])
        n++;
    return n;
}

char* stdString_SafeStrCopy(char* pDst, const char* pSrc, uint32_t lenDst)
{
    strncpy(pDst, pSrc, lenDst - 1);
    pDst[lenDst - 1] = 0;
    return pDst;
}

char16_t* stdString_SafeWStrCopy(char16_t* pDst, const char16_t* pSrc, uint32_t lenDst)
{
    uint32_t i = 0;
    for (; i + 1 < lenDst && pSrc[i]; i++)
        pDst[i] = pSrc[i];
    pDst[i] = 0;
    return pDst;
}

int stdString_CharToWchar(char16_t *pwString, const char *pString, int maxChars)
{
    int i = 0;
    for (; i < maxChars && pString[i]; i++)
        pwString[i] = (unsigned char)pString[i];
    if (i < maxChars)
        pwString[i] = 0;
    return i;
}

char16_t* jkStrings_GetUniStringWithFallback(const char* key)
{
    if (!strcmp(key, "GUI_NO_LEVELS_IN_EPISODE"))
        return u"No levels in this episode";
    return u"?";
}

// ---- Text

static void toAscii(char* out, int outLen, const char16_t* s)
{
    int n = 0;
    for (; s && *s && n < outLen - 1; s++)
        out[n++] = *s < 128 ? (char)*s : '?';
    out[n] = 0;
}

static int wis(const char16_t* s, const char* want)
{
    if (!s)
        return 0;
    for (; *want; want++, s++)
    {
        if (*s != (unsigned char)*want)
            return 0;
    }
    return *s == 0;
}

static int wcontains(const char16_t* s, const char* want)
{
    char buf[1024];
    toAscii(buf, sizeof(buf), s);
    return strstr(buf, want) != NULL;
}

static char shown[1024];
static const char* show(const char16_t* s)
{
    toAscii(shown, sizeof(shown), s ? s : u"(null)");
    return shown;
}

static int js(const char* code)
{
    return emscripten_run_script_int(code);
}

static const char* jsText(const char* code)
{
    return emscripten_run_script_string(code);
}

// ---- Episodes: made up, with their levels, as jkEpisode_aEpisodes and the
// level lists have them.

typedef struct
{
    const char* name;
    const char16_t* title;
    int type;
    int numLevels;
    const char* jkl[8];
    const char16_t* lvTitle[8];
} Fixture;

static Fixture fixtures[16] = {
    { "JK1MP", u"Jedi Training", JK_EPISODE_DEATHMATCH, 6,
      { "m2.jkl", "m4.jkl", "m5.jkl", "m10.jkl", "m_boss15.jkl", "m_boss17.jkl" },
      { u"Canyon Oasis", u"Bespin Mining", u"Jabba's Palace", u"Courtyard", u"Lava Pit", u"Execution Platform" } },
    { "JK1CTF", u"Capture the Flag", JK_EPISODE_SPECIAL_CTF | JK_EPISODE_DEATHMATCH, 3,
      { "c1.jkl", "c2.jkl", "c3.jkl" }, { u"The Hideout", u"Spaceport", u"Dark Temple" } },
    { "dm_canyon", u"Big Canyon", JK_EPISODE_DEATHMATCH, 1, { "canyon.jkl" }, { u"The Big Canyon" } },
    { "JK1", u"Dark Forces II", JK_EPISODE_SINGLEPLAYER, 2, { "01narshadda.jkl", "02narshadda.jkl" },
      { u"Nar Shaddaa", u"Nar Shaddaa 2" } },
    { "dm_empty", u"Nothing In It", JK_EPISODE_DEATHMATCH, 0, { 0 }, { 0 } },
};
static int numFixtures = 5;

static void setEpisodes(void)
{
    memset(jkEpisode_aEpisodes, 0, sizeof(jkEpisode_aEpisodes));
    jkEpisode_var2 = numFixtures;
    for (int i = 0; i < numFixtures; i++)
    {
        strcpy(jkEpisode_aEpisodes[i].name, fixtures[i].name);
        stdString_SafeWStrCopy(jkEpisode_aEpisodes[i].unistr, fixtures[i].title, 32);
        jkEpisode_aEpisodes[i].type = fixtures[i].type;
    }
}

static Fixture* fixtureOf(const char* name)
{
    for (int i = 0; i < numFixtures; i++)
    {
        if (!strcmp(fixtures[i].name, name))
            return &fixtures[i];
    }
    return NULL;
}

static int gobLoads = 0;
void jkRes_LoadGob(char *a1)
{
    gobLoads++;
    stdString_SafeStrCopy(jkRes_episodeGobName, a1, sizeof(jkRes_episodeGobName));
}

int jkEpisode_Load(jkEpisodeLoad* a1)
{
    Fixture* f = fixtureOf(jkRes_episodeGobName);
    if (!f || !f->numLevels)
        return 0;
    a1->type = f->type;
    a1->numSeq = f->numLevels;
    return 1;
}

int jkEpisode_LoadVerify(void)
{
    return 1;
}

// ---- Lists, as the game keeps them (jkGUIRend.c): an array of entries,
// the strings copied in, a NULL one last.

int32_t jkGuiRend_DarrayNewStr(Darray *array, int32_t num, int32_t initVal)
{
    array->alloc = calloc(num > 0 ? num : 1, sizeof(jkGuiStringEntry));
    array->entrySize = sizeof(jkGuiStringEntry);
    array->size = num > 0 ? num : 1;
    array->total = 0;
    array->bInitialized = initVal;
    return 1;
}

int32_t jkGuiRend_DarrayReallocStr(Darray *array, char16_t *wStr, intptr_t id)
{
    if ((uint32_t)array->total >= array->size)
    {
        array->size *= 2;
        array->alloc = realloc(array->alloc, array->size * sizeof(jkGuiStringEntry));
    }
    jkGuiStringEntry* e = &((jkGuiStringEntry*)array->alloc)[array->total++];
    if (wStr && array->bInitialized)
    {
        char16_t* copy = malloc(sizeof(char16_t) * (wlen(wStr) + 1));
        memcpy(copy, wStr, sizeof(char16_t) * (wlen(wStr) + 1));
        wStr = copy;
    }
    e->str = wStr;
    e->id = id;
    return 1;
}

void jkGuiRend_DarrayFreeEntry(Darray *array)
{
    for (int i = 0; i < (int)array->total; i++)
    {
        if (array->bInitialized)
            free(((jkGuiStringEntry*)array->alloc)[i].str);
    }
    array->total = 0;
}

void jkGuiRend_DarrayFree(Darray *array)
{
    jkGuiRend_DarrayFreeEntry(array);
    free(array->alloc);
    array->alloc = NULL;
    array->size = 0;
}

void jkGuiRend_SetClickableString(jkGuiElement *element, Darray *array)
{
    element->unistr = (jkGuiStringEntry*)array->alloc;
}

jkGuiStringEntry* jkGuiRend_GetStringEntry(Darray *array, int32_t idx)
{
    return &((jkGuiStringEntry*)array->alloc)[idx];
}

static int rowCount(jkGuiElement* el)
{
    int n = 0;
    while (el->unistr && el->unistr[n].str)
        n++;
    return n;
}

static const char16_t* row(jkGuiElement* el, int i)
{
    return i < rowCount(el) ? el->unistr[i].str : NULL;
}

// The episode list and level list as the Host screen fills them
// (jkGUISingleplayer.c): the episodes of the types asked for, each entry's id
// the episode; the levels with their files after an int, in ids the list's
// owner frees.
int jkGuiSingleplayer_EnumEpisodes(Darray *array, jkGuiElement *element, int a3, jkEpisodeTypeFlags_t typeMask)
{
    int ret = 0;
    jkGuiRend_DarrayFreeEntry(array);
    for (int i = 0; i < (int)jkEpisode_var2; i++)
    {
        if (jkEpisode_aEpisodes[i].type & typeMask)
        {
            jkGuiRend_DarrayReallocStr(array, jkEpisode_aEpisodes[i].unistr, (intptr_t)&jkEpisode_aEpisodes[i]);
            ++ret;
        }
    }
    jkGuiRend_DarrayReallocStr(array, 0, 0);
    jkGuiRend_SetClickableString(element, array);
    return ret;
}

void jkGuiSingleplayer_sub_41AA30(Darray *array, jkGuiElement *element, int a3, char *episodeDir, int a5, int a6, int a7, jkEpisodeEntry* a8)
{
    for (int i = 0; i < (int)array->total; i++)
    {
        jkGuiStringEntry* e = jkGuiRend_GetStringEntry(array, i);
        if (e->id)
        {
            pHS->free((void*)e->id);
            e->id = 0;
        }
    }
    jkGuiRend_DarrayFreeEntry(array);
    Fixture* f = fixtureOf(episodeDir);
    for (int i = 0; f && i < f->numLevels; i++)
    {
        char* id = pHS->alloc(sizeof(int) + strlen(f->jkl[i]) + 1);
        *(int*)id = 0;
        strcpy(id + sizeof(int), f->jkl[i]);
        jkGuiRend_DarrayReallocStr(array, (char16_t*)f->lvTitle[i], (intptr_t)id);
    }
    jkGuiRend_DarrayReallocStr(array, 0, 0);
    jkGuiRend_SetClickableString(element, array);
    element->selectedTextEntry = a3;
}

// ---- The Level Library, as the Playlist screen opens it

static jkGuiMenu* displayedMenu(void);

static int libraryAvailable = 1;
static int libraryShows = 0;
static const char* libraryGives[4]; // what it "installs" each time it's opened, NULL for Cancel

int jkGuiLibrary_IsAvailable()
{
    return libraryAvailable;
}

int jkGuiLibrary_ShowToAdd(int bMots, char* pOutName, int outLen)
{
    CHECK(!displayedMenu(), "the Playlist screen is closed while the library is up");
    const char* gives = libraryShows < 4 ? libraryGives[libraryShows] : NULL;
    libraryShows++;
    if (!gives)
        return 0;
    stdString_SafeStrCopy(pOutName, gives, outLen);
    return 1;
}

// ---- The menu system: drawing is counted, the focus followed.

static jkGuiMenu* displayed = NULL;
static jkGuiMenu* displayedMenu(void) { return displayed; }
static int draws = 0;
static int paints = 0;
static int sounds = 0;
static int focusRunaway = 0;

void jkGui_InitMenu(jkGuiMenu *menu, stdBitmap *bgBitmap)
{
    // As the game's: the strings and hints back to the table's, string table
    // keys looked up, the rest kept as they are.
    for (jkGuiElement* el = menu->paElements; el->type != ELEMENT_END; el++)
    {
        el->hintText = el->origHintText;
        el->str = el->origStr;
        if (el->origStr && !strcmp(el->origStr, "GUI_CANCEL"))
            el->wstr = u"Cancel";
        else if (el->origStr && !strcmp(el->origStr, "GUI_CHOOSEEPISODE"))
            el->wstr = u"Choose Episode:";
        else if (el->origStr && !strcmp(el->origStr, "GUI_CHOOSELEVEL"))
            el->wstr = u"Choose Level:";
        else if (el->origStr && !strcmp(el->origStr, "GUIEXT_LEVEL_LIBRARY"))
            el->wstr = u"Level Library...";
    }
}

void jkGuiRend_MenuSetReturnKeyShortcutElement(jkGuiMenu *menu, jkGuiElement *element) { menu->pReturnKeyShortcutElement = element; }
void jkGuiRend_MenuSetEscapeKeyShortcutElement(jkGuiMenu *menu, jkGuiElement *element) { menu->pEscapeKeyShortcutElement = element; }

static void drawElement(jkGuiElement* el, jkGuiMenu* menu)
{
    CHECK(menu == displayed && menu, "draws only while the menu is up");
    draws++;
    // Drawing a hidden element with the focus on a hidden one drew the
    // focused one again forever in the game (library-menu-test.c).
    if (!el->bIsVisible && menu->focusedElement && !menu->focusedElement->bIsVisible)
        focusRunaway = 1;
}

void jkGuiRend_UpdateAndDrawClickable(jkGuiElement *clickable, jkGuiMenu *menu, BOOL forceRedraw)
{
    drawElement(clickable, menu);
}

// The height of a text wrapped in a box, as stdFont_Draw3 wraps it, for a
// made-up font of 9 pixels a letter and 14 a line: 34 letters to a line in
// the info box (310 wide), and 5 lines in its 72. The game's small font fits
// about 36 to a line there.
#define FONT_LETTER 9
#define FONT_LINE 14
static int textMeasures = 0;
static int textLines(const char16_t* s, int width)
{
    int perLine = width / FONT_LETTER, lines = 0, col = 0;
    while (s && *s)
    {
        while (*s == ' ')
            s++;
        int w = 0;
        while (s[w] && s[w] != ' ')
            w++;
        if (!w)
            break;
        if (!lines || col + 1 + w > perLine)
        {
            lines++;
            col = w;
        }
        else
            col += 1 + w;
        s += w;
    }
    return lines ? lines : 1;
}

int stdFont_sub_4357C0(stdFont *a1, const char16_t *a2, rdRect *a4)
{
    CHECK(a1 && a1 == jkGui_stdFonts[0], "measured with the box's font");
    textMeasures++;
    return textLines(a2, a4->width) * FONT_LINE;
}

void jkGuiRend_SetVisibleAndDraw(jkGuiElement *clickable, jkGuiMenu *menu, int32_t bVisible)
{
    clickable->bIsVisible = bVisible;
    drawElement(clickable, menu);
}

void jkGuiRend_Paint(jkGuiMenu *menu)
{
    CHECK(menu == displayed && menu, "paints only while the menu is up");
    paints++;
    for (jkGuiElement* el = menu->paElements; el->type != ELEMENT_END; el++)
        drawElement(el, menu);
    // The info text: never past its box's last line.
    jkGuiElement* pInfo = &menu->paElements[E_INFO];
    CHECK(textLines(pInfo->wstr, pInfo->rect.width) * FONT_LINE <= pInfo->rect.height, "the info fits its box: %s",
          show(pInfo->wstr));
}

void jkGuiNetHost_RightButtonDraw(jkGuiElement *pElement, jkGuiMenu *pMenu, tVBuffer *pVbuf, BOOL redraw)
{
}

int jkGuiRend_ClickSound(jkGuiElement *element, jkGuiMenu *menu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    sounds++;
    return 0;
}

void jkGuiRend_PlayWav(char *fpath)
{
    sounds++;
}

int jkGuiRend_DrawClickableAndUpdatebool(jkGuiElement *element, jkGuiMenu *menu, int32_t a, int32_t b, BOOL c)
{
    element->selectedTextEntry = element->selectedTextEntry == 0;
    drawElement(element, menu);
    return 0;
}

// The test's part of each frame.
typedef void (*script_t)(jkGuiMenu* menu, jkGuiElement* el, int frame);
static script_t script = NULL;
static int displays = 0;

int32_t jkGuiRend_DisplayAndReturnClicked(jkGuiMenu *menu)
{
    CHECK(!displayed, "one menu up at a time");
    displayed = menu;
    displays++;
    menu->lastClicked = 0;
    menu->focusedElement = NULL;
    menu->lastMouseOverClickable = NULL;
    jkGuiRend_Paint(menu);
    int frame = 0;
    for (; frame < 200 && !menu->lastClicked; frame++)
    {
        if (menu->idkFunc)
            menu->idkFunc(menu);
        if (!menu->lastClicked && script)
            script(menu, menu->paElements, frame);
    }
    CHECK(frame < 200, "the menu closed (frame %d)", frame);
    displayed = NULL;
    return menu->lastClicked;
}

// A click, as jkGuiRend_InvokeClicked makes one: the element's handler, or
// for a button without one, its number (jkGuiRend_PlayClickSound).
static void click(jkGuiMenu* menu, jkGuiElement* el, int redraw)
{
    CHECK(el->bIsVisible, "clicks only what's shown");
    if (!el->bIsVisible || el->enableHover)
        return;
    if (el->clickHandlerFunc)
        menu->lastClicked = el->clickHandlerFunc(el, menu, 100, 100, redraw);
    else if (el->type == ELEMENT_TEXTBUTTON || el->type == ELEMENT_PICBUTTON)
        menu->lastClicked = el->hoverId;
    else if (el->type == ELEMENT_CHECKBOX)
        jkGuiRend_DrawClickableAndUpdatebool(el, menu, 100, 100, redraw);
}

// Picks row i of a list, as a click on it does.
static void pickRow(jkGuiMenu* menu, jkGuiElement* el, int i, int bDouble)
{
    el->selectedTextEntry = i;
    click(menu, el, bDouble);
}

static int pickEpisode(jkGuiMenu* menu, jkGuiElement* el, const char* title)
{
    for (int i = 0; i < rowCount(&el[E_EPISODES]); i++)
    {
        if (wis(row(&el[E_EPISODES], i), title))
        {
            pickRow(menu, &el[E_EPISODES], i, 0);
            return 1;
        }
    }
    CHECK(0, "episode %s is listed", title);
    return 0;
}

static int pickLevel(jkGuiMenu* menu, jkGuiElement* el, const char* title, int bDouble)
{
    for (int i = 0; i < rowCount(&el[E_LEVELS]); i++)
    {
        if (wis(row(&el[E_LEVELS], i), title))
        {
            pickRow(menu, &el[E_LEVELS], i, bDouble);
            return 1;
        }
    }
    CHECK(0, "level %s is listed", title);
    return 0;
}

// ---- The playlist (src/Main/jkPlaylist.c)

static void setPlaylist(int n, int bLoop)
{
    static const char* gobs[] = { "JK1MP", "dm_canyon", "JK1MP", "JK1MP", "JK1MP", "JK1MP" };
    static const char* jkls[] = { "m2.jkl", "canyon.jkl", "m4.jkl", "m5.jkl", "m10.jkl", "m_boss15.jkl" };
    static const char16_t* titles[] = { u"Canyon Oasis", u"The Big Canyon", u"Bespin Mining", u"Jabba's Palace",
                                        u"Courtyard", u"Lava Pit" };
    memset(&jkPlaylist_list, 0, sizeof(jkPlaylist_list));
    stdString_SafeWStrCopy(jkPlaylist_list.name, u"Friday Night", JKPLAYLIST_NAME_LEN);
    jkPlaylist_list.bLoop = bLoop;
    jkPlaylist_list.count = n;
    for (int i = 0; i < n; i++)
    {
        strcpy(jkPlaylist_list.entries[i].gob, gobs[i]);
        strcpy(jkPlaylist_list.entries[i].jkl, jkls[i]);
        stdString_SafeWStrCopy(jkPlaylist_list.entries[i].title, titles[i], JKPLAYLIST_TITLE_LEN);
        jkPlaylist_list.entries[i].type = JK_EPISODE_DEATHMATCH;
    }
}

static void setPlayers(int n)
{
    memset(jkPlayer_playerInfos, 0, sizeof(jkPlayer_playerInfos));
    for (int i = 0; i < n; i++)
    {
        jkPlayer_playerInfos[i].flags = 1;
        jkPlayer_playerInfos[i].playerNetId = i + 1; // the host is 1
    }
}

static void asHost(void)
{
    sithNet_isMulti = 1;
    sithNet_isServer = 1;
    stdComm_Imperial_bActive = 0;
}

static void asJoiner(void)
{
    sithNet_isMulti = 1;
    sithNet_isServer = 0;
    stdComm_Imperial_bActive = 0;
}

static void resetMock(void)
{
    js("var M = Module.playlistMock; M.calls = []; M.state = 1; M.progress = -1; M.error = ''; "
       "M.waiting = { count: 0, least: -1 }; M.waitingAsked = []; 0");
}

static void testSaved(void)
{
    js("localStorage.clear(), 0");
    jkPlaylist_Load(0);
    CHECK(jkPlaylist_list.count == 0, "nothing saved: no levels (%d)", jkPlaylist_list.count);
    CHECK(jkPlaylist_list.bLoop == 1, "nothing saved: it loops");
    CHECK(jkPlaylist_list.name[0] == 0, "nothing saved: no name");

    setPlaylist(3, 0);
    memset(jkPlaylist_list.entries[1].title, 0, sizeof(jkPlaylist_list.entries[1].title));
    stdString_SafeWStrCopy(jkPlaylist_list.entries[1].title, u"Café “Noir”", JKPLAYLIST_TITLE_LEN);
    jkPlaylist_Save(0);
    CHECK(js("localStorage.getItem('jkdf2mp-playlist-jk1') !== null ? 1 : 0"), "saved under JK's key");
    CHECK(js("localStorage.getItem('jkdf2mp-playlist-mots') === null ? 1 : 0"), "not under MotS's");
    CHECK(!strcmp(jsText("JSON.stringify(JSON.parse(localStorage.getItem('jkdf2mp-playlist-jk1')).items.map(i => [i.gob, i.jkl, i.type]))"),
                  "[[\"JK1MP\",\"m2.jkl\",2],[\"dm_canyon\",\"canyon.jkl\",2],[\"JK1MP\",\"m4.jkl\",2]]"),
          "what's saved: %s", jsText("localStorage.getItem('jkdf2mp-playlist-jk1')"));
    jkPlaylist saved = jkPlaylist_list;
    memset(&jkPlaylist_list, 0, sizeof(jkPlaylist_list));
    jkPlaylist_Load(0);
    CHECK(!memcmp(&saved, &jkPlaylist_list, sizeof(saved)), "loads back as it was saved");
    CHECK(jkPlaylist_list.entries[1].title[3] == 0xe9 && jkPlaylist_list.entries[1].title[5] == 0x201c,
          "a title's letters outside ASCII come back");
    jkPlaylist_Load(1);
    CHECK(jkPlaylist_list.count == 0, "MotS keeps its own");

    // What's kept is checked: too many levels, ones without a file, odd file
    // names, a list that isn't one.
    js("localStorage.setItem('jkdf2mp-playlist-jk1', JSON.stringify({ v: 1, name: 'X'.repeat(80), loop: 0, items: ["
       "{ gob: 'a', jkl: 'a.jkl', title: 'A', type: 2 }, { gob: '', jkl: 'b.jkl' }, { gob: 'c', jkl: '' },"
       "{ gob: 'd\\u00e9', jkl: 'd.jkl' }, { gob: 'e', jkl: 'e.jkl' }, { gob: 'f', jkl: 'f.jkl' },"
       "{ gob: 'g', jkl: 'g.jkl' }, { gob: 'h', jkl: 'h.jkl' }, { gob: 'i', jkl: 'i.jkl' } ] })), 0");
    jkPlaylist_Load(0);
    CHECK(jkPlaylist_list.count == 4, "only levels with both files, of the first 6 (%d)", jkPlaylist_list.count);
    CHECK(!strcmp(jkPlaylist_list.entries[1].gob, "d_"), "a file name is ASCII (%s)", jkPlaylist_list.entries[1].gob);
    CHECK(wis(jkPlaylist_list.entries[1].title, "d.jkl"), "a level without a title is called by its file (%s)",
          show(jkPlaylist_list.entries[1].title));
    CHECK(wlen(jkPlaylist_list.name) == JKPLAYLIST_NAME_LEN - 1, "a long name is cut (%d)", wlen(jkPlaylist_list.name));
    CHECK(jkPlaylist_list.bLoop == 0, "loop as saved");
    js("localStorage.setItem('jkdf2mp-playlist-jk1', '{not json'), 0");
    jkPlaylist_Load(0);
    CHECK(jkPlaylist_list.count == 0, "a broken one is no playlist");
    js("localStorage.setItem('jkdf2mp-playlist-jk1', JSON.stringify({ v: 2, items: [{ gob: 'a', jkl: 'a.jkl' }] })), 0");
    jkPlaylist_Load(0);
    CHECK(jkPlaylist_list.count == 0, "a later version's is left alone");
}

static void testKinds(void)
{
    CHECK(jkPlaylist_KindOf(JK_EPISODE_DEATHMATCH) == JKPLAYLIST_KIND_DM, "deathmatch");
    CHECK(jkPlaylist_KindOf(JK_EPISODE_4_UNK) == JKPLAYLIST_KIND_DM, "type 4 is deathmatch");
    CHECK(jkPlaylist_KindOf(JK_EPISODE_SPECIAL_CTF | JK_EPISODE_DEATHMATCH) == JKPLAYLIST_KIND_CTF, "CTF");
    CHECK(jkPlaylist_KindOf(JKM_EPISODE_SABER) == JKPLAYLIST_KIND_SABER, "saber");
    CHECK(jkPlaylist_KindOf(JK_EPISODE_SINGLEPLAYER) == JKPLAYLIST_KIND_COOP, "co-op");
    Main_bMotsCompat = 1;
    CHECK(wis(jkPlaylist_KindName(JKPLAYLIST_KIND_CTF), "Kill the Fool"), "MotS's CTF");
    Main_bMotsCompat = 0;
    CHECK(wis(jkPlaylist_KindName(JKPLAYLIST_KIND_CTF), "Capture the Flag"), "JK's CTF");
}

// The host goes through the playlist: 1, 2, 3, then the end or 1 again.
static void testHostOrder(void)
{
    resetMock();
    asHost();
    setPlayers(3);
    setPlaylist(3, 0);

    jkPlaylist_SetHosting(0, 0);
    jkPlaylist_GameStarted();
    CHECK(!jkPlaylist_IsHosting() && !jkPlaylist_HostPick(), "not hosting the playlist: nothing named");

    jkPlaylist_SetHosting(1, 0);
    jkPlaylist_GameStarted();
    CHECK(jkPlaylist_IsHosting(), "hosting it");
    stdComm_Imperial_bActive = 1;
    CHECK(!jkPlaylist_IsHosting(), "not on the Imperial Network Protocol");
    stdComm_Imperial_bActive = 0;
    sithNet_isServer = 0;
    CHECK(!jkPlaylist_IsHosting(), "not as a joiner");
    sithNet_isServer = 1;

    CHECK(jkPlaylist_HostPick() == 1, "names the next level");
    CHECK(jkPlaylist_next.bNamed && !jkPlaylist_next.bEnd && jkPlaylist_next.idx == 1 && jkPlaylist_next.count == 3,
          "level 2 of 3 (%d of %d)", jkPlaylist_next.idx + 1, jkPlaylist_next.count);
    CHECK(!strcmp(jkPlaylist_next.gob, "dm_canyon") && !strcmp(jkPlaylist_next.jkl, "canyon.jkl") &&
          wis(jkPlaylist_next.title, "The Big Canyon"), "its files and title");
    CHECK(!strcmp(jsText("Module.playlistMock.calls.join('|')"), "host dm_canyon.gob"), "gets it ready for players: %s",
          jsText("Module.playlistMock.calls.join('|')"));
    CHECK(jkPlaylist_HostPick() == 1 && js("Module.playlistMock.calls.length") == 1,
          "once a level: the end-of-level message goes 3 times");

    // The players it waits for: those in the game, not the host itself.
    CHECK(!jkPlaylist_TallyMayEnd(0, 30000, 0), "the score screen waits as before");
    CHECK(jkPlaylist_TallyMayEnd(30001, 30000, 0), "nobody getting it: it ends when it did");
    CHECK(!strcmp(jsText("JSON.stringify(Module.playlistMock.waitingAsked[0] || [])"), "[2,3]"),
          "waits for players 2 and 3: %s", jsText("JSON.stringify(Module.playlistMock.waitingAsked)"));

    jkPlaylist_LevelStarted();
    CHECK(!jkPlaylist_next.bNamed, "the next level begins: nothing named");
    CHECK(jkPlaylist_HostPick() && jkPlaylist_next.idx == 2 && !strcmp(jkPlaylist_next.jkl, "m4.jkl"), "then level 3");
    jkPlaylist_LevelStarted();
    CHECK(jkPlaylist_HostPick() && jkPlaylist_next.bEnd, "then the end, without Loop");
    CHECK(js("Module.playlistMock.calls.length") == 2, "nothing to get ready at the end");

    setPlaylist(3, 1);
    jkPlaylist_SetHosting(1, 2);
    jkPlaylist_GameStarted();
    CHECK(jkPlaylist_HostPick() && !jkPlaylist_next.bEnd && jkPlaylist_next.idx == 0, "with Loop, level 1 after level 3");

    // Starting with another level.
    jkPlaylist_SetHosting(1, 1);
    jkPlaylist_GameStarted();
    CHECK(jkPlaylist_HostPick() && jkPlaylist_next.idx == 2, "started with level 2: level 3 next");
    jkPlaylist_SetHosting(1, 7);
    jkPlaylist_GameStarted();
    CHECK(jkPlaylist_HostPick() && jkPlaylist_next.idx == 1, "a start past the end is level 1");

    // A one-level playlist that loops plays it again.
    setPlaylist(1, 1);
    jkPlaylist_SetHosting(1, 0);
    jkPlaylist_GameStarted();
    CHECK(jkPlaylist_HostPick() && !jkPlaylist_next.bEnd && jkPlaylist_next.idx == 0, "one level, looping");
    jkPlaylist_SetHosting(0, 0);
}

static void testHostTally(void)
{
    char16_t l1[128], l2[128];
    resetMock();
    asHost();
    setPlayers(4);
    setPlaylist(3, 1);
    jkPlaylist_SetHosting(1, 0);
    jkPlaylist_GameStarted();

    CHECK(!jkPlaylist_TallyText(0, 30000, l1, l2, 128) && !l1[0] && !l2[0], "nothing named: the score screen as before");
    CHECK(jkPlaylist_TallyMayEnd(30001, 30000, 0) && jkPlaylist_TallyMayEnd(5, 30000, 1) && !jkPlaylist_TallyMayEnd(5, 30000, 0),
          "nothing named: it ends when it did");

    jkPlaylist_HostPick();
    js("Module.playlistMock.waiting = { count: 0, least: -1 }, 0");
    CHECK(jkPlaylist_TallyText(1000, 30000, l1, l2, 128), "says what's next");
    CHECK(wis(l1, "Up next: The Big Canyon (2 of 3)"), "line 1: %s", show(l1));
    CHECK(wis(l2, "Next level in 29"), "line 2: %s", show(l2));
    CHECK(!jkPlaylist_TallyMayEnd(29999, 30000, 0) && jkPlaylist_TallyMayEnd(30001, 30000, 0), "nobody waiting: as before");
    CHECK(!strcmp(jsText("JSON.stringify(Module.playlistMock.waitingAsked[0] || [])"), "[2,3,4]"),
          "asks about players 2 to 4: %s", jsText("JSON.stringify(Module.playlistMock.waitingAsked)"));

    js("Module.playlistMock.waiting = { count: 2, least: 0.42 }, 0");
    jkPlaylist_TallyText(10000, 30000, l1, l2, 128);
    CHECK(wis(l2, "2 players are getting it. Next level in 20"), "players getting it: %s", show(l2));
    CHECK(!jkPlaylist_TallyMayEnd(30001, 30000, 0), "waits for them after the usual time");
    jkPlaylist_TallyText(40000, 30000, l1, l2, 128);
    CHECK(wis(l2, "Waiting for 2 players (42%), 20 s at most"), "how far the slowest is: %s", show(l2));
    js("Module.playlistMock.waiting = { count: 1, least: -1 }, 0");
    jkPlaylist_TallyText(40000, 30000, l1, l2, 128);
    CHECK(wis(l2, "Waiting for 1 player, 20 s at most"), "one player, not said how far: %s", show(l2));
    CHECK(!jkPlaylist_TallyMayEnd(59999, 30000, 0) && jkPlaylist_TallyMayEnd(60001, 30000, 0), "60 s at the most");
    CHECK(jkPlaylist_TallyMayEnd(100, 30000, 1), "the host's Ok goes on at once");
    js("Module.playlistMock.waiting = { count: 0, least: -1 }, 0");
    CHECK(jkPlaylist_TallyMayEnd(45000, 30000, 0), "goes on once they have it");

    // The dedicated host's score screen is up 0 ms as before; it still waits.
    js("Module.playlistMock.waiting = { count: 1, least: 0.1 }, 0");
    CHECK(!jkPlaylist_TallyMayEnd(10, 0, 0), "a dedicated host waits for players too");

    setPlaylist(1, 0);
    jkPlaylist_SetHosting(1, 0);
    jkPlaylist_GameStarted();
    jkPlaylist_HostPick();
    jkPlaylist_TallyText(1000, 30000, l1, l2, 128);
    CHECK(wis(l1, "That was the playlist's last level.") && !l2[0], "the end: %s / %s", show(l1), show(l2));
    CHECK(jkPlaylist_TallyMayEnd(30001, 30000, 0), "the end: as before");
    CHECK(jkPlaylist_FailedText()[0] == 0, "the host never failed to get it");
    jkPlaylist_SetHosting(0, 0);
}

static void testJoiner(void)
{
    char16_t l1[128], l2[128];
    jkPlaylistNext next;
    resetMock();
    asJoiner();
    jkPlaylist_GameStarted();

    memset(&next, 0, sizeof(next));
    next.idx = 3;
    next.count = 5;
    strcpy(next.gob, "dm_canyon");
    strcpy(next.jkl, "canyon.jkl");
    stdString_SafeWStrCopy(next.title, u"The Big Canyon", JKPLAYLIST_TITLE_LEN);

    js("Module.playlistMock.state = 0, Module.playlistMock.progress = -1, 0");
    jkPlaylist_Named(&next);
    CHECK(jkPlaylist_next.bNamed && jkPlaylist_next.idx == 3, "the host named it");
    CHECK(!strcmp(jsText("Module.playlistMock.calls.join('|')"), "fetch dm_canyon.gob"), "starts getting it: %s",
          jsText("Module.playlistMock.calls.join('|')"));
    jkPlaylist_TallyText(1000, 30000, l1, l2, 128);
    CHECK(wis(l1, "Up next: The Big Canyon (4 of 5)"), "line 1: %s", show(l1));
    CHECK(wis(l2, "Getting it..."), "line 2 before it knows how much: %s", show(l2));
    js("Module.playlistMock.progress = 0.375, 0");
    jkPlaylist_TallyText(1000, 30000, l1, l2, 128);
    CHECK(wis(l2, "Getting it: 37%"), "line 2 as it comes: %s", show(l2));
    CHECK(!jkPlaylist_TallyMayEnd(40000, 30000, 1), "waits for it, Ok or not");

    js("Module.playlistMock.state = 1, 0");
    jkPlaylist_TallyText(20000, 30000, l1, l2, 128);
    CHECK(wis(l2, "Next level in 10"), "has it: %s", show(l2));
    CHECK(!jkPlaylist_TallyMayEnd(20000, 30000, 0) && jkPlaylist_TallyMayEnd(20000, 30000, 1) &&
          jkPlaylist_TallyMayEnd(30001, 30000, 0), "has it: as before");
    CHECK(jkPlaylist_FailedText()[0] == 0, "nothing failed");

    js("Module.playlistMock.state = 2, Module.playlistMock.error = 'the host left.', 0");
    jkPlaylist_TallyText(20000, 30000, l1, l2, 128);
    CHECK(wis(l2, "Couldn't get it: the host left."), "couldn't: %s", show(l2));
    CHECK(wis(jkPlaylist_FailedText(), "the host left."), "why, for after the score screen: %s", show(jkPlaylist_FailedText()));
    CHECK(jkPlaylist_TallyMayEnd(30001, 30000, 0), "couldn't: the score screen still ends");

    jkPlaylist_LevelStarted();
    CHECK(!jkPlaylist_next.bNamed && jkPlaylist_FailedText()[0] == 0, "the next level begins: nothing named");

    resetMock();
    next.bEnd = 1;
    jkPlaylist_Named(&next);
    CHECK(jkPlaylist_next.bNamed && jkPlaylist_next.bEnd, "the end");
    CHECK(js("Module.playlistMock.calls.length") == 0, "nothing to get at the end");
    jkPlaylist_TallyText(1000, 30000, l1, l2, 128);
    CHECK(wis(l1, "That was the playlist's last level."), "the end: %s", show(l1));

    // Names that don't end in the host's message end here.
    jkPlaylist_GameStarted();
    CHECK(!jkPlaylist_next.bNamed, "a new game: nothing named");
    memset(next.gob, 'x', sizeof(next.gob));
    memset(next.title, 'y', sizeof(next.title));
    next.bEnd = 0;
    jkPlaylist_Named(&next);
    CHECK(strlen(jkPlaylist_next.gob) == JKPLAYLIST_GOB_LEN - 1 && wlen(jkPlaylist_next.title) == JKPLAYLIST_TITLE_LEN - 1,
          "names cut to fit");

    // The host's own game ignores what it hears.
    asHost();
    jkPlaylist_GameStarted();
    jkPlaylist_Named(&next);
    CHECK(!jkPlaylist_next.bNamed, "the host doesn't take a name from a message");
}

// ---- The Playlist screen

static int count(void)
{
    return jkPlaylist_list.count;
}

static void scriptAddAndUse(jkGuiMenu* menu, jkGuiElement* el, int frame)
{
    switch (frame)
    {
    case 0:
        CHECK(wis(el[E_COUNT].wstr, "0 of 6"), "count: %s", show(el[E_COUNT].wstr));
        CHECK(rowCount(&el[E_LIST]) == 1 && wis(row(&el[E_LIST], 0), "Add levels from the left."), "empty: %s",
              show(row(&el[E_LIST], 0)));
        CHECK(!el[E_REMOVE].bIsVisible && !el[E_UP].bIsVisible && !el[E_DOWN].bIsVisible && !el[E_CLEAR].bIsVisible,
              "nothing to move, remove or clear");
        CHECK(el[E_LIBRARY].bIsVisible, "Level Library... when the site has it");
        // Save, not Use: Clear and Cancel left players thinking the playlist was gone.
        CHECK(wis(el[E_USE].wstr, "Save Playlist") && wis(el[E_CANCEL].wHintText, "Back to the Host screen without saving."),
              "Save Playlist, and Cancel doesn't save: %s / %s", show(el[E_USE].wstr), show(el[E_CANCEL].wHintText));
        // The episodes the Host screen lists: not the single-player one.
        CHECK(rowCount(&el[E_EPISODES]) == 4, "episodes of the kinds asked for (%d)", rowCount(&el[E_EPISODES]));
        for (int i = 0; i < rowCount(&el[E_EPISODES]); i++)
            CHECK(!wis(row(&el[E_EPISODES], i), "Dark Forces II"), "no single-player episode");
        CHECK(wis(row(&el[E_EPISODES], el[E_EPISODES].selectedTextEntry), "Jedi Training"), "the first episode is picked");
        CHECK(rowCount(&el[E_LEVELS]) == 6 && wis(row(&el[E_LEVELS], 0), "Canyon Oasis"), "its levels");
        CHECK(el[E_ADD].bIsVisible && el[E_ADDALL].bIsVisible, "Add Level and Add All");
        CHECK(el[E_LOOP].selectedTextEntry == 1, "Loop as nothing saved has it");
        break;
    case 1:
        pickLevel(menu, el, "Bespin Mining", 0);
        click(menu, &el[E_ADD], 0);
        CHECK(wis(el[E_COUNT].wstr, "1 of 6"), "count: %s", show(el[E_COUNT].wstr));
        CHECK(wis(row(&el[E_LIST], 0), "1. Bespin Mining"), "row 1: %s", show(row(&el[E_LIST], 0)));
        CHECK(el[E_REMOVE].bIsVisible && el[E_CLEAR].bIsVisible && !el[E_UP].bIsVisible && !el[E_DOWN].bIsVisible,
              "one level: Remove and Clear");
        CHECK(wcontains(el[E_INFO].wstr, "Bespin Mining, from Jedi Training. Everyone has it"), "info: %s", show(el[E_INFO].wstr));
        CHECK(wcontains(el[E_INFO].wstr, "A deathmatch playlist."), "its kind: %s", show(el[E_INFO].wstr));
        break;
    case 2:
        // Another GOB: a double click adds it.
        pickEpisode(menu, el, "Big Canyon");
        CHECK(rowCount(&el[E_LEVELS]) == 1 && el[E_ADD].bIsVisible && !el[E_ADDALL].bIsVisible, "one level: no Add All");
        pickLevel(menu, el, "The Big Canyon", 0);
        CHECK(wis(el[E_COUNT].wstr, "1 of 6"), "a single click doesn't add: %s", show(el[E_COUNT].wstr));
        pickLevel(menu, el, "The Big Canyon", 1);
        CHECK(wis(row(&el[E_LIST], 1), "2. The Big Canyon"), "a double click adds: %s", show(row(&el[E_LIST], 1)));
        CHECK(el[E_LIST].selectedTextEntry == 1, "the added level is picked");
        CHECK(wcontains(el[E_INFO].wstr, "(dm_canyon.gob). Players who don't have it get it"), "where players get it: %s",
              show(el[E_INFO].wstr));
        break;
    case 3:
        // Another kind: refused.
        pickEpisode(menu, el, "Capture the Flag");
        pickLevel(menu, el, "Spaceport", 0);
        click(menu, &el[E_ADD], 0);
        CHECK(wis(el[E_COUNT].wstr, "2 of 6"), "CTF isn't added: %s", show(el[E_COUNT].wstr));
        CHECK(wcontains(el[E_MESSAGE].wstr, "This playlist is deathmatch, and Capture the Flag is Capture the Flag"),
              "says why: %s", show(el[E_MESSAGE].wstr));
        click(menu, &el[E_ADDALL], 0);
        CHECK(wis(el[E_COUNT].wstr, "2 of 6"), "nor with Add All");
        break;
    case 4:
        // An episode with no levels.
        pickEpisode(menu, el, "Nothing In It");
        CHECK(el[E_MESSAGE].wstr && !el[E_MESSAGE].wstr[0], "a new pick clears the message");
        CHECK(rowCount(&el[E_LEVELS]) == 1 && wis(row(&el[E_LEVELS], 0), "No levels in this episode"), "says so");
        CHECK(!el[E_ADD].bIsVisible && !el[E_ADDALL].bIsVisible, "nothing to add");
        break;
    case 5:
        // Add All up to 6.
        pickEpisode(menu, el, "Jedi Training");
        click(menu, &el[E_ADDALL], 0);
        CHECK(wis(el[E_COUNT].wstr, "6 of 6"), "full: %s", show(el[E_COUNT].wstr));
        CHECK(wis(el[E_MESSAGE].wstr, "Added 4 of 6: a playlist holds up to 6 levels."), "says so: %s", show(el[E_MESSAGE].wstr));
        CHECK(wis(row(&el[E_LIST], 2), "3. Canyon Oasis") && wis(row(&el[E_LIST], 5), "6. Courtyard"),
              "in the episode's order: %s", show(row(&el[E_LIST], 5)));
        pickLevel(menu, el, "Courtyard", 1);
        CHECK(wcontains(el[E_MESSAGE].wstr, "holds up to 6 levels. Remove one"), "a 7th: %s", show(el[E_MESSAGE].wstr));
        break;
    case 6:
        // Move and remove.
        pickRow(menu, &el[E_LIST], 0, 0);
        CHECK(!el[E_UP].bIsVisible && el[E_DOWN].bIsVisible, "the first: Move Down only");
        click(menu, &el[E_DOWN], 0);
        CHECK(wis(row(&el[E_LIST], 0), "1. The Big Canyon") && wis(row(&el[E_LIST], 1), "2. Bespin Mining"),
              "moved down: %s", show(row(&el[E_LIST], 1)));
        CHECK(el[E_LIST].selectedTextEntry == 1 && el[E_UP].bIsVisible && el[E_DOWN].bIsVisible, "it stays picked");
        click(menu, &el[E_UP], 0);
        CHECK(wis(row(&el[E_LIST], 0), "1. Bespin Mining"), "and up again");
        pickRow(menu, &el[E_LIST], 5, 0);
        CHECK(el[E_UP].bIsVisible && !el[E_DOWN].bIsVisible, "the last: Move Up only");
        // The focus on a button that goes away goes to the playlist.
        menu->focusedElement = &el[E_DOWN];
        pickRow(menu, &el[E_LIST], 5, 0);
        CHECK(menu->focusedElement == &el[E_LIST], "focus off a hidden button");
        menu->focusedElement = &el[E_REMOVE];
        click(menu, &el[E_REMOVE], 0);
        CHECK(wis(el[E_COUNT].wstr, "5 of 6") && el[E_LIST].selectedTextEntry == 4, "removed the last; the new last picked");
        pickRow(menu, &el[E_LIST], 1, 0);
        click(menu, &el[E_REMOVE], 0);
        CHECK(wis(row(&el[E_LIST], 0), "1. Bespin Mining") && wis(row(&el[E_LIST], 1), "2. Canyon Oasis"),
              "removed the 2nd: %s / %s", show(row(&el[E_LIST], 0)), show(row(&el[E_LIST], 1)));
        CHECK(el[E_LIST].selectedTextEntry == 1, "the next one picked");
        CHECK(count() == 0, "nothing saved before Save Playlist");
        break;
    case 7:
        click(menu, &el[E_LOOP], 0);
        CHECK(el[E_LOOP].selectedTextEntry == 0, "Loop off");
        stdString_SafeWStrCopy((char16_t*)el[E_NAME].wstr, u"Friday Night", 25);
        CHECK(el[E_NAME].selectedTextEntry == 24, "the name box takes 24 letters");
        CHECK(menu->pReturnKeyShortcutElement == &el[E_USE] && menu->pEscapeKeyShortcutElement == &el[E_CANCEL],
              "Return is Save Playlist, Escape Cancel");
        click(menu, &el[E_USE], 0);
        break;
    }
}

static void scriptCancel(jkGuiMenu* menu, jkGuiElement* el, int frame)
{
    switch (frame)
    {
    case 0:
        CHECK(wis(el[E_NAME].wstr, "Friday Night"), "the saved name: %s", show(el[E_NAME].wstr));
        CHECK(rowCount(&el[E_LIST]) == 4 && el[E_LOOP].selectedTextEntry == 0, "the saved playlist");
        // The left half starts at its last level's episode.
        CHECK(wis(row(&el[E_EPISODES], el[E_EPISODES].selectedTextEntry), "Jedi Training"), "its last level's episode: %s",
              show(row(&el[E_EPISODES], el[E_EPISODES].selectedTextEntry)));
        click(menu, &el[E_CLEAR], 0);
        CHECK(rowCount(&el[E_LIST]) == 1 && wis(el[E_COUNT].wstr, "0 of 6"), "cleared");
        CHECK(wcontains(el[E_MESSAGE].wstr, "Save Playlist with no levels in it takes the playlist off the Host screen."),
              "says what Save does now: %s", show(el[E_MESSAGE].wstr));
        break;
    case 1:
        click(menu, &el[E_CANCEL], 0);
        break;
    }
}

static int libraryStep = 0;
static void scriptLibrary(jkGuiMenu* menu, jkGuiElement* el, int frame)
{
    if (frame != 0)
        return;
    switch (libraryStep++)
    {
    case 0:
        click(menu, &el[E_LIBRARY], 0); // gives a one-level episode
        break;
    case 1:
        CHECK(wis(el[E_MESSAGE].wstr, "Added Hoth Base."), "a one-level episode is added: %s", show(el[E_MESSAGE].wstr));
        CHECK(wis(row(&el[E_LIST], rowCount(&el[E_LIST]) - 1), "5. Hoth Base"), "at the end: %s",
              show(row(&el[E_LIST], rowCount(&el[E_LIST]) - 1)));
        CHECK(wis(row(&el[E_EPISODES], el[E_EPISODES].selectedTextEntry), "Hoth"), "and picked on the left");
        click(menu, &el[E_LIBRARY], 0); // gives a pack
        break;
    case 2:
        CHECK(wis(el[E_MESSAGE].wstr, "Pack of Three has 3 levels: add the ones you want, or Add All."), "a pack: %s",
              show(el[E_MESSAGE].wstr));
        CHECK(rowCount(&el[E_LIST]) == 5, "nothing added from a pack");
        CHECK(rowCount(&el[E_LEVELS]) == 3, "its levels are listed");
        click(menu, &el[E_LIBRARY], 0); // Cancel in the library
        break;
    case 3:
        CHECK(el[E_MESSAGE].wstr && !el[E_MESSAGE].wstr[0], "nothing to say after the library's Cancel");
        click(menu, &el[E_USE], 0);
        break;
    }
}

static void scriptGone(jkGuiMenu* menu, jkGuiElement* el, int frame)
{
    if (frame != 0)
        return;
    CHECK(!el[E_LIBRARY].bIsVisible, "no Level Library... without the site's");
    pickRow(menu, &el[E_LIST], 4, 0);
    CHECK(wcontains(el[E_INFO].wstr, "Hoth Base is in gone, which isn't in your game any more"), "a level whose episode is gone: %s",
          show(el[E_INFO].wstr));
    click(menu, &el[E_CANCEL], 0);
}

static void scriptMots(jkGuiMenu* menu, jkGuiElement* el, int frame)
{
    switch (frame)
    {
    case 0:
        pickEpisode(menu, el, "Duel Pits");
        click(menu, &el[E_ADD], 0);
        CHECK(wcontains(el[E_INFO].wstr, "(duel.goo)") && wcontains(el[E_INFO].wstr, "A saber only playlist."),
              "MotS's GOO, and the kind: %s", show(el[E_INFO].wstr));
        pickEpisode(menu, el, "Jedi Training");
        click(menu, &el[E_ADD], 0);
        CHECK(wcontains(el[E_MESSAGE].wstr, "This playlist is saber only, and Jedi Training is deathmatch"), "one kind: %s",
              show(el[E_MESSAGE].wstr));
        pickEpisode(menu, el, "Saber Arenas");
        click(menu, &el[E_ADD], 0);
        CHECK(wcontains(el[E_INFO].wstr, "Arena, from Saber Arenas. Everyone has it"), "MotS's own episode: %s", show(el[E_INFO].wstr));
        break;
    case 1:
        click(menu, &el[E_USE], 0);
        break;
    }
}

// Long names: the info text drops what doesn't fit its box, down to the
// level's title and the playlist's kind.
#define LONG_GOB "dm_the_longest_gob_name_allowed"
#define LONG_EPISODE "The Very Long Episode Name Here"
#define LONG_LEVEL "The Grand Imperial Palace Of The Outer Rim Ways"
static void scriptLongNames(jkGuiMenu* menu, jkGuiElement* el, int frame)
{
    if (frame != 0)
        return;
    jkGuiElement* pInfo = &el[E_INFO];
    CHECK(wcontains(pInfo->wstr, "Pick an episode and one of its levels on the left"), "the empty playlist's info: %s",
          show(pInfo->wstr));

    // An episode named as its one level: the name once, not "X, from X".
    pickEpisode(menu, el, "The Ancient Oasis Dome PC");
    int measures = textMeasures;
    click(menu, &el[E_ADD], 0);
    CHECK(textMeasures > measures, "measured");
    CHECK(wcontains(pInfo->wstr, "The Ancient Oasis Dome PC (AODpc.gob). Players who don't have it get it when"),
          "the level, its GOB and where players get it: %s", show(pInfo->wstr));
    CHECK(!wcontains(pInfo->wstr, ", from"), "the name only once: %s", show(pInfo->wstr));
    CHECK(wcontains(pInfo->wstr, "A deathmatch playlist."), "its kind: %s", show(pInfo->wstr));
    CHECK(textLines(pInfo->wstr, pInfo->rect.width) * FONT_LINE <= pInfo->rect.height, "fits: %s", show(pInfo->wstr));

    // The longest names there can be: still the level and the kind.
    pickEpisode(menu, el, LONG_EPISODE);
    click(menu, &el[E_ADD], 0);
    CHECK(wis(row(&el[E_LIST], 1), "2. " LONG_LEVEL), "added: %s", show(row(&el[E_LIST], 1)));
    CHECK(wcontains(pInfo->wstr, LONG_LEVEL) && wcontains(pInfo->wstr, "A deathmatch playlist."), "the level and the kind: %s",
          show(pInfo->wstr));
    CHECK(textLines(pInfo->wstr, pInfo->rect.width) * FONT_LINE <= pInfo->rect.height, "fits: %s", show(pInfo->wstr));

    // The same for a level that comes with the game, and one that's gone.
    pickEpisode(menu, el, "Jedi Training");
    pickLevel(menu, el, "Execution Platform", 0);
    click(menu, &el[E_ADD], 0);
    CHECK(wcontains(pInfo->wstr, "Execution Platform, from Jedi Training. Everyone has it: it comes with the game. "
                                 "A deathmatch playlist."), "a short one's whole text: %s", show(pInfo->wstr));
    for (int i = 0; i < (int)jkEpisode_var2; i++)
    {
        if (!strcmp(jkEpisode_aEpisodes[i].name, LONG_GOB))
            strcpy(jkEpisode_aEpisodes[i].name, "gone_too");
    }
    pickRow(menu, &el[E_LIST], 1, 0);
    CHECK(wcontains(pInfo->wstr, LONG_LEVEL " isn't in your game any more"), "gone: %s", show(pInfo->wstr));
    CHECK(textLines(pInfo->wstr, pInfo->rect.width) * FONT_LINE <= pInfo->rect.height, "fits: %s", show(pInfo->wstr));

    pickRow(menu, &el[E_LIST], 0, 0);
    CHECK(wcontains(pInfo->wstr, "The Ancient Oasis Dome PC (AODpc.gob)"), "picked again: %s", show(pInfo->wstr));
    click(menu, &el[E_CANCEL], 0);
}

static void testScreen(void)
{
    const int mask = JK_EPISODE_DEATHMATCH | JK_EPISODE_4_UNK | JK_EPISODE_SPECIAL_CTF;
    int before = allocs;

    js("localStorage.clear(), 0");
    jkPlaylist_Load(0);
    script = scriptAddAndUse;
    int r = jkGuiPlaylist_Show(0, mask);
    CHECK(r == 1, "Save Playlist");
    CHECK(count() == 4, "the playlist: 4 levels (%d)", count());
    CHECK(wis(jkPlaylist_list.name, "Friday Night") && jkPlaylist_list.bLoop == 0, "its name and Loop");
    CHECK(!strcmp(jkPlaylist_list.entries[0].gob, "JK1MP") && !strcmp(jkPlaylist_list.entries[0].jkl, "m4.jkl") &&
          wis(jkPlaylist_list.entries[0].title, "Bespin Mining") && jkPlaylist_list.entries[0].type == JK_EPISODE_DEATHMATCH,
          "level 1: %s %s", jkPlaylist_list.entries[0].gob, jkPlaylist_list.entries[0].jkl);
    CHECK(!strcmp(jkPlaylist_list.entries[1].jkl, "m2.jkl") && !strcmp(jkPlaylist_list.entries[3].jkl, "m5.jkl"),
          "the rest in order: %s %s", jkPlaylist_list.entries[1].jkl, jkPlaylist_list.entries[3].jkl);
    CHECK(js("JSON.parse(localStorage.getItem('jkdf2mp-playlist-jk1')).items.length") == 4, "saved in the browser");
    CHECK(allocs == before, "the level lists' files are freed (%d left)", allocs - before);
    CHECK(displays == 1 && paints > 8 && draws > 0 && sounds > 8, "drawn and clicked (%d %d %d %d)", displays, paints, draws, sounds);
    CHECK(gobLoads > 4 && !strcmp(jkRes_episodeGobName, "JK1MP"), "each episode picked was loaded (%d, %s)", gobLoads,
          jkRes_episodeGobName);
    CHECK(!focusRunaway, "never drew with the focus on a hidden element");

    jkPlaylist saved = jkPlaylist_list;
    script = scriptCancel;
    r = jkGuiPlaylist_Show(0, mask);
    CHECK(r == 0, "Cancel");
    CHECK(!memcmp(&saved, &jkPlaylist_list, sizeof(saved)), "Cancel leaves the playlist as it was");
    CHECK(js("JSON.parse(localStorage.getItem('jkdf2mp-playlist-jk1')).items.length") == 4, "and what's saved");

    // The Level Library, with Use Level as Add to Playlist.
    fixtures[numFixtures++] = (Fixture){ "hoth", u"Hoth", JK_EPISODE_DEATHMATCH, 1, { "hoth.jkl" }, { u"Hoth Base" } };
    fixtures[numFixtures++] = (Fixture){ "pack3", u"Pack of Three", JK_EPISODE_DEATHMATCH, 3, { "p1.jkl", "p2.jkl", "p3.jkl" },
                                         { u"One", u"Two", u"Three" } };
    setEpisodes();
    libraryStep = 0;
    libraryShows = 0;
    libraryGives[0] = "hoth";
    libraryGives[1] = "pack3";
    libraryGives[2] = NULL;
    script = scriptLibrary;
    r = jkGuiPlaylist_Show(0, mask);
    CHECK(r == 1, "Save Playlist after the library");
    CHECK(libraryShows == 3, "the library opened 3 times (%d)", libraryShows);
    CHECK(displays == 6, "the screen came back after each (%d)", displays);
    CHECK(count() == 5 && !strcmp(jkPlaylist_list.entries[4].gob, "hoth") && !strcmp(jkPlaylist_list.entries[4].jkl, "hoth.jkl"),
          "the library's level is in the playlist");
    CHECK(allocs == before, "freed after the library too (%d left)", allocs - before);

    // A level whose episode is gone; no Level Library.
    strcpy(jkPlaylist_list.entries[4].gob, "gone");
    libraryAvailable = 0;
    script = scriptGone;
    r = jkGuiPlaylist_Show(0, mask);
    CHECK(r == 0, "Cancel");
    libraryAvailable = 1;

    // A MotS saber playlist: its GOO, and its kind.
    Main_bMotsCompat = 1;
    fixtures[numFixtures++] = (Fixture){ "jkm_saber", u"Saber Arenas", JKM_EPISODE_SABER, 1, { "s1.jkl" }, { u"Arena" } };
    fixtures[numFixtures++] = (Fixture){ "duel", u"Duel Pits", JKM_EPISODE_SABER, 1, { "duel.jkl" }, { u"The Pit" } };
    setEpisodes();
    js("localStorage.clear(), 0");
    jkPlaylist_Load(1);
    script = scriptMots;
    r = jkGuiPlaylist_Show(1, mask | JKM_EPISODE_SABER);
    CHECK(r == 1 && count() == 2, "a saber playlist (%d)", count());
    CHECK(js("JSON.parse(localStorage.getItem('jkdf2mp-playlist-mots')).items.length") == 2, "saved as MotS's");
    CHECK(js("JSON.parse(localStorage.getItem('jkdf2mp-playlist-jk1') || 'null') === null ? 1 : 0"), "not as JK's");
    Main_bMotsCompat = 0;

    fixtures[numFixtures++] = (Fixture){ "AODpc", u"The Ancient Oasis Dome PC", JK_EPISODE_DEATHMATCH, 1, { "aodpc.jkl" },
                                         { u"The Ancient Oasis Dome PC" } };
    fixtures[numFixtures++] = (Fixture){ LONG_GOB, u"" LONG_EPISODE, JK_EPISODE_DEATHMATCH, 1, { "long.jkl" }, { u"" LONG_LEVEL } };
    setEpisodes();
    js("localStorage.clear(), 0");
    jkPlaylist_Load(0);
    script = scriptLongNames;
    r = jkGuiPlaylist_Show(0, mask);
    CHECK(r == 0, "Cancel after the long names");
    CHECK(allocs == before, "all freed (%d left)", allocs - before);
    CHECK(!focusRunaway, "never drew with the focus on a hidden element");
}

int main(void)
{
    hs.alloc = testAlloc;
    hs.free = testFree;
    for (int i = 0; i < 16; i++)
        jkGui_stdFonts[i] = &fonts[i];
    setEpisodes();

    testSaved();
    testKinds();
    testHostOrder();
    testHostTally();
    testJoiner();
    sithNet_isMulti = 0;
    sithNet_isServer = 0;
    testScreen();

    if (failures)
        printf("%d of %d checks failed\n", failures, checks);
    else
        printf("all %d checks passed\n", checks);
    return failures != 0;
}
