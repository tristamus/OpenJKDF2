#include "jkGUIPlaylist.h"

// Added: the Playlist screen (jkGUIPlaylist.h), drawn like the Host screen
// it opens from and the Level Library: its background, its fonts, its lists
// and checkboxes in the small font, and its three buttons along the bottom.
// The left half picks levels as the Host screen does (an episode, then one of
// its levels), the right half is the playlist in the order it plays.

#ifdef PLATFORM_WSRELAY

#include <string.h>

#include "General/stdString.h"
#include "General/stdFont.h"
#include "General/Darray.h"
#include "Engine/rdMaterial.h" // TODO move tVBuffer
#include "stdPlatform.h"
#include "jk.h"
#include "Gui/jkGUIRend.h"
#include "Gui/jkGUI.h"
#include "Gui/jkGUINetHost.h"
#include "Gui/jkGUILibrary.h"
#include "Gui/jkGUISingleplayer.h"
#include "Main/jkPlaylist.h"
#include "Main/jkRes.h"
#include "Main/jkEpisode.h"
#include "Main/jkStrings.h"

enum jkGuiPlaylistButton_t
{
    PL_CANCEL = -1,
    PL_USE = 1,
    PL_LIBRARY = 3, // Level Library...: the menu closes for the library, then opens again
};

enum jkGuiPlaylistElement_t
{
    PL_HINT = 0,
    PL_TITLE = 1,
    PL_INTRO = 2,
    PL_EPISODE_LABEL = 3,
    PL_LIBRARY_BUTTON = 4,
    PL_EPISODE_LIST = 5,
    PL_LEVEL_LABEL = 6,
    PL_ADDALL_BUTTON = 7,
    PL_LEVEL_LIST = 8,
    PL_ADD_BUTTON = 9,
    PL_NAME_LABEL = 10,
    PL_NAME = 11,
    PL_LIST_LABEL = 12,
    PL_COUNT = 13,
    PL_LIST = 14,
    PL_UP_BUTTON = 15,
    PL_DOWN_BUTTON = 16,
    PL_REMOVE_BUTTON = 17,
    PL_LOOP = 18,
    PL_INFO = 19,
    PL_MESSAGE = 20,
    PL_CANCEL_BUTTON = 21,
    PL_CLEAR_BUTTON = 22,
    PL_USE_BUTTON = 23,
};

// The longest name the box takes: the Host screen shows it in its episode
// list, after "Playlist: ".
#define PL_NAME_MAX (24)

static int32_t jkGuiPlaylist_listBitmaps[2] = {JKGUI_BM_UP_15, JKGUI_BM_DOWN_15};

static int jkGuiPlaylist_EpisodeClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiPlaylist_LevelClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiPlaylist_ListClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiPlaylist_AddClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiPlaylist_AddAllClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiPlaylist_MoveClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiPlaylist_RemoveClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiPlaylist_LoopClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiPlaylist_ClearClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);

