#include "jkGUILibrary.h"

// Added: the Level Library as a game menu, drawn like the Host screen it opens
// from (its background, fonts, checkboxes, lists and buttons), for the browser
// build. The list, the details and the downloads are the page's
// (packaging/wasm/library.js): this menu asks it for text when something has
// changed and tells it what the player picks. The one thing the menu can't
// draw, a level's screenshot in full color, the page puts over the frame the
// menu leaves for it.

#ifdef PLATFORM_WSRELAY

#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

#include "General/stdBitmap.h"
#include "General/stdFont.h"
#include "General/stdString.h"
#include "General/Darray.h"
#include "Engine/rdMaterial.h" // TODO move tVBuffer
#include "stdPlatform.h"
#include "jk.h"
#include "Gui/jkGUIRend.h"
#include "Gui/jkGUI.h"

enum jkGuiLibraryButton_t
{
    LIB_CANCEL = -1,
    LIB_DONE = 2,
    LIB_VIEW = 3, // the 3D view's frame clicked: the big 3D view
};

enum jkGuiLibraryElement_t
{
    LIB_HINT = 0,
    LIB_TITLE = 1,
    LIB_STATUS = 2,
    LIB_SEARCH_LABEL = 3,
    LIB_SEARCH = 4,
    LIB_FILTER_MP = 5,
    LIB_FILTER_CTF = 6,
    LIB_FILTER_COOP = 7,
    LIB_SORT_LABEL = 8,
    LIB_SORT_PREV = 9,
    LIB_SORT_NEXT = 10,
    LIB_SORT_NAME = 11,
    LIB_LIST = 12,
    LIB_TAB_OVERVIEW = 13,
    LIB_TAB_README = 14,
    LIB_TAB_FILES = 15,
    LIB_SHOT = 16,
    LIB_NO_SHOT = 17,
    LIB_LEVEL_TITLE = 18,
    LIB_FACTS = 19,
    LIB_TEXT = 20,
    LIB_MESSAGE = 21,
    LIB_CANCEL_BUTTON = 22,
    LIB_PAGE_BUTTON = 23,
    LIB_USE_BUTTON = 24,
    LIB_PV = 25,
    LIB_PV_TEXT = 26,
    LIB_FILTER_MINE = 27,
};

// The big 3D view's elements.
enum jkGuiLibraryViewElement_t
{
    VIEW_HINT = 0,
    VIEW_TITLE = 1,
    VIEW_FRAME = 2,
    VIEW_TEXT = 3,
    VIEW_HELP = 4,
    VIEW_NOTE = 5,
    VIEW_BACK = 6,
    VIEW_MODE = 7,
    VIEW_NEXT = 8,
};

enum jkGuiLibraryTab_t
{
    LIB_TAB_IS_OVERVIEW = 0,
    LIB_TAB_IS_README = 1,
    LIB_TAB_IS_FILES = 2,
};

// What changed since the menu last asked (library.js, CH_*).
#define LIB_CH_STATUS (1)
#define LIB_CH_ROWS (2)
#define LIB_CH_PANEL (4)
#define LIB_CH_MSG (8)
#define LIB_CH_USE (16)
#define LIB_CH_PV (32)
#define LIB_CH_VIEW (64)
#define LIB_CH_ALL (127)

// library.js's text() and number().
#define LIB_TEXT_STATUS (0)
#define LIB_TEXT_ROW (1)
#define LIB_TEXT_SORT (2)
#define LIB_TEXT_TITLE (3)
#define LIB_TEXT_OVERVIEW (4)
#define LIB_TEXT_README (5)
#define LIB_TEXT_FILES (6)
#define LIB_TEXT_MESSAGE (7)
#define LIB_TEXT_PV (8)
#define LIB_TEXT_VIEW_TITLE (9)
#define LIB_TEXT_VIEW_HELP (10)
#define LIB_TEXT_VIEW_NOTE (11)
#define LIB_TEXT_VIEW_MODE (12)
#define LIB_TEXT_VIEW_NEXT (13)

#define LIB_NUM_CHANGED (0)
#define LIB_NUM_COUNT (1)
#define LIB_NUM_SELECTED (2)
#define LIB_NUM_CAN_USE (3)
#define LIB_NUM_BUSY (4)
#define LIB_NUM_DONE (5)
#define LIB_NUM_SHOTS (6)
// The selected level's 3D view: 0 none, 1 coming, 2 there, 3 failed.
#define LIB_NUM_PV (7)

#define LIB_NUM_SORTS (6)
#define LIB_SEARCH_MAX (31)

// Where the title column ends in the list, and where the year and size start
// (from the row's left edge, as the list draws it). The list draws what's
// after a row's last tab at the first of these stops past what's before it,
// and cuts it at the list's edge.
#define LIB_LIST_TITLE_W (144)
#define LIB_LIST_YEAR_X (150)
// The year's stop, and before it the kinds' (CTF, KFY, SP), set for each list
// by jkGuiLibrary_FillRows.
static int32_t jkGuiLibrary_listPaddings[3] = {LIB_LIST_YEAR_X, 0, 0};
// Labels in the Overview (Author:, Released:...), sizes in Files.
static int32_t jkGuiLibrary_factsPaddings[2] = {80, 0};
static int32_t jkGuiLibrary_filesPaddings[2] = {58, 0};

static int32_t jkGuiLibrary_listBitmaps[2] = {JKGUI_BM_UP_15, JKGUI_BM_DOWN_15};

static int jkGuiLibrary_FilterClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_SortClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_ListClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_TabClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_ShotClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_PreviewClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_ViewFrameClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_ViewBackClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_ViewModeClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_ViewNextClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static void jkGuiLibrary_ViewIdle(jkGuiMenu *pMenu);
static int jkGuiLibrary_CancelClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_PageClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_UseClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static void jkGuiLibrary_ShotDraw(jkGuiElement *pElement, jkGuiMenu *pMenu, tVBuffer *vbuf, BOOL redraw);
static void jkGuiLibrary_PlainListDraw(jkGuiElement *pElement, jkGuiMenu *pMenu, tVBuffer *vbuf, BOOL redraw);
static void jkGuiLibrary_Idle(jkGuiMenu *pMenu);

