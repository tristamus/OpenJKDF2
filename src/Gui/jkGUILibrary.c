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
#define LIB_CH_ALL (31)

// library.js's text() and number().
#define LIB_TEXT_STATUS (0)
#define LIB_TEXT_ROW (1)
#define LIB_TEXT_SORT (2)
#define LIB_TEXT_TITLE (3)
#define LIB_TEXT_OVERVIEW (4)
#define LIB_TEXT_README (5)
#define LIB_TEXT_FILES (6)
#define LIB_TEXT_MESSAGE (7)

#define LIB_NUM_CHANGED (0)
#define LIB_NUM_COUNT (1)
#define LIB_NUM_SELECTED (2)
#define LIB_NUM_CAN_USE (3)
#define LIB_NUM_BUSY (4)
#define LIB_NUM_DONE (5)
#define LIB_NUM_SHOTS (6)

#define LIB_NUM_SORTS (6)
#define LIB_SEARCH_MAX (31)

// Where the title column ends in the list, and where the year and size start
// (from the row's left edge, as the list draws it). The list draws what's
// after a row's last tab at the first of these stops past what's before it,
// and cuts it at the list's edge.
#define LIB_LIST_TITLE_W (156)
static int32_t jkGuiLibrary_listPaddings[2] = {162, 0};
// Labels in the Overview (Author:, Released:...), sizes in Files.
static int32_t jkGuiLibrary_factsPaddings[2] = {72, 0};
static int32_t jkGuiLibrary_filesPaddings[2] = {58, 0};

static int32_t jkGuiLibrary_listBitmaps[2] = {JKGUI_BM_UP_15, JKGUI_BM_DOWN_15};

static int jkGuiLibrary_FilterClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_SortClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_ListClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_TabClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
static int jkGuiLibrary_ShotClicked(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, BOOL redraw);
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
    { ELEMENT_CUSTOM,     0, 0, NULL, 0, { 310, 104, 320, 180 }, 1, 0, NULL, jkGuiLibrary_ShotDraw, jkGuiLibrary_ShotClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, u"No screenshot for this level", 3, { 310, 104, 320, 180 }, 0, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 2, NULL, 2, { 310, 286, 320, 22 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_LISTBOX,    0, 0, NULL, 0, { 310, 310, 320, 76 }, 1, 0, NULL, jkGuiLibrary_PlainListDraw, NULL, jkGuiLibrary_listBitmaps, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_LISTBOX,    0, 0, NULL, 0, { 310, 104, 320, 282 }, 0, 0, NULL, jkGuiLibrary_PlainListDraw, NULL, jkGuiLibrary_listBitmaps, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXT,       0, 0, NULL, 3, { 20, 390, 600, 18 }, 1, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, "GUI_CANCEL", 3, { 20, 430, 200, 40 }, 1, 0, NULL, NULL, jkGuiLibrary_CancelClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Massassi Page", 3, { 220, 430, 200, 40 }, 1, 0, u"Opens the level's page on Massassi.net in a new tab.", NULL, jkGuiLibrary_PageClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_TEXTBUTTON, 0, 2, u"Use Level", 3, { 420, 430, 200, 40 }, 1, 0, u"Gets the level, saves it in this browser and picks it on the Host screen.", NULL, jkGuiLibrary_UseClicked, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
    { ELEMENT_END,        0, 0, NULL, 0, { 0, 0, 0, 0 }, 0, 0, NULL, NULL, NULL, NULL, { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } }, 0 },
};

static jkGuiMenu jkGuiLibrary_menu =
{
    jkGuiLibrary_aElements, 0, 65535, 65535, 15, NULL, NULL, jkGui_stdBitmaps, jkGui_stdFonts, (intptr_t)jkGuiLibrary_listPaddings, jkGuiLibrary_Idle, "thermloop01.wav", "thrmlpu2.wav", NULL, NULL, NULL, 0, NULL, NULL
};