// An element that's hovered is the first one under the mouse, so the labels
// end before the buttons on their lines. They're short: the game's "Choose
// Episode:" doesn't fit before Level Library... in the left half.
static jkGuiElement jkGuiPlaylist_aElements[] =
{
    { ELEMENT_TEXT,       0, 0, NULL, 3, { 0, 410, 640, 20 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 6, u"Playlist", 3, { 20, 20, 600, 40 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, u"Up to 6 levels of one kind, played one after another.", 3, { 20, 58, 600, 16 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    // Left: the episodes and their levels, as on the Host screen.
    { ELEMENT_TEXT,       0, 0, u"Episodes:", 2, { 20, 82, 115, 20 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, PL_LIBRARY, 2, "GUIEXT_LEVEL_LIBRARY", 3, { 140, 82, 160, 20 }, 1, 0, u"Get a community level from the Level Library and add it here.", jkGuiNetHost_RightButtonDraw, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_LISTBOX,    0, 0, NULL, 0, { 20, 106, 280, 120 }, 1, 0, NULL, NULL, jkGuiPlaylist_EpisodeClicked, jkGuiPlaylist_listBitmaps, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, u"Levels:", 2, { 20, 232, 115, 20 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Add All", 3, { 140, 232, 160, 20 }, 1, 0, u"Adds every level of this episode, in its order.", jkGuiNetHost_RightButtonDraw, jkGuiPlaylist_AddAllClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_LISTBOX,    0, 0, NULL, 0, { 20, 256, 280, 106 }, 1, 0, u"Double-click a level to add it.", NULL, jkGuiPlaylist_LevelClicked, jkGuiPlaylist_listBitmaps, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Add Level", 3, { 140, 366, 160, 20 }, 1, 0, u"Adds the level to the end of the playlist.", jkGuiNetHost_RightButtonDraw, jkGuiPlaylist_AddClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    // Right: the playlist.
    { ELEMENT_TEXT,       0, 0, u"Name:", 2, { 320, 82, 58, 20 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBOX,    0, 0, NULL, PL_NAME_MAX, { 380, 82, 250, 20 }, 1, 0, u"The playlist's name, which the Host screen shows.", NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, u"Levels, in order:", 2, { 320, 108, 190, 20 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, NULL, 2, { 520, 108, 110, 20 }, 1, 0, NULL, jkGuiNetHost_RightButtonDraw, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_LISTBOX,    0, 0, NULL, 0, { 320, 130, 310, 120 }, 1, 0, NULL, NULL, jkGuiPlaylist_ListClicked, jkGuiPlaylist_listBitmaps, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Move Up", 2, { 320, 256, 100, 20 }, 1, 0, u"The level plays one place sooner.", NULL, jkGuiPlaylist_MoveClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Move Down", 2, { 424, 256, 112, 20 }, 1, 0, u"The level plays one place later.", NULL, jkGuiPlaylist_MoveClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Remove", 3, { 540, 256, 90, 20 }, 1, 0, u"Takes the level out of the playlist.", jkGuiNetHost_RightButtonDraw, jkGuiPlaylist_RemoveClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_CHECKBOX,   0, 0, u"Loop: start over after the last level", 0, { 320, 284, 310, 22 }, 1, 0, u"Without it the game ends after the last level's score screen.", NULL, jkGuiPlaylist_LoopClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, NULL, 0, { 320, 312, 310, 72 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, NULL, 3, { 20, 390, 600, 18 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, PL_CANCEL, 2, "GUI_CANCEL", 3, { 20, 430, 200, 40 }, 1, 0, u"Back to the Host screen, with the playlist as it was.", NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Clear", 3, { 220, 430, 200, 40 }, 1, 0, u"Takes every level out.", NULL, jkGuiPlaylist_ClearClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, PL_USE, 2, u"Use Playlist", 3, { 420, 430, 200, 40 }, 1, 0, u"Saves the playlist in this browser and picks it on the Host screen.", NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_END,        0, 0, NULL, 0, { 0, 0, 0, 0 }, 0, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
};

static jkGuiMenu jkGuiPlaylist_menu =
{
    jkGuiPlaylist_aElements, 0, 65535, 65535, 15, NULL, NULL, jkGui_stdBitmaps, jkGui_stdFonts, 0, NULL, "thermloop01.wav", "thrmlpu2.wav", NULL, NULL, NULL, 0, NULL, NULL
};

// The playlist as it's being changed: jkPlaylist_list takes it on Use
// Playlist.
static jkPlaylist jkGuiPlaylist_work;
static int jkGuiPlaylist_sel = 0;
static int jkGuiPlaylist_typeMask = 0;
static int jkGuiPlaylist_bMots = 0;

static Darray jkGuiPlaylist_episodes;
static Darray jkGuiPlaylist_levels;
static Darray jkGuiPlaylist_rows;

static char16_t jkGuiPlaylist_name[JKPLAYLIST_NAME_LEN];
static char16_t jkGuiPlaylist_count[32];
static char16_t jkGuiPlaylist_info[256];
static char16_t jkGuiPlaylist_message[160];

// The episodes the game itself comes with: everyone who can play it has
// them (packaging/wasm/maps.js, RETAIL_EPISODES).
static const char* jkGuiPlaylist_aRetail[] = {
    "jk1", "jk1ctf", "jk1mp", "jk1demo", "jk1mpdem", "jk1mpdemo",
    "jkm", "jkm_kfy", "jkm_mp", "jkm_saber", NULL
};

static int jkGuiPlaylist_IsRetail(const char* pGob)
{
    for (int i = 0; jkGuiPlaylist_aRetail[i]; i++)
    {
        if (!__strnicmp(pGob, jkGuiPlaylist_aRetail[i], JKPLAYLIST_GOB_LEN) && strlen(pGob) == strlen(jkGuiPlaylist_aRetail[i]))
            return 1;
    }
    return 0;
}

// The episode's title, as the episode lists show it, or NULL when this game
// doesn't have it.
static const char16_t* jkGuiPlaylist_EpisodeTitle(const char* pGob)
{
    for (int i = 0; i < jkEpisode_var2; i++)
    {
        if (!__strnicmp(jkEpisode_aEpisodes[i].name, pGob, sizeof(jkEpisode_aEpisodes[i].name)))
            return jkEpisode_aEpisodes[i].unistr;
    }
    return NULL;
}

static void jkGuiPlaylist_SetMessage(jkGuiMenu* pMenu, const char16_t* pText)
{
    stdString_SafeWStrCopy(jkGuiPlaylist_message, pText ? pText : u"", sizeof(jkGuiPlaylist_message) / sizeof(char16_t));
    jkGuiPlaylist_aElements[PL_MESSAGE].wstr = jkGuiPlaylist_message;
    if (pMenu)
        jkGuiRend_UpdateAndDrawClickable(&jkGuiPlaylist_aElements[PL_MESSAGE], pMenu, 1);
}

// ---- The left half: episodes and their levels

// The list's entries, not counting the empty one that ends it.
static int jkGuiPlaylist_NumEntries(Darray* pArray)
{
    return pArray->total > 0 ? (int)pArray->total - 1 : 0;
}

static jkEpisode* jkGuiPlaylist_SelectedEpisode(void)
{
    jkGuiElement* pList = &jkGuiPlaylist_aElements[PL_EPISODE_LIST];
    int i = pList->selectedTextEntry;
    if (i < 0 || i >= jkGuiPlaylist_NumEntries(&jkGuiPlaylist_episodes))
        return NULL;
    jkGuiStringEntry* pEntry = jkGuiRend_GetStringEntry(&jkGuiPlaylist_episodes, i);
    return pEntry ? (jkEpisode*)pEntry->id : NULL;
}

// A level of the level list: its title, and its file (the Host screen's
// level list keeps the file after an int).
static int jkGuiPlaylist_LevelAt(int i, const char16_t** ppTitle, const char** ppJkl)
{
    if (i < 0 || i >= jkGuiPlaylist_NumEntries(&jkGuiPlaylist_levels))
        return 0;
    jkGuiStringEntry* pEntry = jkGuiRend_GetStringEntry(&jkGuiPlaylist_levels, i);
    if (!pEntry || !pEntry->id || !pEntry->str)
        return 0;
    *ppTitle = pEntry->str;
    *ppJkl = pEntry->c_str + sizeof(int);
    return 1;
}

static void jkGuiPlaylist_FreeLevels(void)
{
    for (int i = 0; i < (int)jkGuiPlaylist_levels.total; i++)
    {
        jkGuiStringEntry* pEntry = jkGuiRend_GetStringEntry(&jkGuiPlaylist_levels, i);
        if (pEntry && pEntry->id)
        {
            JK_FREE((void*)pEntry->id);
            pEntry->id = 0;
        }
    }
    jkGuiRend_DarrayFreeEntry(&jkGuiPlaylist_levels);
}

static int jkGuiPlaylist_NumLevels(void)
{
    const char16_t* pTitle;
    const char* pJkl;
    int n = 0;
    for (int i = 0; i < jkGuiPlaylist_NumEntries(&jkGuiPlaylist_levels); i++)
        n += jkGuiPlaylist_LevelAt(i, &pTitle, &pJkl);
    return n;
}

// The levels of the episode picked on the left, as the Host screen lists
// them (jkGuiNetHost_sub_4119D0).
static void jkGuiPlaylist_LoadLevels(void)
{
    jkGuiElement* pLevels = &jkGuiPlaylist_aElements[PL_LEVEL_LIST];
    jkEpisode* pEpisode = jkGuiPlaylist_SelectedEpisode();

    if (pEpisode)
    {
        jkRes_LoadGob(pEpisode->name);
        if (jkEpisode_Load(&jkGui_episodeLoad))
        {
            // It frees the old entries' files itself.
            jkGuiSingleplayer_sub_41AA30(&jkGuiPlaylist_levels, pLevels, 0, jkRes_episodeGobName, jkGui_episodeLoad.type,
                                         jkGui_episodeLoad.numSeq, jkGui_episodeLoad.currentEpisodeEntryIdx,
                                         jkGui_episodeLoad.paEntries);
            pLevels->texInfo.textScrollY = 0;
            return;
        }
    }
    jkGuiPlaylist_FreeLevels();
    jkGuiRend_DarrayReallocStr(&jkGuiPlaylist_levels, jkStrings_GetUniStringWithFallback("GUI_NO_LEVELS_IN_EPISODE"), 0);
    jkGuiRend_DarrayReallocStr(&jkGuiPlaylist_levels, NULL, 0);
    jkGuiRend_SetClickableString(pLevels, &jkGuiPlaylist_levels);
    pLevels->selectedTextEntry = 0;
    pLevels->texInfo.textScrollY = 0;
}

static void jkGuiPlaylist_LoadEpisodes(void)
{
    jkGuiSingleplayer_EnumEpisodes(&jkGuiPlaylist_episodes, &jkGuiPlaylist_aElements[PL_EPISODE_LIST], 0, jkGuiPlaylist_typeMask);
}

// Picks the episode of this name on the left, if it's listed.
static int jkGuiPlaylist_PickEpisode(const char* pGob)
{
    jkGuiElement* pList = &jkGuiPlaylist_aElements[PL_EPISODE_LIST];
    for (int i = 0; i < jkGuiPlaylist_NumEntries(&jkGuiPlaylist_episodes); i++)
    {
        jkGuiStringEntry* pEntry = jkGuiRend_GetStringEntry(&jkGuiPlaylist_episodes, i);
        jkEpisode* pEpisode = pEntry ? (jkEpisode*)pEntry->id : NULL;
        if (pEpisode && !__strnicmp(pEpisode->name, pGob, sizeof(pEpisode->name)))
        {
            pList->selectedTextEntry = i;
            return 1;
        }
    }
    return 0;
}

// ---- The right half: the playlist

// Whether the info text fits its box, wrapped as stdFont_Draw3 wraps it: the
// box holds 5 lines, and what's past them isn't drawn. Long level and episode
// names (The Ancient Oasis Dome PC, from The Ancient Oasis Dome PC) pushed
// the end of the longest wording out of it.
static int jkGuiPlaylist_InfoFits(void)
{
    jkGuiElement* pElement = &jkGuiPlaylist_aElements[PL_INFO];
    stdFont* pFont = jkGuiPlaylist_menu.fonts ? jkGuiPlaylist_menu.fonts[pElement->textType] : NULL;
    if (!pFont)
        return 1;
    return stdFont_sub_4357C0(pFont, jkGuiPlaylist_info, &pElement->rect) <= pElement->rect.height;
}

static int jkGuiPlaylist_SameTitle(const char16_t* a, const char16_t* b)
{
    for (; *a && *a == *b; a++, b++)
        ;
    return *a == *b;
}

// The info text for the playlist's picked level, or for an empty playlist:
// the longest wording that fits the box, down to a short one that always
// does (a title is under 48 characters).
static void jkGuiPlaylist_ShowInfo(void)
{
    jkPlaylist* pList = &jkGuiPlaylist_work;
    char16_t* pOut = jkGuiPlaylist_info;
    int len = sizeof(jkGuiPlaylist_info) / sizeof(char16_t);

    if (!pList->count)
    {
        jk_snwprintf(pOut, len, u"Pick an episode and one of its levels on the left, then Add Level, or double-click the level. "
                                u"The levels play in the order they have here.");
        if (!jkGuiPlaylist_InfoFits())
            jk_snwprintf(pOut, len, u"Pick a level on the left, then Add Level. The levels play in this order.");
        return;
    }
    jkPlaylistEntry* pEntry = &pList->entries[jkGuiPlaylist_sel];
    const char16_t* pEpisode = jkGuiPlaylist_EpisodeTitle(pEntry->gob);
    const char16_t* pKind = jkPlaylist_KindName(jkPlaylist_KindOf(pList->entries[0].type));
    // "Canyon Oasis, from Jedi Training", or only "Canyon Oasis" when the
    // episode has the level's name, as a GOB of one level often has.
    char16_t from[JKPLAYLIST_TITLE_LEN + 48];
    if (pEpisode && !jkGuiPlaylist_SameTitle(pEntry->title, pEpisode))
        jk_snwprintf(from, sizeof(from) / sizeof(char16_t), u"%s, from %s", pEntry->title, pEpisode);
    else
        stdString_SafeWStrCopy(from, pEntry->title, sizeof(from) / sizeof(char16_t));

    if (!pEpisode)
    {
        jk_snwprintf(pOut, len, u"%s is in %S, which isn't in your game any more: remove it, or get it again. A %s playlist.",
                     pEntry->title, pEntry->gob, pKind);
        if (!jkGuiPlaylist_InfoFits())
            jk_snwprintf(pOut, len, u"%s isn't in your game any more: remove it, or get it again.", pEntry->title);
        if (!jkGuiPlaylist_InfoFits())
            jk_snwprintf(pOut, len, u"%s isn't in your game any more.", pEntry->title);
    }
    else if (jkGuiPlaylist_IsRetail(pEntry->gob))
    {
        jk_snwprintf(pOut, len, u"%s. Everyone has it: it comes with the game. A %s playlist.", from, pKind);
        if (!jkGuiPlaylist_InfoFits())
            jk_snwprintf(pOut, len, u"%s. Everyone has it: it comes with the game. A %s playlist.", pEntry->title, pKind);
        if (!jkGuiPlaylist_InfoFits())
            jk_snwprintf(pOut, len, u"%s. A %s playlist.", pEntry->title, pKind);
    }
    else
    {
        jk_snwprintf(pOut, len, u"%s (%S.%S). Players who don't have it get it when the game moves on to it, "
                                u"from the Level Library or from you. A %s playlist.",
                     from, pEntry->gob, JKRES_GOB_EXT, pKind);
        if (!jkGuiPlaylist_InfoFits())
            jk_snwprintf(pOut, len, u"%s (%S.%S). Players who don't have it get it when it comes up. A %s playlist.",
                         from, pEntry->gob, JKRES_GOB_EXT, pKind);
        if (!jkGuiPlaylist_InfoFits())
            jk_snwprintf(pOut, len, u"%s (%S.%S). Players who don't have it get it when it comes up. A %s playlist.",
                         pEntry->title, pEntry->gob, JKRES_GOB_EXT, pKind);
        if (!jkGuiPlaylist_InfoFits())
            jk_snwprintf(pOut, len, u"%s (%S.%S). A %s playlist.", pEntry->title, pEntry->gob, JKRES_GOB_EXT, pKind);
        if (!jkGuiPlaylist_InfoFits())
            jk_snwprintf(pOut, len, u"%s. A %s playlist.", pEntry->title, pKind);
    }
}

static void jkGuiPlaylist_SetVisible(jkGuiMenu* pMenu, int idx, int bVisible)
{
    jkGuiElement* pElement = &jkGuiPlaylist_aElements[idx];
    // A button that goes away gives the focus to the playlist first: drawing
    // it hidden looks for where the focus goes next.
    if (pMenu && !bVisible && pMenu->focusedElement == pElement)
        pMenu->focusedElement = &jkGuiPlaylist_aElements[PL_LIST];
    if (pMenu)
        jkGuiRend_SetVisibleAndDraw(pElement, pMenu, bVisible);
    else
        pElement->bIsVisible = bVisible;
}

// Shows the playlist as it is now, and the buttons that do something.
static void jkGuiPlaylist_Refresh(jkGuiMenu* pMenu)
{
    jkGuiElement* el = jkGuiPlaylist_aElements;
    jkPlaylist* pList = &jkGuiPlaylist_work;
    char16_t line[JKPLAYLIST_TITLE_LEN + 16];

    if (jkGuiPlaylist_sel >= pList->count)
        jkGuiPlaylist_sel = pList->count - 1;
    if (jkGuiPlaylist_sel < 0)
        jkGuiPlaylist_sel = 0;

    jkGuiRend_DarrayFreeEntry(&jkGuiPlaylist_rows);
    if (!pList->count)
        jkGuiRend_DarrayReallocStr(&jkGuiPlaylist_rows, u"Add levels from the left.", 0);
    for (int i = 0; i < pList->count; i++)
    {
        jk_snwprintf(line, sizeof(line) / sizeof(char16_t), u"%d. %s", i + 1, pList->entries[i].title);
        jkGuiRend_DarrayReallocStr(&jkGuiPlaylist_rows, line, i + 1);
    }
    jkGuiRend_DarrayReallocStr(&jkGuiPlaylist_rows, NULL, 0);
    jkGuiRend_SetClickableString(&el[PL_LIST], &jkGuiPlaylist_rows);
    el[PL_LIST].selectedTextEntry = jkGuiPlaylist_sel;

    jk_snwprintf(jkGuiPlaylist_count, sizeof(jkGuiPlaylist_count) / sizeof(char16_t), u"%d of %d", pList->count, JKPLAYLIST_MAX);
    el[PL_COUNT].wstr = jkGuiPlaylist_count;
    jkGuiPlaylist_ShowInfo();
    el[PL_INFO].wstr = jkGuiPlaylist_info;
    el[PL_LOOP].selectedTextEntry = !!pList->bLoop;

    int numLevels = jkGuiPlaylist_NumLevels();
    jkGuiPlaylist_SetVisible(pMenu, PL_ADD_BUTTON, numLevels > 0);
    jkGuiPlaylist_SetVisible(pMenu, PL_ADDALL_BUTTON, numLevels > 1);
    jkGuiPlaylist_SetVisible(pMenu, PL_UP_BUTTON, pList->count > 1 && jkGuiPlaylist_sel > 0);
    jkGuiPlaylist_SetVisible(pMenu, PL_DOWN_BUTTON, pList->count > 1 && jkGuiPlaylist_sel < pList->count - 1);
    jkGuiPlaylist_SetVisible(pMenu, PL_REMOVE_BUTTON, pList->count > 0);
    jkGuiPlaylist_SetVisible(pMenu, PL_CLEAR_BUTTON, pList->count > 0);

    // A button that went away gives the focus to the playlist: the menu
    // system, drawing it hidden, would otherwise look for where the focus
    // goes next forever.
    if (pMenu && pMenu->focusedElement && !pMenu->focusedElement->bIsVisible)
        pMenu->focusedElement = &el[PL_LIST];
    if (pMenu)
        jkGuiRend_Paint(pMenu);
}

// Adds level i of the level list to the end of the playlist. 1 if it did;
// otherwise the message says why.
static int jkGuiPlaylist_Add(jkGuiMenu* pMenu, int i)
{
    jkPlaylist* pList = &jkGuiPlaylist_work;
    jkEpisode* pEpisode = jkGuiPlaylist_SelectedEpisode();
    const char16_t* pTitle;
    const char* pJkl;
    char16_t msg[160];

    if (!pEpisode || !jkGuiPlaylist_LevelAt(i, &pTitle, &pJkl))
        return 0;
    if (pList->count >= JKPLAYLIST_MAX)
    {
        jkGuiPlaylist_SetMessage(pMenu, u"A playlist holds up to 6 levels. Remove one to add another.");
        return 0;
    }
    int kind = jkPlaylist_KindOf(pEpisode->type);
    if (pList->count > 0)
    {
        int listKind = jkPlaylist_KindOf(pList->entries[0].type);
        if (kind != listKind)
        {
            jk_snwprintf(msg, sizeof(msg) / sizeof(char16_t), u"This playlist is %s, and %s is %s: a playlist keeps to one kind of game.",
                         jkPlaylist_KindName(listKind), pEpisode->unistr, jkPlaylist_KindName(kind));
            jkGuiPlaylist_SetMessage(pMenu, msg);
            return 0;
        }
    }
    if (strlen(pEpisode->name) >= JKPLAYLIST_GOB_LEN || strlen(pJkl) >= JKPLAYLIST_JKL_LEN)
    {
        jkGuiPlaylist_SetMessage(pMenu, u"That level's file name is too long for a playlist.");
        return 0;
    }

    jkPlaylistEntry* pEntry = &pList->entries[pList->count];
    memset(pEntry, 0, sizeof(*pEntry));
    stdString_SafeStrCopy(pEntry->gob, pEpisode->name, sizeof(pEntry->gob));
    stdString_SafeStrCopy(pEntry->jkl, pJkl, sizeof(pEntry->jkl));
    stdString_SafeWStrCopy(pEntry->title, pTitle, JKPLAYLIST_TITLE_LEN);
    pEntry->type = pEpisode->type;
    jkGuiPlaylist_sel = pList->count++;
    return 1;
}

// ---- Clicks

static void jkGuiPlaylist_ClickSound(jkGuiMenu* pMenu)
{
    jkGuiRend_PlayWav(pMenu->soundClick);
}

static int jkGuiPlaylist_EpisodeClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    if (mouseX != -1 || mouseY != -1)
        jkGuiRend_ClickSound(pElement, pMenu, mouseX, mouseY, redraw);
    jkGuiPlaylist_LoadLevels();
    jkGuiPlaylist_SetMessage(NULL, NULL);
    jkGuiPlaylist_Refresh(pMenu);
    return 0;
}

static int jkGuiPlaylist_LevelClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    if (mouseX != -1 || mouseY != -1)
        jkGuiRend_ClickSound(pElement, pMenu, mouseX, mouseY, redraw);
    // A double click adds it.
    if (redraw)
    {
        jkGuiPlaylist_SetMessage(NULL, NULL);
        jkGuiPlaylist_Add(pMenu, pElement->selectedTextEntry);
        jkGuiPlaylist_Refresh(pMenu);
    }
    return 0;
}

static int jkGuiPlaylist_ListClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    if (mouseX != -1 || mouseY != -1)
        jkGuiRend_ClickSound(pElement, pMenu, mouseX, mouseY, redraw);
    jkGuiPlaylist_sel = pElement->selectedTextEntry;
    jkGuiPlaylist_Refresh(pMenu);
    return 0;
}

static int jkGuiPlaylist_AddClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    jkGuiPlaylist_ClickSound(pMenu);
    jkGuiPlaylist_SetMessage(NULL, NULL);
    jkGuiPlaylist_Add(pMenu, jkGuiPlaylist_aElements[PL_LEVEL_LIST].selectedTextEntry);
    jkGuiPlaylist_Refresh(pMenu);
    return 0;
}

static int jkGuiPlaylist_AddAllClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    char16_t msg[160];
    int added = 0;

    jkGuiPlaylist_ClickSound(pMenu);
    jkGuiPlaylist_SetMessage(NULL, NULL);
    for (int i = 0; i < jkGuiPlaylist_NumEntries(&jkGuiPlaylist_levels); i++)
    {
        const char16_t* pTitle;
        const char* pJkl;
        if (!jkGuiPlaylist_LevelAt(i, &pTitle, &pJkl))
            continue;
        if (!jkGuiPlaylist_Add(NULL, i))
        {
            // The message says why (the playlist is full, or of another kind).
            if (jkGuiPlaylist_work.count >= JKPLAYLIST_MAX && added)
            {
                int left = 0;
                for (int k = i; k < jkGuiPlaylist_NumEntries(&jkGuiPlaylist_levels); k++)
                    left += jkGuiPlaylist_LevelAt(k, &pTitle, &pJkl);
                jk_snwprintf(msg, sizeof(msg) / sizeof(char16_t), u"Added %d of %d: a playlist holds up to 6 levels.", added, added + left);
                jkGuiPlaylist_SetMessage(NULL, msg);
            }
            break;
        }
        added++;
    }
    jkGuiPlaylist_Refresh(pMenu);
    return 0;
}

static int jkGuiPlaylist_MoveClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    jkPlaylist* pList = &jkGuiPlaylist_work;
    int from = jkGuiPlaylist_sel;
    int to = from + (pElement == &jkGuiPlaylist_aElements[PL_UP_BUTTON] ? -1 : 1);

    jkGuiPlaylist_ClickSound(pMenu);
    if (from < 0 || from >= pList->count || to < 0 || to >= pList->count)
        return 0;
    jkPlaylistEntry tmp = pList->entries[to];
    pList->entries[to] = pList->entries[from];
    pList->entries[from] = tmp;
    jkGuiPlaylist_sel = to;
    jkGuiPlaylist_SetMessage(NULL, NULL);
    jkGuiPlaylist_Refresh(pMenu);
    return 0;
}

static int jkGuiPlaylist_RemoveClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    jkPlaylist* pList = &jkGuiPlaylist_work;
    int i = jkGuiPlaylist_sel;

    jkGuiPlaylist_ClickSound(pMenu);
    if (i < 0 || i >= pList->count)
        return 0;
    memmove(&pList->entries[i], &pList->entries[i + 1], sizeof(jkPlaylistEntry) * (pList->count - i - 1));
    pList->count--;
    memset(&pList->entries[pList->count], 0, sizeof(jkPlaylistEntry));
    jkGuiPlaylist_SetMessage(NULL, NULL);
    jkGuiPlaylist_Refresh(pMenu);
    return 0;
}

static int jkGuiPlaylist_LoopClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    jkGuiRend_DrawClickableAndUpdatebool(pElement, pMenu, mouseX, mouseY, redraw);
    jkGuiPlaylist_work.bLoop = !!pElement->selectedTextEntry;
    return 0;
}

static int jkGuiPlaylist_ClearClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    jkGuiPlaylist_ClickSound(pMenu);
    jkGuiPlaylist_work.count = 0;
    memset(jkGuiPlaylist_work.entries, 0, sizeof(jkGuiPlaylist_work.entries));
    jkGuiPlaylist_sel = 0;
    jkGuiPlaylist_SetMessage(NULL, u"Use Playlist with no levels in it goes back to hosting one episode.");
    jkGuiPlaylist_Refresh(pMenu);
    return 0;
}

// Level Library...: the library, then the level it installed picked on the
// left, and added when it's the episode's only level.
static void jkGuiPlaylist_OpenLibrary(void)
{
    char name[32];
    char16_t msg[160];

    if (!jkGuiLibrary_ShowToAdd(jkGuiPlaylist_bMots, name, sizeof(name)))
        return;
    jkGuiPlaylist_LoadEpisodes();
    if (!jkGuiPlaylist_PickEpisode(name))
    {
        jkGuiPlaylist_SetMessage(NULL, u"The Level Library saved the level, but it isn't a kind this screen lists.");
        jkGuiPlaylist_LoadLevels();
        return;
    }
    jkGuiPlaylist_LoadLevels();
    jkEpisode* pEpisode = jkGuiPlaylist_SelectedEpisode();
    int n = jkGuiPlaylist_NumLevels();
    if (n == 1)
    {
        const char16_t* pTitle;
        const char* pJkl;
        for (int i = 0; i < jkGuiPlaylist_NumEntries(&jkGuiPlaylist_levels); i++)
        {
            if (!jkGuiPlaylist_LevelAt(i, &pTitle, &pJkl))
                continue;
            if (jkGuiPlaylist_Add(NULL, i))
            {
                jk_snwprintf(msg, sizeof(msg) / sizeof(char16_t), u"Added %s.", pTitle);
                jkGuiPlaylist_SetMessage(NULL, msg);
            }
            break;
        }
    }
    else if (n > 1 && pEpisode)
    {
        jk_snwprintf(msg, sizeof(msg) / sizeof(char16_t), u"%s has %d levels: add the ones you want, or Add All.", pEpisode->unistr, n);
        jkGuiPlaylist_SetMessage(NULL, msg);
    }
}

// ---- The screen

int jkGuiPlaylist_Show(int bMots, int typeMask)
{
    jkGuiElement* el = jkGuiPlaylist_aElements;
    int result;

    jkGuiPlaylist_bMots = bMots;
    jkGuiPlaylist_typeMask = typeMask;
    jkGuiPlaylist_work = jkPlaylist_list;
    jkGuiPlaylist_sel = 0;
    stdString_SafeWStrCopy(jkGuiPlaylist_name, jkGuiPlaylist_work.name, PL_NAME_MAX + 1);

    jkGuiRend_DarrayNewStr(&jkGuiPlaylist_episodes, jkEpisode_var2 + 1, 1);
    jkGuiRend_DarrayNewStr(&jkGuiPlaylist_levels, 16, 1);
    jkGuiRend_DarrayNewStr(&jkGuiPlaylist_rows, JKPLAYLIST_MAX + 2, 1);

    jkGui_InitMenu(&jkGuiPlaylist_menu, jkGui_stdBitmaps[JKGUI_BM_BK_MULTI]);
    el[PL_NAME].wstr = jkGuiPlaylist_name;
    el[PL_NAME].selectedTextEntry = PL_NAME_MAX;
    el[PL_LIBRARY_BUTTON].bIsVisible = jkGuiLibrary_IsAvailable();
    jkGuiPlaylist_SetMessage(NULL, NULL);

    // The left half starts at the playlist's last level's episode, or the
    // first one listed.
    jkGuiPlaylist_LoadEpisodes();
    el[PL_EPISODE_LIST].selectedTextEntry = 0;
    if (jkGuiPlaylist_work.count)
        jkGuiPlaylist_PickEpisode(jkGuiPlaylist_work.entries[jkGuiPlaylist_work.count - 1].gob);
    jkGuiPlaylist_LoadLevels();
    jkGuiPlaylist_Refresh(NULL);

    // The Host screen's menu mode and palette are already up.
    jkGuiRend_MenuSetReturnKeyShortcutElement(&jkGuiPlaylist_menu, &el[PL_USE_BUTTON]);
    jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiPlaylist_menu, &el[PL_CANCEL_BUTTON]);
    for (;;)
    {
        result = jkGuiRend_DisplayAndReturnClicked(&jkGuiPlaylist_menu);
        if (result != PL_LIBRARY)
            break;
        jkGuiPlaylist_SetMessage(NULL, NULL);
        jkGuiPlaylist_OpenLibrary();
        jkGuiPlaylist_Refresh(NULL);
    }

    if (result == PL_USE)
    {
        stdString_SafeWStrCopy(jkGuiPlaylist_work.name, jkGuiPlaylist_name, JKPLAYLIST_NAME_LEN);
        jkPlaylist_list = jkGuiPlaylist_work;
        jkPlaylist_Save(bMots);
    }

    // The lists' strings are the arrays' own; the elements forget them.
    jkGuiRend_DarrayFree(&jkGuiPlaylist_episodes);
    jkGuiPlaylist_FreeLevels();
    jkGuiRend_DarrayFree(&jkGuiPlaylist_levels);
    jkGuiRend_DarrayFree(&jkGuiPlaylist_rows);
    el[PL_EPISODE_LIST].unistr = NULL;
    el[PL_LEVEL_LIST].unistr = NULL;
    el[PL_LIST].unistr = NULL;

    return result == PL_USE;
}

#endif // PLATFORM_WSRELAY