// The places follow the Host screen's: its title, its labels, checkboxes and
// lists in the small font, its three buttons along the
// bottom, and its hint line over them. An element that's hovered is the
// first one under the mouse, so the screenshot's frame comes before the text
// that can sit in it.
static jkGuiElement jkGuiLibrary_aElements[] =
{
    { ELEMENT_TEXT,       0, 0, NULL, 3, { 0, 410, 640, 20 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 6, u"Level Library", 3, { 20, 20, 600, 40 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, NULL, 3, { 20, 58, 600, 16 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, u"Search", 2, { 20, 78, 78, 24 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBOX,    0, 0, NULL, LIB_SEARCH_MAX, { 100, 80, 200, 20 }, 1, 0, u"Type part of a level's title or its author's name.", NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_CHECKBOX,   0, 0, u"Multiplayer", 1, { 20, 104, 138, 24 }, 1, 0, u"Show deathmatch levels.", NULL, jkGuiLibrary_FilterClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_CHECKBOX,   0, 0, u"CTF", 1, { 162, 104, 138, 24 }, 1, 0, NULL, NULL, jkGuiLibrary_FilterClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_CHECKBOX,   0, 0, u"Co-op", 0, { 20, 128, 138, 24 }, 1, 0, u"Show single-player levels. The Host screen lists them with Experimental Co-op ticked under Advanced.", NULL, jkGuiLibrary_FilterClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, u"Sort", 2, { 20, 152, 58, 30 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_PICBUTTON,  0, 0, NULL, JKGUI_BM_ARROW_LEFT, { 80, 152, 30, 30 }, 1, 0, NULL, NULL, jkGuiLibrary_SortClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_PICBUTTON,  0, 0, NULL, JKGUI_BM_ARROW_RIGHT, { 110, 152, 30, 30 }, 1, 0, NULL, NULL, jkGuiLibrary_SortClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, NULL, 2, { 146, 152, 154, 30 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_LISTBOX,    0, 0, NULL, 0, { 20, 186, 280, 200 }, 1, 0, u"Levels with * are saved in this browser. Double-click one to use it.", NULL, jkGuiLibrary_ListClicked, jkGuiLibrary_listBitmaps, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Overview", 3, { 310, 78, 104, 24 }, 1, 0, NULL, NULL, jkGuiLibrary_TabClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Readme", 3, { 418, 78, 104, 24 }, 1, 0, u"The text files in the level's zip.", NULL, jkGuiLibrary_TabClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Files", 3, { 526, 78, 104, 24 }, 1, 0, u"What's in the level's zip.", NULL, jkGuiLibrary_TabClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    // The Overview: the screenshot and the 3D view side by side (4:3 each),
    // the level's name under them, its details under that.
    { ELEMENT_CUSTOM,     0, 0, NULL, 0, { 310, 104, 156, 117 }, 1, 0, NULL, jkGuiLibrary_ShotDraw, jkGuiLibrary_ShotClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, u"No screenshot for this level", 3, { 310, 104, 156, 117 }, 0, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 2, NULL, 3, { 310, 224, 320, 20 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_LISTBOX,    0, 0, NULL, 0, { 310, 246, 320, 142 }, 1, 0, NULL, jkGuiLibrary_PlainListDraw, NULL, jkGuiLibrary_listBitmaps, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_LISTBOX,    0, 0, NULL, 0, { 310, 104, 320, 282 }, 0, 0, NULL, jkGuiLibrary_PlainListDraw, NULL, jkGuiLibrary_listBitmaps, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, NULL, 3, { 20, 390, 600, 18 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, "GUI_CANCEL", 3, { 20, 430, 200, 40 }, 1, 0, NULL, NULL, jkGuiLibrary_CancelClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Massassi Page", 3, { 220, 430, 200, 40 }, 1, 0, u"Opens the level's page on Massassi.net in a new tab.", NULL, jkGuiLibrary_PageClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Use Level", 3, { 420, 430, 200, 40 }, 1, 0, u"Gets the level, saves it in this browser and picks it on the Host screen.", NULL, jkGuiLibrary_UseClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    // After the rest so their numbers stay: the 3D view's frame (the page
    // draws the level over it, and a click on it opens the big 3D view),
    // what it says while there's no 3D view, and My levels.
    { ELEMENT_CUSTOM,     0, 0, NULL, 0, { 474, 104, 156, 117 }, 1, 0, NULL, jkGuiLibrary_ShotDraw, jkGuiLibrary_PreviewClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, NULL, 3, { 474, 104, 156, 117 }, 0, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_CHECKBOX,   0, 0, u"My levels", 0, { 162, 128, 138, 24 }, 1, 0, u"Show only the levels saved in this browser: ones you used here, or got from a host.", NULL, jkGuiLibrary_FilterClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_END,        0, 0, NULL, 0, { 0, 0, 0, 0 }, 0, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
};

static jkGuiMenu jkGuiLibrary_menu =
{
    jkGuiLibrary_aElements, 0, 65535, 65535, 15, NULL, NULL, jkGui_stdBitmaps, jkGui_stdFonts, (intptr_t)jkGuiLibrary_listPaddings, jkGuiLibrary_Idle, "thermloop01.wav", "thrmlpu2.wav", NULL, NULL, NULL, 0, NULL, NULL
};

// The big 3D view: the level's name, a wide frame the page draws the level
// over (and takes the mouse in), how to move, and Back (Escape), Walk or
// Orbit, and the next starting place or map.
static jkGuiElement jkGuiLibrary_aViewElements[] =
{
    { ELEMENT_TEXT,       0, 0, NULL, 3, { 0, 410, 640, 20 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 6, NULL, 3, { 20, 14, 600, 40 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_CUSTOM,     0, 0, NULL, 0, { 20, 58, 600, 312 }, 1, 0, NULL, jkGuiLibrary_ShotDraw, jkGuiLibrary_ViewFrameClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 2, NULL, 3, { 20, 58, 600, 312 }, 0, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, NULL, 3, { 20, 373, 600, 18 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, NULL, 3, { 20, 391, 600, 18 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Back", 3, { 20, 430, 200, 40 }, 1, 0, u"Back to the Level Library (Esc).", NULL, jkGuiLibrary_ViewBackClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, NULL, 3, { 220, 430, 200, 40 }, 0, 0, NULL, NULL, jkGuiLibrary_ViewModeClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, NULL, 3, { 420, 430, 200, 40 }, 0, 0, NULL, NULL, jkGuiLibrary_ViewNextClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_END,        0, 0, NULL, 0, { 0, 0, 0, 0 }, 0, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
};

static jkGuiMenu jkGuiLibrary_viewMenu =
{
    jkGuiLibrary_aViewElements, 0, 65535, 65535, 15, NULL, NULL, jkGui_stdBitmaps, jkGui_stdFonts, 0, jkGuiLibrary_ViewIdle, "thermloop01.wav", "thrmlpu2.wav", NULL, NULL, NULL, 0, NULL, NULL
};

// The search, filters, sort and tab stay as the player left them, for the
// next time the library opens.
static char16_t jkGuiLibrary_search[LIB_SEARCH_MAX + 1];
static char16_t jkGuiLibrary_lastSearch[LIB_SEARCH_MAX + 1];
static int jkGuiLibrary_filterMp = 1;
static int jkGuiLibrary_filterCtf = 1;
static int jkGuiLibrary_filterCoop = 0;
static int jkGuiLibrary_filterMine = 0;
static int jkGuiLibrary_sort = 0;
static int jkGuiLibrary_tab = LIB_TAB_IS_OVERVIEW;

static Darray jkGuiLibrary_rows;
static Darray jkGuiLibrary_facts;
static Darray jkGuiLibrary_text;
static int jkGuiLibrary_bArrays = 0;

// Texts the menu shows, from library.js (malloc'd).
static char16_t* jkGuiLibrary_pStatus = NULL;
static char16_t* jkGuiLibrary_pSortName = NULL;
static char16_t* jkGuiLibrary_pLevelTitle = NULL;
static char16_t* jkGuiLibrary_pMessage = NULL;
static char16_t* jkGuiLibrary_pPreviewText = NULL;
static char16_t* jkGuiLibrary_pViewTitle = NULL;
static char16_t* jkGuiLibrary_pViewText = NULL;
static char16_t* jkGuiLibrary_pViewHelp = NULL;
static char16_t* jkGuiLibrary_pViewNote = NULL;
static char16_t* jkGuiLibrary_pViewMode = NULL;
static char16_t* jkGuiLibrary_pViewNext = NULL;
// What changed while the big 3D view was up, for the library after it.
static int jkGuiLibrary_pending = 0;

// ---- library.js

EM_JS(int, jkGuiLibrary_js_available, (), {
    return Module.jkLibrary && Module.jkLibrary.available() ? 1 : 0;
});

EM_JS(int, jkGuiLibrary_js_begin, (int bMots), {
    return Module.jkLibrary && Module.jkLibrary.begin(bMots ? 'mots' : 'jk1') ? 1 : 0;
});

EM_JS(void, jkGuiLibrary_js_end, (), {
    if (Module.jkLibrary)
        Module.jkLibrary.end();
});

EM_JS(int, jkGuiLibrary_js_number, (int what), {
    return Module.jkLibrary ? (Module.jkLibrary.number(what) | 0) : 0;
});

// A text from library.js: its length first (kept for the copy that follows).
EM_JS(int, jkGuiLibrary_js_textLength, (int what, int arg), {
    var s = Module.jkLibrary ? String(Module.jkLibrary.text(what, arg)) : "";
    Module.jkLibraryLastText = s;
    return s.length;
});

EM_JS(void, jkGuiLibrary_js_textCopy, (char16_t* pOut, int outLen), {
    var s = Module.jkLibraryLastText || "";
    var n = Math.min(s.length, outLen - 1);
    for (var i = 0; i < n; i++)
        HEAPU16[(pOut >> 1) + i] = s.charCodeAt(i);
    HEAPU16[(pOut >> 1) + n] = 0;
    Module.jkLibraryLastText = "";
});

EM_JS(void, jkGuiLibrary_js_query, (const char16_t* pSearch, int filters, int sort), {
    var q = "";
    for (var i = 0; HEAPU16[(pSearch >> 1) + i]; i++)
        q += String.fromCharCode(HEAPU16[(pSearch >> 1) + i]);
    if (Module.jkLibrary)
        Module.jkLibrary.query(q, filters, sort);
});

EM_JS(void, jkGuiLibrary_js_select, (int idx), {
    if (Module.jkLibrary)
        Module.jkLibrary.select(idx);
});

EM_JS(int, jkGuiLibrary_js_use, (), {
    return Module.jkLibrary && Module.jkLibrary.use() ? 1 : 0;
});

EM_JS(int, jkGuiLibrary_js_cancel, (), {
    return Module.jkLibrary && Module.jkLibrary.cancel() ? 1 : 0;
});

EM_JS(void, jkGuiLibrary_js_page, (), {
    if (Module.jkLibrary)
        Module.jkLibrary.page();
});

EM_JS(void, jkGuiLibrary_js_shot, (int bOn, int x, int y, int w, int h), {
    if (Module.jkLibrary)
        Module.jkLibrary.shot(bOn, x, y, w, h);
});

EM_JS(void, jkGuiLibrary_js_preview, (int bOn, int x, int y, int w, int h), {
    if (Module.jkLibrary && Module.jkLibrary.preview)
        Module.jkLibrary.preview(bOn, x, y, w, h);
});

EM_JS(void, jkGuiLibrary_js_view, (int bOn, int x, int y, int w, int h), {
    if (Module.jkLibrary && Module.jkLibrary.view)
        Module.jkLibrary.view(bOn, x, y, w, h);
});

// 1: Walk or Orbit, 2: the next starting place (Walk) or map (Orbit), 3:
// try again after the 3D view didn't load.
EM_JS(void, jkGuiLibrary_js_viewCommand, (int cmd), {
    if (Module.jkLibrary && Module.jkLibrary.viewCommand)
        Module.jkLibrary.viewCommand(cmd);
});

EM_JS(void, jkGuiLibrary_js_nextShot, (), {
    if (Module.jkLibrary)
        Module.jkLibrary.nextShot();
});

EM_JS(void, jkGuiLibrary_js_take, (char* pOut, int outLen), {
    stringToUTF8(Module.jkLibrary ? Module.jkLibrary.take() : "", pOut, outLen);
});

static char16_t* jkGuiLibrary_GetText(int what, int arg)
{
    int len = jkGuiLibrary_js_textLength(what, arg);
    char16_t* pText = (char16_t*)malloc(sizeof(char16_t) * (len + 1));
    if (!pText)
        return NULL;
    jkGuiLibrary_js_textCopy(pText, len + 1);
    return pText;
}

static int jkGuiLibrary_TextWidth(stdFont* pFont, const char16_t* pText, int len);
static int jkGuiLibrary_CharWidth(stdFont* pFont, char16_t c);

// A line of text for a text element, cut with "..." to the element's width:
// the game's text drawing wraps what doesn't fit, which a one-line element
// shows cut in half.
static char16_t* jkGuiLibrary_Fit(jkGuiElement* pElement, char16_t* pText)
{
    static const char16_t dots[4] = {'.', '.', '.', 0};
    stdFont* pFont = jkGui_stdFonts[pElement->textType];
    int width = pElement->rect.width - 4;
    int len = 0;

    if (!pText || !pFont)
        return pText;
    while (pText[len])
        len++;
    if (jkGuiLibrary_TextWidth(pFont, pText, len) <= width)
        return pText;
    char16_t* pOut = (char16_t*)malloc(sizeof(char16_t) * (len + 4));
    if (!pOut)
        return pText;
    int w = jkGuiLibrary_TextWidth(pFont, dots, 3), n = 0;
    for (int i = 0; i < len; i++)
    {
        int cw = jkGuiLibrary_CharWidth(pFont, pText[i]);
        if (w + cw > width)
            break;
        w += cw;
        pOut[n++] = pText[i];
    }
    while (n > 0 && pOut[n - 1] == ' ')
        n--;
    memcpy(pOut + n, dots, sizeof(dots));
    free(pText);
    return pOut;
}

// Whether a line of text fits in a text element's width, in its font.
static int jkGuiLibrary_Fits(jkGuiElement* pElement, const char16_t* pText)
{
    stdFont* pFont = jkGui_stdFonts[pElement->textType];
    int len = 0;

    if (!pText || !pFont)
        return 1;
    while (pText[len])
        len++;
    return jkGuiLibrary_TextWidth(pFont, pText, len) <= pElement->rect.width - 4;
}

static void jkGuiLibrary_SetText(jkGuiElement* pElement, char16_t** ppKeep, char16_t* pText)
{
    pText = jkGuiLibrary_Fit(pElement, pText);
    if (*ppKeep)
        free(*ppKeep);
    *ppKeep = pText;
    pElement->wstr = pText;
}

// ---- Text that fits: lines cut to the width the lists draw in, measured as
// stdFont_Draw1 draws (each glyph's width plus the font's spacing, a space as
// the font's space width).

static int jkGuiLibrary_CharWidth(stdFont* pFont, char16_t c)
{
    char16_t tmp[2];
    if (_iswspace(c))
        return pFont->marginX;
    tmp[0] = c;
    tmp[1] = 0;
    return stdFont_sub_4355F0(pFont, tmp);
}

static int jkGuiLibrary_TextWidth(stdFont* pFont, const char16_t* pText, int len)
{
    int w = 0;
    for (int i = 0; i < len && pText[i]; i++)
        w += jkGuiLibrary_CharWidth(pFont, pText[i]);
    return w;
}

#define LIB_LINE_MAX (512)

static void jkGuiLibrary_AddLine(Darray* pArray, const char16_t* pPrefix, int prefixLen, const char16_t* pText, int len)
{
    char16_t line[LIB_LINE_MAX];
    int n = 0;
    for (int i = 0; i < prefixLen && n < LIB_LINE_MAX - 1; i++)
        line[n++] = pPrefix[i];
    for (int i = 0; i < len && n < LIB_LINE_MAX - 1; i++)
        line[n++] = pText[i];
    line[n] = 0;
    jkGuiRend_DarrayReallocStr(pArray, line, 0);
}

// Adds pText[0..len) as lines no wider than width, broken at spaces where it
// can be; the first line after pFirst, the others after pNext.
static void jkGuiLibrary_AddWrapped(Darray* pArray, stdFont* pFont, const char16_t* pFirst, int firstLen,
                                    const char16_t* pNext, int nextLen, const char16_t* pText, int len, int width)
{
    int bFirst = 1;
    if (width < 16)
        width = 16;
    if (len <= 0)
    {
        jkGuiLibrary_AddLine(pArray, pFirst, firstLen, pText, 0);
        return;
    }
    while (len > 0)
    {
        if (!bFirst)
        {
            while (len > 0 && *pText == ' ')
            {
                pText++;
                len--;
            }
            if (len <= 0)
                break;
        }
        int w = 0, fit = 0, lastSpace = -1;
        for (int i = 0; i < len; i++)
        {
            w += jkGuiLibrary_CharWidth(pFont, pText[i]);
            if (w > width)
                break;
            fit = i + 1;
            if (pText[i] == ' ' && i > 0)
                lastSpace = i;
        }
        int take;
        if (fit >= len)
            take = len;
        else if (lastSpace > 0 && fit < len && pText[fit] != ' ')
            take = lastSpace;
        else
            take = fit > 0 ? fit : 1;
        if (take > LIB_LINE_MAX - 8)
            take = LIB_LINE_MAX - 8;
        if (bFirst)
            jkGuiLibrary_AddLine(pArray, pFirst, firstLen, pText, take);
        else
            jkGuiLibrary_AddLine(pArray, pNext, nextLen, pText, take);
        pText += take;
        len -= take;
        bFirst = 0;
    }
}

// Fills a list with text from library.js: lines split at '\n', each wrapped
// to the list's width. A line with a tab has a label before it (drawn at the
// left) and the rest wrapped in the column the paddings start.
static void jkGuiLibrary_FillLines(Darray* pArray, jkGuiElement* pElement, const char16_t* pText, int pad)
{
    static const char16_t tab[2] = {'\t', 0};
    stdFont* pFont = jkGui_stdFonts[pElement->textType];
    int width = pElement->rect.width - 12;

    jkGuiRend_DarrayFreeEntry(pArray);
    if (pText && pFont)
    {
        const char16_t* p = pText;
        while (1)
        {
            int len = 0;
            while (p[len] && p[len] != '\n')
                len++;
            int tabAt = -1;
            if (pad > 0)
            {
                for (int i = 0; i < len; i++)
                {
                    if (p[i] == '\t')
                    {
                        tabAt = i;
                        break;
                    }
                }
            }
            if (tabAt >= 0)
            {
                char16_t label[64];
                int labelLen = tabAt < 62 ? tabAt : 62;
                memcpy(label, p, sizeof(char16_t) * labelLen);
                label[labelLen++] = '\t';
                jkGuiLibrary_AddWrapped(pArray, pFont, label, labelLen, tab, 1, p + tabAt + 1, len - tabAt - 1, width - pad);
            }
            else
            {
                jkGuiLibrary_AddWrapped(pArray, pFont, NULL, 0, NULL, 0, p, len, width);
            }
            if (!p[len])
                break;
            p += len + 1;
        }
    }
    jkGuiRend_DarrayReallocStr(pArray, NULL, 0);
    jkGuiRend_SetClickableString(pElement, pArray);
}

// Copies pText[0..len) to pOut, cut with "..." to maxW if it's wider, and
// returns how many letters it wrote (at most len + 3).
static int jkGuiLibrary_CutTo(stdFont* pFont, const char16_t* pText, int len, int maxW, char16_t* pOut)
{
    int n = 0;
    if (jkGuiLibrary_TextWidth(pFont, pText, len) <= maxW)
    {
        memcpy(pOut, pText, sizeof(char16_t) * len);
        return len;
    }
    static const char16_t dots[4] = {'.', '.', '.', 0};
    int dotsW = jkGuiLibrary_TextWidth(pFont, dots, 3);
    int w = 0;
    for (int i = 0; i < len; i++)
    {
        int cw = jkGuiLibrary_CharWidth(pFont, pText[i]);
        if (w + cw + dotsW > maxW)
            break;
        w += cw;
        pOut[n++] = pText[i];
    }
    while (n > 0 && pOut[n - 1] == ' ')
        n--;
    pOut[n++] = '.';
    pOut[n++] = '.';
    pOut[n++] = '.';
    return n;
}

// Where a row's parts are: its title [0, tabAt), the year and size
// [restAt, restEnd), and its kind, if it has one, kindLen letters at kindAt.
static void jkGuiLibrary_SplitRow(const char16_t* pRow, int* pTabAt, int* pRestAt, int* pRestEnd, int* pKindAt, int* pKindLen)
{
    int tabAt = 0;
    while (pRow[tabAt] && pRow[tabAt] != '\t')
        tabAt++;
    int restAt = tabAt, restEnd = tabAt, kindAt = 0, kindLen = 0;
    if (pRow[tabAt] == '\t')
    {
        restAt = tabAt + 1;
        restEnd = restAt;
        while (pRow[restEnd] && pRow[restEnd] != '\t')
            restEnd++;
        if (pRow[restEnd] == '\t')
        {
            kindAt = restEnd + 1;
            while (pRow[kindAt + kindLen] && kindLen < 8)
                kindLen++;
        }
    }
    *pTabAt = tabAt;
    *pRestAt = restAt;
    *pRestEnd = restEnd;
    *pKindAt = kindAt;
    *pKindLen = kindLen;
}

// A list row from the page: the title, a tab, the year and size, and for a
// level that isn't plain multiplayer, a tab and its kind (CTF, KFY, SP). The
// list draws what's after a row's last tab at the first stop past what's
// before it. With kinds in the list (kindX > 0), the stops are kindX and the
// year's: a row with a kind is its title, cut to end before kindX, a tab, and
// the kind with spaces up to the year; a row without one has spaces after its
// title, so it ends past kindX and its year goes to the year's stop.
static void jkGuiLibrary_AddRow(stdFont* pFont, const char16_t* pRow, int kindX)
{
    char16_t line[LIB_LINE_MAX];
    int tabAt = 0, restAt, restEnd, kindAt = 0, kindLen = 0;
    jkGuiLibrary_SplitRow(pRow, &tabAt, &restAt, &restEnd, &kindAt, &kindLen);
    int titleLen = tabAt;
    if (titleLen > LIB_LINE_MAX / 2)
        titleLen = LIB_LINE_MAX / 2;
    int spaceW = pFont->marginX > 0 ? pFont->marginX : 1;
    int n;
    if (kindLen && kindX > 0)
    {
        n = jkGuiLibrary_CutTo(pFont, pRow, titleLen, kindX - (LIB_LIST_YEAR_X - LIB_LIST_TITLE_W), line);
        line[n++] = '\t';
        for (int i = 0; i < kindLen; i++)
            line[n++] = pRow[kindAt + i];
        // As many spaces as fit before the year's stop, one at least.
        int w = kindX + jkGuiLibrary_TextWidth(pFont, pRow + kindAt, kindLen) + spaceW;
        line[n++] = ' ';
        while (w + spaceW <= LIB_LIST_YEAR_X && n < LIB_LINE_MAX / 2 + 64)
        {
            line[n++] = ' ';
            w += spaceW;
        }
    }
    else
    {
        n = jkGuiLibrary_CutTo(pFont, pRow, titleLen, LIB_LIST_TITLE_W, line);
        if (kindX > 0 && pRow[tabAt] == '\t')
        {
            int w = jkGuiLibrary_TextWidth(pFont, line, n);
            while (w <= kindX && n < LIB_LINE_MAX / 2 + 64)
            {
                line[n++] = ' ';
                w += spaceW;
            }
        }
        if (pRow[tabAt] == '\t')
            line[n++] = '\t';
    }
    for (int i = restAt; i < restEnd && n < LIB_LINE_MAX - 1; i++)
        line[n++] = pRow[i];
    line[n] = 0;
    jkGuiRend_DarrayReallocStr(&jkGuiLibrary_rows, line, 0);
}

// ---- Drawing

static void jkGuiLibrary_ShotDraw(jkGuiElement *pElement, jkGuiMenu *pMenu, tVBuffer *vbuf, BOOL redraw)
{
    if (redraw)
        jkGuiRend_CopyVBuffer(pMenu, &pElement->rect);
    jkGuiRend_DrawRect(vbuf, &pElement->rect, pMenu->fillColor);
}

// A list of text, not of things to pick: no line is drawn picked.
static void jkGuiLibrary_PlainListDraw(jkGuiElement *pElement, jkGuiMenu *pMenu, tVBuffer *vbuf, BOOL redraw)
{
    stdFont* fonts[16];
    jkGuiMenu tmp = *pMenu;
    int last = pElement->textType + 1;

    if (last >= 16)
        last = 15;
    for (int i = 0; i <= last; i++)
        fonts[i] = pMenu->fonts[i];
    fonts[last] = pMenu->fonts[pElement->textType];
    tmp.fonts = fonts;
    if (pElement == &jkGuiLibrary_aElements[LIB_FACTS])
        tmp.paddings = (intptr_t)jkGuiLibrary_factsPaddings;
    else if (jkGuiLibrary_tab == LIB_TAB_IS_FILES)
        tmp.paddings = (intptr_t)jkGuiLibrary_filesPaddings;
    else
        tmp.paddings = 0;
    jkGuiRend_ListBoxDraw(pElement, &tmp, vbuf, redraw);
}

// The mouse wheel over the details or the Readme/Files text scrolls that
// text, 3 lines a notch, and over the list of levels picks the next or
// previous level a notch, whichever has the focus. Elsewhere it does what
// the arrow keys do.
static int jkGuiLibrary_Wheel(jkGuiMenu* pMenu, int32_t notches)
{
    jkGuiElement* el = jkGuiLibrary_aElements;
    jkGuiElement* pOver = pMenu ? pMenu->lastMouseOverClickable : NULL;
    if (!notches || !pOver || !pOver->bIsVisible)
        return 0;
    if (pOver == &el[LIB_LIST])
    {
        int count = pOver->texInfo.numTextEntries;
        int sel = pOver->selectedTextEntry - notches;
        if (sel >= count)
            sel = count - 1;
        if (sel < 0)
            sel = 0;
        if (count <= 0 || sel == pOver->selectedTextEntry)
            return 1;
        pOver->selectedTextEntry = sel;
        jkGuiLibrary_ListClicked(pOver, pMenu, -1, -1, 0);
        jkGuiRend_UpdateAndDrawClickable(pOver, pMenu, 1);
        return 1;
    }
    if (pOver != &el[LIB_FACTS] && pOver != &el[LIB_TEXT])
        return 0;
    // When a list scrolls, its arrows take its first and last lines.
    int maxScroll = pOver->texInfo.numTextEntries - (pOver->texInfo.maxTextEntries - 2);
    if (pOver->texInfo.numTextEntries <= pOver->texInfo.maxTextEntries || maxScroll <= 0)
        return 1;
    int scroll = pOver->texInfo.textScrollY - notches * 3;
    if (scroll > maxScroll)
        scroll = maxScroll;
    if (scroll < 0)
        scroll = 0;
    if (scroll == pOver->texInfo.textScrollY)
        return 1;
    pOver->texInfo.textScrollY = scroll;
    pOver->selectedTextEntry = scroll;
    jkGuiRend_UpdateAndDrawClickable(pOver, pMenu, 1);
    return 1;
}

static void jkGuiLibrary_Draw(jkGuiMenu* pMenu, int idx)
{
    if (pMenu)
        jkGuiRend_UpdateAndDrawClickable(&jkGuiLibrary_aElements[idx], pMenu, 1);
}

// ---- What the menu shows

static void jkGuiLibrary_ShowTab(jkGuiMenu* pMenu)
{
    jkGuiElement* el = jkGuiLibrary_aElements;
    int bOverview = jkGuiLibrary_tab == LIB_TAB_IS_OVERVIEW;
    int bShots = jkGuiLibrary_js_number(LIB_NUM_SHOTS) > 0;
    // With no level picked (an empty list), the details' text says why, with
    // no picture frames beside it.
    int bPicked = jkGuiLibrary_js_number(LIB_NUM_SELECTED) >= 0;

    // The tab that's open is drawn lit and isn't a button.
    for (int i = 0; i < 3; i++)
    {
        jkGuiElement* pTab = &el[LIB_TAB_OVERVIEW + i];
        int bOpen = jkGuiLibrary_tab == i;
        pTab->type = bOpen ? ELEMENT_TEXT : ELEMENT_TEXTBUTTON;
        pTab->textType = bOpen ? 3 : 2;
    }
    el[LIB_SHOT].bIsVisible = bOverview && bPicked;
    el[LIB_NO_SHOT].bIsVisible = bOverview && bPicked && !bShots;
    el[LIB_SHOT].wHintText = bShots && jkGuiLibrary_js_number(LIB_NUM_SHOTS) > 1 ? u"Click the picture for the next screenshot." : NULL;
    el[LIB_LEVEL_TITLE].bIsVisible = bOverview;
    el[LIB_FACTS].bIsVisible = bOverview;
    el[LIB_TEXT].bIsVisible = !bOverview;
    int pv = jkGuiLibrary_js_number(LIB_NUM_PV);
    el[LIB_PV].bIsVisible = bOverview && bPicked;
    el[LIB_PV_TEXT].bIsVisible = bOverview && bPicked && pv != 2;
    el[LIB_PV].wHintText = pv == 1 || pv == 2 ? u"Click for a bigger 3D view you can move around in."
                         : pv == 3 ? u"Click to try the 3D view again." : NULL;
    // A text box the player clicked in (which focuses it) that's now hidden
    // gives the focus to the list of levels: the menu system, drawing it
    // hidden, would otherwise look for where the focus goes next forever.
    if (pMenu && pMenu->focusedElement && !pMenu->focusedElement->bIsVisible)
        pMenu->focusedElement = &el[LIB_LIST];
    jkGuiLibrary_js_shot(bOverview && bPicked && bShots, el[LIB_SHOT].rect.x + 1, el[LIB_SHOT].rect.y + 1,
                         el[LIB_SHOT].rect.width - 2, el[LIB_SHOT].rect.height - 2);
    jkGuiLibrary_js_preview(bOverview && bPicked, el[LIB_PV].rect.x + 1, el[LIB_PV].rect.y + 1,
                            el[LIB_PV].rect.width - 2, el[LIB_PV].rect.height - 2);
}

// What the 3D view's frame says while the level isn't drawn in it.
static void jkGuiLibrary_SetPreviewText()
{
    if (jkGuiLibrary_pPreviewText)
        free(jkGuiLibrary_pPreviewText);
    jkGuiLibrary_pPreviewText = jkGuiLibrary_GetText(LIB_TEXT_PV, 0);
    jkGuiLibrary_aElements[LIB_PV_TEXT].wstr = jkGuiLibrary_pPreviewText;
}

static void jkGuiLibrary_FillPanel()
{
    jkGuiElement* el = jkGuiLibrary_aElements;
    char16_t* pText;

    jkGuiLibrary_SetText(&el[LIB_LEVEL_TITLE], &jkGuiLibrary_pLevelTitle, jkGuiLibrary_GetText(LIB_TEXT_TITLE, 0));

    pText = jkGuiLibrary_GetText(LIB_TEXT_OVERVIEW, 0);
    jkGuiLibrary_FillLines(&jkGuiLibrary_facts, &el[LIB_FACTS], pText, jkGuiLibrary_factsPaddings[0]);
    free(pText);
    el[LIB_FACTS].selectedTextEntry = 0;
    el[LIB_FACTS].texInfo.textScrollY = 0;

    if (jkGuiLibrary_tab == LIB_TAB_IS_README)
    {
        pText = jkGuiLibrary_GetText(LIB_TEXT_README, 0);
        jkGuiLibrary_FillLines(&jkGuiLibrary_text, &el[LIB_TEXT], pText, 0);
    }
    else
    {
        pText = jkGuiLibrary_GetText(LIB_TEXT_FILES, 0);
        jkGuiLibrary_FillLines(&jkGuiLibrary_text, &el[LIB_TEXT], pText, jkGuiLibrary_filesPaddings[0]);
    }
    free(pText);
    el[LIB_TEXT].selectedTextEntry = 0;
    el[LIB_TEXT].texInfo.textScrollY = 0;
}

static void jkGuiLibrary_FillRows()
{
    jkGuiElement* pList = &jkGuiLibrary_aElements[LIB_LIST];
    stdFont* pFont = jkGui_stdFonts[pList->textType];
    int count = jkGuiLibrary_js_number(LIB_NUM_COUNT);

    jkGuiRend_DarrayFreeEntry(&jkGuiLibrary_rows);
    char16_t** ppRows = count > 0 ? (char16_t**)calloc(count, sizeof(char16_t*)) : NULL;
    int kindW = 0;
    for (int i = 0; ppRows && i < count; i++)
    {
        ppRows[i] = jkGuiLibrary_GetText(LIB_TEXT_ROW, i);
        int tabAt, restAt, restEnd, kindAt, kindLen;
        if (ppRows[i] && pFont)
        {
            jkGuiLibrary_SplitRow(ppRows[i], &tabAt, &restAt, &restEnd, &kindAt, &kindLen);
            int w = kindLen ? jkGuiLibrary_TextWidth(pFont, ppRows[i] + kindAt, kindLen) + (pFont->marginX > 0 ? pFont->marginX : 1) : 0;
            if (w > kindW)
                kindW = w;
        }
    }
    // The kinds' stop: the widest kind and a space end at the year's.
    int kindX = kindW ? LIB_LIST_YEAR_X - kindW : 0;
    jkGuiLibrary_listPaddings[0] = kindX ? kindX : LIB_LIST_YEAR_X;
    jkGuiLibrary_listPaddings[1] = kindX ? LIB_LIST_YEAR_X : 0;
    jkGuiLibrary_listPaddings[2] = 0;
    for (int i = 0; ppRows && i < count; i++)
    {
        if (ppRows[i] && pFont)
            jkGuiLibrary_AddRow(pFont, ppRows[i], kindX);
        free(ppRows[i]);
    }
    free(ppRows);
    jkGuiRend_DarrayReallocStr(&jkGuiLibrary_rows, NULL, 0);
    jkGuiRend_SetClickableString(pList, &jkGuiLibrary_rows);

    int sel = jkGuiLibrary_js_number(LIB_NUM_SELECTED);
    pList->selectedTextEntry = sel >= 0 ? sel : 0;
}

static void jkGuiLibrary_ShowUse(jkGuiMenu* pMenu)
{
    jkGuiElement* el = jkGuiLibrary_aElements;
    int bBusy = jkGuiLibrary_js_number(LIB_NUM_BUSY);
    int bUse = !bBusy && jkGuiLibrary_js_number(LIB_NUM_CAN_USE);

    if (pMenu)
    {
        jkGuiRend_SetVisibleAndDraw(&el[LIB_USE_BUTTON], pMenu, bUse);
        jkGuiRend_SetVisibleAndDraw(&el[LIB_PAGE_BUTTON], pMenu, !bBusy && jkGuiLibrary_js_number(LIB_NUM_SELECTED) >= 0);
    }
    else
    {
        el[LIB_USE_BUTTON].bIsVisible = bUse;
        el[LIB_PAGE_BUTTON].bIsVisible = !bBusy && jkGuiLibrary_js_number(LIB_NUM_SELECTED) >= 0;
    }
}

// Takes in what changed (LIB_CH_*) and draws it again when the menu is up.
static void jkGuiLibrary_Refresh(jkGuiMenu* pMenu, int changed)
{
    jkGuiElement* el = jkGuiLibrary_aElements;

    if (changed & (LIB_CH_PV | LIB_CH_PANEL))
        jkGuiLibrary_SetPreviewText();
    if (changed & LIB_CH_STATUS)
    {
        jkGuiLibrary_SetText(&el[LIB_STATUS], &jkGuiLibrary_pStatus, jkGuiLibrary_GetText(LIB_TEXT_STATUS, 0));
        jkGuiLibrary_Draw(pMenu, LIB_STATUS);
    }
    if (changed & LIB_CH_ROWS)
    {
        jkGuiLibrary_FillRows();
        jkGuiLibrary_Draw(pMenu, LIB_LIST);
    }
    if (changed & LIB_CH_PANEL)
    {
        jkGuiLibrary_FillPanel();
        jkGuiLibrary_ShowTab(pMenu);
        if (pMenu)
            jkGuiRend_Paint(pMenu);
    }
    if (changed & LIB_CH_MSG)
    {
        jkGuiLibrary_SetText(&el[LIB_MESSAGE], &jkGuiLibrary_pMessage, jkGuiLibrary_GetText(LIB_TEXT_MESSAGE, 0));
        jkGuiLibrary_Draw(pMenu, LIB_MESSAGE);
    }
    if (changed & (LIB_CH_USE | LIB_CH_PANEL | LIB_CH_ROWS))
        jkGuiLibrary_ShowUse(pMenu);
    if ((changed & LIB_CH_PV) && !(changed & LIB_CH_PANEL))
    {
        jkGuiLibrary_ShowTab(pMenu);
        if (pMenu)
            jkGuiRend_Paint(pMenu);
    }
}

static void jkGuiLibrary_Query()
{
    int filters = (jkGuiLibrary_filterMp ? 1 : 0) | (jkGuiLibrary_filterCtf ? 2 : 0) | (jkGuiLibrary_filterCoop ? 4 : 0) |
                  (jkGuiLibrary_filterMine ? 8 : 0);
    jkGuiLibrary_js_query(jkGuiLibrary_search, filters, jkGuiLibrary_sort);
    memcpy(jkGuiLibrary_lastSearch, jkGuiLibrary_search, sizeof(jkGuiLibrary_search));
}

static void jkGuiLibrary_ShowSortName(jkGuiMenu* pMenu)
{
    jkGuiLibrary_SetText(&jkGuiLibrary_aElements[LIB_SORT_NAME], &jkGuiLibrary_pSortName, jkGuiLibrary_GetText(LIB_TEXT_SORT, 0));
    jkGuiLibrary_Draw(pMenu, LIB_SORT_NAME);
}

// Each frame the menu is up: the search box, then whatever library.js says
// changed, then whether a level came in.
static void jkGuiLibrary_Idle(jkGuiMenu *pMenu)
{
    if (memcmp(jkGuiLibrary_search, jkGuiLibrary_lastSearch, sizeof(jkGuiLibrary_search)))
        jkGuiLibrary_Query();

    int changed = jkGuiLibrary_js_number(LIB_NUM_CHANGED);
    if (changed)
        jkGuiLibrary_Refresh(pMenu, changed);
    else
        jkGuiLibrary_ShowTab(pMenu); // keeps the screenshot where the frame is

    if (jkGuiLibrary_js_number(LIB_NUM_DONE))
        pMenu->lastClicked = LIB_DONE;
}

// ---- Clicks

static void jkGuiLibrary_ClickSound(jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY)
{
    jkGuiRend_PlayWav(pMenu->soundClick);
}

static int jkGuiLibrary_FilterClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    jkGuiRend_DrawClickableAndUpdatebool(pElement, pMenu, mouseX, mouseY, redraw);
    jkGuiLibrary_filterMp = jkGuiLibrary_aElements[LIB_FILTER_MP].selectedTextEntry != 0;
    jkGuiLibrary_filterCtf = jkGuiLibrary_aElements[LIB_FILTER_CTF].selectedTextEntry != 0;
    jkGuiLibrary_filterCoop = jkGuiLibrary_aElements[LIB_FILTER_COOP].selectedTextEntry != 0;
    jkGuiLibrary_filterMine = jkGuiLibrary_aElements[LIB_FILTER_MINE].selectedTextEntry != 0;
    jkGuiLibrary_Query();
    return 0;
}

static int jkGuiLibrary_SortClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    int step = pElement == &jkGuiLibrary_aElements[LIB_SORT_NEXT] ? 1 : LIB_NUM_SORTS - 1;
    jkGuiLibrary_ClickSound(pMenu, mouseX, mouseY);
    jkGuiLibrary_sort = (jkGuiLibrary_sort + step) % LIB_NUM_SORTS;
    jkGuiLibrary_Query();
    jkGuiLibrary_ShowSortName(pMenu);
    return 0;
}

static int jkGuiLibrary_ListClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    if (mouseX != -1 || mouseY != -1)
        jkGuiRend_ClickSound(pElement, pMenu, mouseX, mouseY, redraw);

    jkGuiLibrary_js_select(pElement->selectedTextEntry);
    // A level coming in keeps its place in the list.
    int sel = jkGuiLibrary_js_number(LIB_NUM_SELECTED);
    if (sel >= 0 && sel != pElement->selectedTextEntry)
    {
        pElement->selectedTextEntry = sel;
        jkGuiRend_UpdateAndDrawClickable(pElement, pMenu, 1);
    }
    // A double click: Use Level.
    if (redraw && jkGuiLibrary_aElements[LIB_USE_BUTTON].bIsVisible)
        jkGuiLibrary_js_use();
    return 0;
}

static int jkGuiLibrary_TabClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    int tab = (int)(pElement - &jkGuiLibrary_aElements[LIB_TAB_OVERVIEW]);
    if (tab < 0 || tab > 2 || tab == jkGuiLibrary_tab)
        return 0;
    jkGuiLibrary_ClickSound(pMenu, mouseX, mouseY);
    jkGuiLibrary_tab = tab;
    jkGuiLibrary_Refresh(pMenu, LIB_CH_PANEL);
    return 0;
}

static int jkGuiLibrary_ShotClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    if (jkGuiLibrary_js_number(LIB_NUM_SHOTS) > 1)
    {
        jkGuiLibrary_ClickSound(pMenu, mouseX, mouseY);
        jkGuiLibrary_js_nextShot();
    }
    return 0;
}

// The 3D view's frame: the big 3D view, once the level's 3D view is there
// or coming; one that didn't load tries again.
static int jkGuiLibrary_PreviewClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    int pv = jkGuiLibrary_js_number(LIB_NUM_PV);
    if (pv == 3)
    {
        jkGuiLibrary_ClickSound(pMenu, mouseX, mouseY);
        jkGuiLibrary_js_viewCommand(3);
        return 0;
    }
    if (pv != 1 && pv != 2)
        return 0;
    jkGuiLibrary_ClickSound(pMenu, mouseX, mouseY);
    return LIB_VIEW;
}

// ---- The big 3D view

static void jkGuiLibrary_ViewRefresh(jkGuiMenu* pMenu)
{
    jkGuiElement* v = jkGuiLibrary_aViewElements;
    int pv = jkGuiLibrary_js_number(LIB_NUM_PV);

    // A title too long for the big font (a long name, or a map's name and
    // number after it) is drawn in the medium one, before it's cut.
    char16_t* pTitle = jkGuiLibrary_GetText(LIB_TEXT_VIEW_TITLE, 0);
    v[VIEW_TITLE].textType = 6;
    if (!jkGuiLibrary_Fits(&v[VIEW_TITLE], pTitle))
        v[VIEW_TITLE].textType = 3;
    jkGuiLibrary_SetText(&v[VIEW_TITLE], &jkGuiLibrary_pViewTitle, pTitle);
    if (jkGuiLibrary_pViewText)
        free(jkGuiLibrary_pViewText);
    jkGuiLibrary_pViewText = jkGuiLibrary_GetText(LIB_TEXT_PV, 0);
    v[VIEW_TEXT].wstr = jkGuiLibrary_pViewText;
    v[VIEW_TEXT].bIsVisible = pv != 2;
    jkGuiLibrary_SetText(&v[VIEW_HELP], &jkGuiLibrary_pViewHelp, jkGuiLibrary_GetText(LIB_TEXT_VIEW_HELP, 0));
    jkGuiLibrary_SetText(&v[VIEW_NOTE], &jkGuiLibrary_pViewNote, jkGuiLibrary_GetText(LIB_TEXT_VIEW_NOTE, 0));
    jkGuiLibrary_SetText(&v[VIEW_MODE], &jkGuiLibrary_pViewMode, jkGuiLibrary_GetText(LIB_TEXT_VIEW_MODE, 0));
    jkGuiLibrary_SetText(&v[VIEW_NEXT], &jkGuiLibrary_pViewNext, jkGuiLibrary_GetText(LIB_TEXT_VIEW_NEXT, 0));
    v[VIEW_MODE].bIsVisible = pv == 2 && jkGuiLibrary_pViewMode && jkGuiLibrary_pViewMode[0];
    // Next Map stays while a map loads (a press goes on to the one after) and
    // when one didn't load, to the level's other maps.
    v[VIEW_NEXT].bIsVisible = pv >= 1 && pv <= 3 && jkGuiLibrary_pViewNext && jkGuiLibrary_pViewNext[0];
    v[VIEW_FRAME].wHintText = pv == 3 ? u"Click to try the 3D view again." : NULL;
    // A button that went away gives the focus back.
    if (pMenu && pMenu->focusedElement && !pMenu->focusedElement->bIsVisible)
        pMenu->focusedElement = &v[VIEW_BACK];
    if (pMenu)
        jkGuiRend_Paint(pMenu);
}

static void jkGuiLibrary_ViewIdle(jkGuiMenu *pMenu)
{
    jkGuiElement* f = &jkGuiLibrary_aViewElements[VIEW_FRAME];
    jkGuiLibrary_js_view(1, f->rect.x + 1, f->rect.y + 1, f->rect.width - 2, f->rect.height - 2);
    int changed = jkGuiLibrary_js_number(LIB_NUM_CHANGED);
    jkGuiLibrary_pending |= changed & ~(LIB_CH_PV | LIB_CH_VIEW);
    if (changed & (LIB_CH_PV | LIB_CH_VIEW | LIB_CH_PANEL))
        jkGuiLibrary_ViewRefresh(pMenu);
}

// The frame takes clicks only while the level isn't drawn over it: one that
// didn't load tries again.
static int jkGuiLibrary_ViewFrameClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    if (jkGuiLibrary_js_number(LIB_NUM_PV) != 3)
        return 0;
    jkGuiLibrary_ClickSound(pMenu, mouseX, mouseY);
    jkGuiLibrary_js_viewCommand(3);
    jkGuiLibrary_ViewRefresh(pMenu);
    return 0;
}

static int jkGuiLibrary_ViewBackClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    jkGuiLibrary_ClickSound(pMenu, mouseX, mouseY);
    return 1;
}

static int jkGuiLibrary_ViewModeClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    jkGuiLibrary_ClickSound(pMenu, mouseX, mouseY);
    jkGuiLibrary_js_viewCommand(1);
    jkGuiLibrary_ViewRefresh(pMenu);
    return 0;
}

static int jkGuiLibrary_ViewNextClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    jkGuiLibrary_ClickSound(pMenu, mouseX, mouseY);
    jkGuiLibrary_js_viewCommand(2);
    jkGuiLibrary_ViewRefresh(pMenu);
    return 0;
}

// The big 3D view, until Back or Escape; then the library as it was.
static void jkGuiLibrary_ShowView()
{
    jkGuiElement* v = jkGuiLibrary_aViewElements;

    jkGui_InitMenu(&jkGuiLibrary_viewMenu, jkGui_stdBitmaps[JKGUI_BM_BK_MULTI]);
    v[VIEW_TITLE].wstr = NULL;
    v[VIEW_TEXT].wstr = NULL;
    v[VIEW_HELP].wstr = NULL;
    v[VIEW_NOTE].wstr = NULL;
    v[VIEW_MODE].wstr = NULL;
    v[VIEW_NEXT].wstr = NULL;
    jkGuiLibrary_pending = 0;
    jkGuiLibrary_js_view(1, v[VIEW_FRAME].rect.x + 1, v[VIEW_FRAME].rect.y + 1,
                         v[VIEW_FRAME].rect.width - 2, v[VIEW_FRAME].rect.height - 2);
    jkGuiLibrary_ViewRefresh(NULL);
    jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiLibrary_viewMenu, &v[VIEW_BACK]);
    jkGuiRend_DisplayAndReturnClicked(&jkGuiLibrary_viewMenu);
    jkGuiLibrary_js_view(0, 0, 0, 0, 0);
}

// Cancel stops a level that's coming in; otherwise it closes the library.
static int jkGuiLibrary_CancelClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    jkGuiLibrary_ClickSound(pMenu, mouseX, mouseY);
    if (jkGuiLibrary_js_cancel())
        return 0;
    return LIB_CANCEL;
}

static int jkGuiLibrary_PageClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    jkGuiLibrary_ClickSound(pMenu, mouseX, mouseY);
    jkGuiLibrary_js_page();
    return 0;
}

static int jkGuiLibrary_UseClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw)
{
    jkGuiLibrary_ClickSound(pMenu, mouseX, mouseY);
    jkGuiLibrary_js_use();
    return 0;
}

// ---- The engine's side

int jkGuiLibrary_IsAvailable()
{
    return jkGuiLibrary_js_available();
}

int jkGuiLibrary_Show(int bMots, char* pOutName, int outLen)
{
    jkGuiElement* el = jkGuiLibrary_aElements;
    int result;

    if (outLen > 0)
        pOutName[0] = 0;
    if (!jkGuiLibrary_js_begin(bMots))
        return 0;

    if (!jkGuiLibrary_bArrays)
    {
        jkGuiRend_DarrayNewStr(&jkGuiLibrary_rows, 128, 1);
        jkGuiRend_DarrayNewStr(&jkGuiLibrary_facts, 32, 1);
        jkGuiRend_DarrayNewStr(&jkGuiLibrary_text, 128, 1);
        jkGuiLibrary_bArrays = 1;
    }

    jkGui_InitMenu(&jkGuiLibrary_menu, jkGui_stdBitmaps[JKGUI_BM_BK_MULTI]);
    el[LIB_FILTER_CTF].wstr = bMots ? u"Kill the Fool" : u"CTF";
    el[LIB_FILTER_CTF].wHintText = bMots ? u"Show Kill the Fool levels." : u"Show Capture the Flag levels.";
    el[LIB_SEARCH].wstr = jkGuiLibrary_search;
    el[LIB_SEARCH].selectedTextEntry = LIB_SEARCH_MAX;
    el[LIB_FILTER_MP].selectedTextEntry = jkGuiLibrary_filterMp;
    el[LIB_FILTER_CTF].selectedTextEntry = jkGuiLibrary_filterCtf;
    el[LIB_FILTER_COOP].selectedTextEntry = jkGuiLibrary_filterCoop;
    el[LIB_FILTER_MINE].selectedTextEntry = jkGuiLibrary_filterMine;
    el[LIB_PV_TEXT].wstr = NULL;
    // Strings this menu keeps itself are dropped with the last ones it showed.
    el[LIB_STATUS].wstr = NULL;
    el[LIB_LEVEL_TITLE].wstr = NULL;
    el[LIB_MESSAGE].wstr = NULL;
    el[LIB_SORT_NAME].wstr = NULL;

    jkGuiLibrary_Query();
    jkGuiLibrary_ShowSortName(NULL);
    jkGuiLibrary_js_number(LIB_NUM_CHANGED);
    jkGuiLibrary_Refresh(NULL, LIB_CH_ALL);

    // The Host screen's menu mode and palette are already up.
    jkGuiRend_MenuSetReturnKeyShortcutElement(&jkGuiLibrary_menu, &el[LIB_USE_BUTTON]);
    jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiLibrary_menu, &el[LIB_CANCEL_BUTTON]);
    for (;;)
    {
        jkGuiRend_SetMouseWheelHandler(&jkGuiLibrary_menu, jkGuiLibrary_Wheel);
        result = jkGuiRend_DisplayAndReturnClicked(&jkGuiLibrary_menu);
        jkGuiRend_SetMouseWheelHandler(NULL, NULL);
        if (result != LIB_VIEW)
            break;
        // The big 3D view, then the library again as it was, with what
        // changed meanwhile.
        jkGuiLibrary_js_shot(0, 0, 0, 0, 0);
        jkGuiLibrary_js_preview(0, 0, 0, 0, 0);
        jkGuiLibrary_ShowView();
        jkGuiLibrary_Refresh(NULL, jkGuiLibrary_pending | LIB_CH_STATUS | LIB_CH_MSG | LIB_CH_USE | LIB_CH_PV);
        jkGuiLibrary_pending = 0;
    }

    jkGuiLibrary_js_shot(0, 0, 0, 0, 0);
    jkGuiLibrary_js_preview(0, 0, 0, 0, 0);
    if (result == LIB_DONE && outLen > 0)
        jkGuiLibrary_js_take(pOutName, outLen);
    jkGuiLibrary_js_end();

    // The lists' strings are the arrays' own; the elements forget them.
    jkGuiRend_DarrayFreeEntry(&jkGuiLibrary_rows);
    jkGuiRend_DarrayFreeEntry(&jkGuiLibrary_facts);
    jkGuiRend_DarrayFreeEntry(&jkGuiLibrary_text);
    el[LIB_LIST].unistr = NULL;
    el[LIB_FACTS].unistr = NULL;
    el[LIB_TEXT].unistr = NULL;

    return outLen > 0 && pOutName[0] != 0;
}

#endif // PLATFORM_WSRELAY