// The search, filters, sort and tab stay as the player left them, for the
// next time the library opens.
static char16_t jkGuiLibrary_search[LIB_SEARCH_MAX + 1];
static char16_t jkGuiLibrary_lastSearch[LIB_SEARCH_MAX + 1];
static int jkGuiLibrary_filterMp = 1;
static int jkGuiLibrary_filterCtf = 1;
static int jkGuiLibrary_filterCoop = 0;
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

static void jkGuiLibrary_SetText(jkGuiElement* pElement, char16_t** ppKeep, char16_t* pText)
{
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

// A list row: the title, cut with "..." to fit its column, a tab, the rest.
static void jkGuiLibrary_AddRow(stdFont* pFont, const char16_t* pRow)
{
    static const char16_t dots[4] = {'.', '.', '.', 0};
    char16_t line[LIB_LINE_MAX];
    int tabAt = 0;
    while (pRow[tabAt] && pRow[tabAt] != '\t')
        tabAt++;
    int titleLen = tabAt;
    if (titleLen > LIB_LINE_MAX / 2)
        titleLen = LIB_LINE_MAX / 2;
    int n = 0;
    if (jkGuiLibrary_TextWidth(pFont, pRow, titleLen) <= LIB_LIST_TITLE_W)
    {
        memcpy(line, pRow, sizeof(char16_t) * titleLen);
        n = titleLen;
    }
    else
    {
        int dotsW = jkGuiLibrary_TextWidth(pFont, dots, 3);
        int w = 0;
        for (int i = 0; i < titleLen; i++)
        {
            int cw = jkGuiLibrary_CharWidth(pFont, pRow[i]);
            if (w + cw + dotsW > LIB_LIST_TITLE_W)
                break;
            w += cw;
            line[n++] = pRow[i];
        }
        while (n > 0 && line[n - 1] == ' ')
            n--;
        line[n++] = '.';
        line[n++] = '.';
        line[n++] = '.';
    }
    if (pRow[tabAt] == '\t')
    {
        const char16_t* pRest = pRow + tabAt;
        while (*pRest && n < LIB_LINE_MAX - 1)
            line[n++] = *pRest++;
    }
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

    // The tab that's open is drawn lit and isn't a button.
    for (int i = 0; i < 3; i++)
    {
        jkGuiElement* pTab = &el[LIB_TAB_OVERVIEW + i];
        int bOpen = jkGuiLibrary_tab == i;
        pTab->type = bOpen ? ELEMENT_TEXT : ELEMENT_TEXTBUTTON;
        pTab->textType = bOpen ? 3 : 2;
    }
    el[LIB_SHOT].bIsVisible = bOverview;
    el[LIB_NO_SHOT].bIsVisible = bOverview && !bShots;
    el[LIB_SHOT].wHintText = bShots && jkGuiLibrary_js_number(LIB_NUM_SHOTS) > 1 ? u"Click the picture for the next screenshot." : NULL;
    el[LIB_LEVEL_TITLE].bIsVisible = bOverview;
    el[LIB_FACTS].bIsVisible = bOverview;
    el[LIB_TEXT].bIsVisible = !bOverview;
    jkGuiLibrary_js_shot(bOverview && bShots, el[LIB_SHOT].rect.x + 1, el[LIB_SHOT].rect.y + 1,
                         el[LIB_SHOT].rect.width - 2, el[LIB_SHOT].rect.height - 2);
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
    for (int i = 0; i < count; i++)
    {
        char16_t* pRow = jkGuiLibrary_GetText(LIB_TEXT_ROW, i);
        if (pRow && pFont)
            jkGuiLibrary_AddRow(pFont, pRow);
        free(pRow);
    }
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
}

static void jkGuiLibrary_Query()
{
    int filters = (jkGuiLibrary_filterMp ? 1 : 0) | (jkGuiLibrary_filterCtf ? 2 : 0) | (jkGuiLibrary_filterCoop ? 4 : 0);
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
    result = jkGuiRend_DisplayAndReturnClicked(&jkGuiLibrary_menu);

    jkGuiLibrary_js_shot(0, 0, 0, 0, 0);
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
