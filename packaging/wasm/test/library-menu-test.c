// Tests for the Level Library menu (src/Gui/jkGUILibrary.c) without the game:
// the menu's own code, built for the browser as the engine is, on stand-ins
// for the game's menu system (below: a menu that runs frames and lets the
// test click) and for the page's half (library-menu-mock.js). What the menu
// shows, what it tells the page, and what it gives the Host screen back.
// From the top of the source tree, after the engine has been built once
// (for build_emcc/generated):
//
//   sh packaging/wasm/test/library-menu-test.sh

#include "types.h"
#include "Gui/jkGUIRend.h"
#include "Gui/jkGUI.h"
#include "General/stdFont.h"
#include "Gui/jkGUILibrary.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The element numbers in jkGUILibrary.c.
enum
{
    E_HINT, E_TITLE, E_STATUS, E_SEARCH_LABEL, E_SEARCH, E_MP, E_CTF, E_COOP, E_SORT_LABEL, E_SORT_PREV,
    E_SORT_NEXT, E_SORT_NAME, E_LIST, E_TAB_OVERVIEW, E_TAB_README, E_TAB_FILES, E_SHOT, E_NO_SHOT,
    E_LEVEL_TITLE, E_FACTS, E_TEXT, E_MESSAGE, E_CANCEL, E_PAGE, E_USE, E_PV, E_PV_TEXT, E_MINE, E_COUNT
};

// The big 3D view's.
enum
{
    V_HINT, V_TITLE, V_FRAME, V_TEXT, V_HELP, V_NOTE, V_BACK, V_MODE, V_NEXT, V_COUNT
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

// ---- Fonts: every letter 5 wide plus 1 between, a space 3.

#define GLYPH_W (5)
#define SPACING (1)
#define SPACE_W (3)

static stdFont fonts[16];
stdFont* jkGui_stdFonts[16];
stdBitmap* jkGui_stdBitmaps[0x27];

int _iswspace(int a)
{
    return a == ' ' || (a >= 9 && a <= 13);
}

int stdFont_sub_4355F0(stdFont *font, const char16_t *text)
{
    int w = 0;
    for (; *text; text++)
        w += _iswspace(*text) ? font->marginX : GLYPH_W + font->marginY;
    return w;
}

static int width(const char16_t* s, int len)
{
    int w = 0;
    for (int i = 0; i < len && s[i]; i++)
        w += _iswspace(s[i]) ? SPACE_W : GLYPH_W + SPACING;
    return w;
}

// ---- Text

static int wlen(const char16_t* s)
{
    int n = 0;
    while (s && s[n])
        n++;
    return n;
}

static void toUtf8(char* out, int outLen, const char16_t* s)
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

static int wstarts(const char16_t* s, const char* want)
{
    if (!s)
        return 0;
    for (; *want; want++, s++)
    {
        if (*s != (unsigned char)*want)
            return 0;
    }
    return 1;
}

static char shown[1024];
static const char* show(const char16_t* s)
{
    toUtf8(shown, sizeof(shown), s);
    return shown;
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
    for (int i = 0; i < array->total; i++)
    {
        if (array->bInitialized)
            free(((jkGuiStringEntry*)array->alloc)[i].str);
    }
    array->total = 0;
}

void jkGuiRend_SetClickableString(jkGuiElement *element, Darray *array)
{
    element->unistr = (jkGuiStringEntry*)array->alloc;
}

static int rowCount(jkGuiElement* el)
{
    int n = 0;
    while (el->unistr && el->unistr[n].str)
        n++;
    return n;
}

// ---- The menu system: drawing is counted, a list's drawing is kept.

static jkGuiMenu* displayed = NULL;
static int draws = 0;
static int paints = 0;
static int listDraws = 0;
static stdFont* listFonts[2];
static int32_t* listPaddings = NULL;
static int sounds = 0;

void jkGui_InitMenu(jkGuiMenu *menu, stdBitmap *bgBitmap)
{
    // As the game's: the strings and hints back to the table's, string table
    // keys looked up (only GUI_CANCEL here), the rest kept as they are.
    for (jkGuiElement* el = menu->paElements; el->type != ELEMENT_END; el++)
    {
        el->hintText = el->origHintText;
        el->str = el->origStr;
        if (el->origStr && !strcmp(el->origStr, "GUI_CANCEL"))
            el->wstr = u"Cancel";
    }
}

static int (*wheelFn)(jkGuiMenu *pMenu, int32_t notches);
static jkGuiMenu* wheelMenu;
void jkGuiRend_SetMouseWheelHandler(jkGuiMenu *pMenu, int (*fn)(jkGuiMenu *pMenu, int32_t notches)) { wheelMenu = pMenu; wheelFn = fn; }
void jkGuiRend_MenuSetReturnKeyShortcutElement(jkGuiMenu *menu, jkGuiElement *element) { menu->pReturnKeyShortcutElement = element; }
void jkGuiRend_MenuSetEscapeKeyShortcutElement(jkGuiMenu *menu, jkGuiElement *element) { menu->pEscapeKeyShortcutElement = element; }

static void drawElement(jkGuiElement* el, jkGuiMenu* menu);

// What drawing a hidden element does in the game (jkGuiRend_RenderIdk2_alt
// in jkGUIRend.c, as SDL builds have it, before the guard added there for
// this): it looks for the next text box or list after the focused one, and
// draws the focused one again. With the focus on a hidden element, that
// drew it again forever. Counted here instead of overflowing the stack.
static int focusDepth = 0;
static int focusRunaway = 0;

static void drawHidden(jkGuiMenu* menu)
{
    if (focusDepth > 20)
    {
        focusRunaway = 1;
        return;
    }
    focusDepth++;
    jkGuiElement* focused = menu->focusedElement;
    int idx = focused ? (int)(focused - menu->paElements) : 0;
    int other = idx + 1;
    int found = 0;
    while (1)
    {
        jkGuiElement* it = &menu->paElements[other];
        if (it->type != ELEMENT_END)
        {
            if (it->bIsVisible && !it->enableHover && !(it->type < ELEMENT_LISTBOX || it->type > ELEMENT_TEXTBOX))
            {
                found = 1;
                break;
            }
        }
        else
        {
            other = -1;
        }
        if (++other == idx)
            break;
    }
    if (found)
    {
        jkGuiElement* el = &menu->paElements[other];
        if (focused && focused != el)
            drawElement(focused, menu);
        if (focused != el)
            drawElement(el, menu);
    }
    focusDepth--;
}

static void drawElement(jkGuiElement* el, jkGuiMenu* menu)
{
    CHECK(menu == displayed && menu, "draws only while the menu is up");
    draws++;
    if (el->bIsVisible)
    {
        if (el->drawFuncOverride)
            el->drawFuncOverride(el, menu, NULL, 1);
    }
    else
    {
        if (menu->lastMouseOverClickable == el)
            menu->lastMouseOverClickable = NULL;
        drawHidden(menu);
    }
}

void jkGuiRend_UpdateAndDrawClickable(jkGuiElement *clickable, jkGuiMenu *menu, BOOL forceRedraw)
{
    drawElement(clickable, menu);
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
}

void jkGuiRend_CopyVBuffer(jkGuiMenu *menu, rdRect *rect) {}
void jkGuiRend_DrawRect(tVBuffer *vbuf, rdRect *rect, __int16 color) {}

void jkGuiRend_ListBoxDraw(jkGuiElement *element, jkGuiMenu *menu, tVBuffer *vbuf, BOOL a4)
{
    listDraws++;
    listFonts[0] = menu->fonts[element->textType];
    listFonts[1] = menu->fonts[element->textType + 1];
    listPaddings = (int32_t*)menu->paddings;
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

// The test's part of each frame, after the menu's own (its idle function).
typedef void (*script_t)(jkGuiMenu* menu, jkGuiElement* el, int frame);
static script_t script = NULL;
static int frames = 0;

static int displays = 0;

int32_t jkGuiRend_DisplayAndReturnClicked(jkGuiMenu *menu)
{
    // As the game's (jkGuiRend_sub_50FAD0): no focus, nothing under the mouse.
    CHECK(!displayed, "one menu up at a time");
    displayed = menu;
    displays++;
    menu->lastClicked = 0;
    menu->focusedElement = NULL;
    menu->lastMouseOverClickable = NULL;
    jkGuiRend_Paint(menu);
    for (frames = 0; frames < 200 && !menu->lastClicked; frames++)
    {
        if (menu->idkFunc)
            menu->idkFunc(menu);
        if (!menu->lastClicked && script)
            script(menu, menu->paElements, frames);
    }
    CHECK(frames < 200, "the menu closed (frame %d)", frames);
    displayed = NULL;
    return menu->lastClicked;
}

// A click, as jkGuiRend_InvokeClicked makes one.
static void click(jkGuiMenu* menu, jkGuiElement* el, int redraw)
{
    if (!el->bIsVisible || el->enableHover || !el->clickHandlerFunc)
        return;
    menu->lastClicked = el->clickHandlerFunc(el, menu, 100, 100, redraw);
}

// ---- The page's side

static int js(const char* code)
{
    return emscripten_run_script_int(code);
}

static const char* jsText(const char* code)
{
    return emscripten_run_script_string(code);
}

static int called(const char* what)
{
    char code[512];
    snprintf(code, sizeof(code), "Module.jkLibrary._M.calls.indexOf(%s) >= 0 ? 1 : 0", what);
    return js(code);
}

static void forgetCalls()
{
    js("Module.jkLibrary._M.calls = [], 0");
}

// ---- Checks on what's shown

// Every line of a list fits its width: a line with a tab has a label before
// it and the rest from the stop at pad.
static void checkLinesFit(jkGuiElement* el, int pad, const char* what)
{
    int textW = el->rect.width - 12;
    for (int i = 0; el->unistr && el->unistr[i].str; i++)
    {
        const char16_t* s = el->unistr[i].str;
        int n = wlen(s), tab = -1;
        for (int j = 0; j < n; j++)
        {
            if (s[j] == '\t')
            {
                tab = j;
                break;
            }
        }
        if (tab >= 0 && pad > 0)
            CHECK(width(s + tab + 1, n - tab - 1) <= textW - pad, "%s line %d fits after its label: %s", what, i, show(s));
        else
            CHECK(width(s, n) <= textW, "%s line %d fits: %s", what, i, show(s));
    }
}

static int hasLine(jkGuiElement* el, const char* want)
{
    for (int i = 0; el->unistr && el->unistr[i].str; i++)
    {
        if (wis(el->unistr[i].str, want))
            return 1;
    }
    return 0;
}

// ---- The tests

static char name[32];

static void firstOpen(jkGuiMenu* menu, jkGuiElement* el, int frame)
{
    switch (frame)
    {
    case 0:
    {
        // What it shows once the page has the list.
        CHECK(wis(el[E_TITLE].wstr, "Level Library"), "title %s", show(el[E_TITLE].wstr));
        CHECK(wis(el[E_STATUS].wstr, "3 levels from Massassi.net"), "status %s", show(el[E_STATUS].wstr));
        // A message wider than its line is cut with "..." (the game would wrap it).
        CHECK(width(el[E_MESSAGE].wstr, wlen(el[E_MESSAGE].wstr)) <= el[E_MESSAGE].rect.width - 4, "the message fits its line");
        CHECK(wis(el[E_SORT_NAME].wstr, "Name"), "sort %s", show(el[E_SORT_NAME].wstr));
        CHECK(wis(el[E_CANCEL].wstr, "Cancel"), "Cancel's text from the string table");
        CHECK(wis(el[E_CTF].wstr, "CTF"), "Jedi Knight's CTF");
        CHECK(el[E_MP].selectedTextEntry == 1 && el[E_CTF].selectedTextEntry == 1 && el[E_COOP].selectedTextEntry == 0,
              "Multiplayer and CTF on, Co-op off, at first");
        CHECK(called("'query \"\" 3 0'"), "asked for the multiplayer and CTF levels by name");
        CHECK(rowCount(&el[E_LIST]) == 3, "3 rows, has %d", rowCount(&el[E_LIST]));
        {
            // With kinds in the list, two stops: the kinds' and the year's, at
            // 150, which the widest kind and a space end at.
            const int32_t* pads = (const int32_t*)menu->paddings;
            int kindX = 150 - width(u"CTF ", 4);
            CHECK(pads[0] == kindX && pads[1] == 150 && pads[2] == 0, "the stops: %d %d %d", pads[0], pads[1], pads[2]);
            // A row without a kind: its title, spaces past the kinds' stop,
            // then its year and size at the year's.
            const char16_t* r0 = el[E_LIST].unistr[0].str;
            int t0 = 0;
            while (r0[t0] && r0[t0] != '\t')
                t0++;
            CHECK(wstarts(r0, "Alpha Arena ") && width(r0, t0) > kindX && width(r0, t0) <= 150 && wis(r0 + t0, "\t1999 12 KB"),
                  "row 0: past the kinds' stop: %s", show(r0));
            // A title too wide for its column ends in "..." before the kinds'
            // stop; after the tab, the kind and spaces up to the year.
            const char16_t* r1 = el[E_LIST].unistr[1].str;
            int tab = 0;
            while (r1[tab] && r1[tab] != '\t')
                tab++;
            CHECK(tab > 3 && r1[tab - 1] == '.' && r1[tab - 2] == '.' && r1[tab - 3] == '.' && r1[tab - 4] != ' ', "row 1 cut: %s", show(r1));
            CHECK(width(r1, tab) <= kindX - 6, "row 1's title ends before the kinds' stop (%d)", width(r1, tab));
            CHECK(wstarts(r1, "* A Very Long"), "row 1 keeps its saved mark: %s", show(r1));
            CHECK(wis(r1 + tab, "\tCTF 2003 3.4 MB"), "row 1: its kind, a space, the year: %s", show(r1 + tab));
            CHECK(kindX + width(u"CTF ", 4) == 150, "so its year is at the year's stop");
            // A shorter kind: spaces up to the year's stop, as near as they go.
            const char16_t* r2 = el[E_LIST].unistr[2].str;
            int t2 = 0;
            while (r2[t2] && r2[t2] != '\t')
                t2++;
            int y2 = t2 + 1;
            while (r2[y2] && !(r2[y2] >= '0' && r2[y2] <= '9'))
                y2++;
            int yearAt = kindX + width(r2 + t2 + 1, y2 - t2 - 1);
            CHECK(wis(r2, "Gamma") == 0 && wstarts(r2, "Gamma\tSP ") && yearAt <= 150 && yearAt > 150 - SPACE_W && wis(r2 + y2, "2001 80 KB"),
                  "row 2: SP and spaces, its year at %d: %s", yearAt, show(r2));
        }
        CHECK(el[E_LIST].selectedTextEntry == 0, "row 0 picked");
        // Its Overview.
        CHECK(wis(el[E_LEVEL_TITLE].wstr, "Alpha Arena"), "level title %s", show(el[E_LEVEL_TITLE].wstr));
        CHECK(hasLine(&el[E_FACTS], "Author:\tTester"), "Author line");
        CHECK(hasLine(&el[E_FACTS], "Type:\tMultiplayer"), "Type line");
        int released = -1;
        for (int i = 0; el[E_FACTS].unistr[i].str; i++)
        {
            if (wstarts(el[E_FACTS].unistr[i].str, "Released:\tMay 2, 1999 (from its readme; Massassi"))
                released = i;
        }
        CHECK(released >= 0 && el[E_FACTS].unistr[released + 1].str[0] == '\t' && el[E_FACTS].unistr[released + 2].str[0] == '\t',
              "Released wraps after its label, its next lines in the value's column");
        CHECK(hasLine(&el[E_FACTS], "Last line."), "the last line");
        CHECK(hasLine(&el[E_FACTS], ""), "blank lines kept");
        checkLinesFit(&el[E_FACTS], 80, "facts");
        int longWord = 0;
        for (int i = 0; el[E_FACTS].unistr[i].str; i++)
            longWord += wstarts(el[E_FACTS].unistr[i].str, "xxxxxxxxxx");
        CHECK(longWord >= 2, "a word too long for a line is broken (%d lines)", longWord);
        CHECK(el[E_SHOT].bIsVisible && !el[E_NO_SHOT].bIsVisible && el[E_FACTS].bIsVisible && !el[E_TEXT].bIsVisible,
              "Overview: the screenshot frame and the facts");
        CHECK(el[E_SHOT].wHintText != NULL, "a hint for the next screenshot");
        CHECK(js("var s = Module.jkLibrary._M.lastShot; s && s.join() === '1,311,105,154,115' ? 1 : 0"),
              "the screenshot asked for inside the frame: %s", jsText("String(Module.jkLibrary._M.lastShot)"));
        // The 3D view's frame beside it, the same size, lined up with the
        // facts on the left and the tabs on the right.
        CHECK(el[E_SHOT].rect.x == el[E_FACTS].rect.x && el[E_PV].rect.x + el[E_PV].rect.width == el[E_FACTS].rect.x + el[E_FACTS].rect.width &&
              el[E_PV].rect.y == el[E_SHOT].rect.y && el[E_PV].rect.width == el[E_SHOT].rect.width && el[E_PV].rect.height == el[E_SHOT].rect.height &&
              el[E_PV].rect.x > el[E_SHOT].rect.x + el[E_SHOT].rect.width && el[E_LEVEL_TITLE].rect.y >= el[E_PV].rect.y + el[E_PV].rect.height,
              "the screenshot and the 3D view side by side, over the title");
        CHECK(el[E_TAB_FILES].rect.x + el[E_TAB_FILES].rect.width == el[E_PV].rect.x + el[E_PV].rect.width, "the 3D view ends where the tabs do");
        CHECK(el[E_PV].bIsVisible && el[E_PV_TEXT].bIsVisible && wis(el[E_PV_TEXT].wstr, "No 3D view for this level yet"),
              "no 3D view yet: it says so: %s", show(el[E_PV_TEXT].wstr));
        CHECK(el[E_PV].wHintText == NULL, "and no hint to click it");
        CHECK(js("var s = Module.jkLibrary._M.lastPreview; s && s.join() === '1,475,105,154,115' ? 1 : 0"),
              "the 3D view asked for inside its frame: %s", jsText("String(Module.jkLibrary._M.lastPreview)"));
        CHECK(wis(el[E_MINE].wstr, "My levels") && el[E_MINE].selectedTextEntry == 0 && el[E_MINE].wHintText,
              "My levels, off at first");
        CHECK(el[E_MINE].rect.x == el[E_CTF].rect.x && el[E_MINE].rect.y == el[E_COOP].rect.y, "My levels beside Co-op, under CTF");
        CHECK(el[E_TAB_OVERVIEW].type == ELEMENT_TEXT && el[E_TAB_OVERVIEW].textType == 3, "Overview is the open tab");
        CHECK(el[E_TAB_README].type == ELEMENT_TEXTBUTTON && el[E_TAB_README].textType == 2, "Readme is a button");
        CHECK(el[E_USE].bIsVisible && el[E_PAGE].bIsVisible, "Use Level and Massassi Page");
        CHECK(wis(el[E_MESSAGE].wstr, "One download at a time."), "message %s", show(el[E_MESSAGE].wstr));
        CHECK(menu->pReturnKeyShortcutElement == &el[E_USE] && menu->pEscapeKeyShortcutElement == &el[E_CANCEL],
              "Return is Use Level, Escape is Cancel");
        // The facts are text, not things to pick: no line lit, labels at 80.
        listDraws = 0;
        jkGuiRend_UpdateAndDrawClickable(&el[E_FACTS], menu, 1);
        CHECK(listDraws == 1 && listFonts[0] == listFonts[1], "the facts draw with no line lit");
        CHECK(listPaddings && listPaddings[0] == 80, "the facts' labels end at 80");
        jkGuiRend_UpdateAndDrawClickable(&el[E_LIST], menu, 1);
        forgetCalls();
        int before = draws;
        click(menu, &el[E_SORT_NEXT], 0);
        CHECK(draws > before, "the sort's name drawn again");
        break;
    }
    case 1:
        CHECK(called("'query \"\" 3 1'"), "sort: next");
        CHECK(wis(el[E_SORT_NAME].wstr, "Newest first"), "sort %s", show(el[E_SORT_NAME].wstr));
        click(menu, &el[E_SORT_PREV], 0);
        click(menu, &el[E_SORT_PREV], 0);
        break;
    case 2:
        CHECK(called("'query \"\" 3 5'"), "sort: back past the first to the last");
        CHECK(wis(el[E_SORT_NAME].wstr, "Smallest first"), "sort %s", show(el[E_SORT_NAME].wstr));
        click(menu, &el[E_SORT_NEXT], 0);
        // Typing in the search box (the game writes into its text).
        ((char16_t*)el[E_SEARCH].wstr)[0] = 'a';
        ((char16_t*)el[E_SEARCH].wstr)[1] = 'l';
        ((char16_t*)el[E_SEARCH].wstr)[2] = 0;
        CHECK(el[E_SEARCH].selectedTextEntry == 31, "the search box takes 31 letters");
        forgetCalls();
        break;
    case 3:
        CHECK(called("'query \"al\" 3 0'"), "search: %s", jsText("JSON.stringify(Module.jkLibrary._M.calls)"));
        forgetCalls();
        break;
    case 4:
        CHECK(!called("'query \"al\" 3 0'"), "no query when nothing changed");
        click(menu, &el[E_CTF], 0);
        CHECK(el[E_CTF].selectedTextEntry == 0, "CTF off");
        CHECK(called("'query \"al\" 1 0'"), "filters: multiplayer only");
        click(menu, &el[E_COOP], 0);
        CHECK(called("'query \"al\" 5 0'"), "filters: and co-op");
        click(menu, &el[E_COOP], 0);
        click(menu, &el[E_CTF], 0);
        CHECK(called("'query \"al\" 3 0'"), "filters: back");
        // My levels: the levels saved here, of the kinds ticked.
        click(menu, &el[E_MINE], 0);
        CHECK(el[E_MINE].selectedTextEntry == 1 && called("'query \"al\" 11 0'"), "My levels on: %s",
              jsText("JSON.stringify(Module.jkLibrary._M.calls)"));
        break;
    case 5:
        // Picking a row.
        el[E_FACTS].texInfo.textScrollY = 3;
        el[E_LIST].selectedTextEntry = 1;
        sounds = 0;
        click(menu, &el[E_LIST], 0);
        CHECK(called("'select 1'"), "select 1");
        CHECK(sounds == 1, "a click sound");
        break;
    case 6:
    {
        CHECK(wis(el[E_LEVEL_TITLE].wstr, "A Very Long Level Title That Goes On And On Past The Co..."), "the new level's title, cut to its line: %s",
              show(el[E_LEVEL_TITLE].wstr));
        CHECK(el[E_FACTS].texInfo.textScrollY == 0, "its facts from the top");
        // A row the page won't pick (busy, say): the list goes back to its pick.
        js("Module.jkLibrary._M.busy = true, 0");
        el[E_LIST].selectedTextEntry = 2;
        click(menu, &el[E_LIST], 0);
        CHECK(el[E_LIST].selectedTextEntry == 1, "the list stays on the page's pick");
        js("Module.jkLibrary._M.busy = false, 0");
        // Readme, after a click in the facts (which focuses them): the
        // hidden facts give the focus to the list.
        int before = paints;
        menu->focusedElement = &el[E_FACTS];
        click(menu, &el[E_TAB_README], 0);
        CHECK(paints > before, "a tab paints the menu again");
        CHECK(!focusRunaway, "no endless focus search with the facts hidden");
        CHECK(menu->focusedElement == &el[E_LIST], "the focus on the list");
        CHECK(el[E_TAB_README].type == ELEMENT_TEXT && el[E_TAB_OVERVIEW].type == ELEMENT_TEXTBUTTON, "Readme is the open tab");
        CHECK(!el[E_SHOT].bIsVisible && !el[E_FACTS].bIsVisible && el[E_TEXT].bIsVisible && !el[E_LEVEL_TITLE].bIsVisible,
              "Readme: the text, no screenshot");
        CHECK(js("Module.jkLibrary._M.lastShot[0]") == 0, "no screenshot over the Readme");
        CHECK(hasLine(&el[E_TEXT], "--- readme.txt ---") && hasLine(&el[E_TEXT], "A readme line."), "the readme's lines");
        checkLinesFit(&el[E_TEXT], 0, "readme");
        listDraws = 0;
        jkGuiRend_UpdateAndDrawClickable(&el[E_TEXT], menu, 1);
        CHECK(listDraws == 1 && listFonts[0] == listFonts[1] && !listPaddings, "the readme draws plain, no columns");
        // The wheel over the readme scrolls it, though the list has the focus.
        CHECK(wheelFn && wheelMenu == menu, "the menu takes the wheel");
        {
            jkGuiElement* t = &el[E_TEXT];
            int n = t->texInfo.numTextEntries, m = t->texInfo.maxTextEntries;
            t->texInfo.numTextEntries = 40;
            t->texInfo.maxTextEntries = 16;
            menu->lastMouseOverClickable = t;
            CHECK(wheelFn(menu, -1) == 1 && t->texInfo.textScrollY == 3 && t->selectedTextEntry == 3, "a notch down: 3 lines (%d)",
                  t->texInfo.textScrollY);
            CHECK(menu->focusedElement == &el[E_LIST], "the focus stays on the list");
            wheelFn(menu, -20);
            CHECK(t->texInfo.textScrollY == 40 - 14, "no further than its last line (%d)", t->texInfo.textScrollY);
            wheelFn(menu, 2);
            CHECK(t->texInfo.textScrollY == 40 - 14 - 6, "back up 2 notches (%d)", t->texInfo.textScrollY);
            wheelFn(menu, 50);
            CHECK(t->texInfo.textScrollY == 0 && t->selectedTextEntry == 0, "to the top");
            menu->lastMouseOverClickable = &el[E_FACTS];
            CHECK(wheelFn(menu, -1) == 0, "the hidden facts: not");
            menu->lastMouseOverClickable = NULL;
            t->texInfo.numTextEntries = n;
            t->texInfo.maxTextEntries = m;
        }
        {
            // Over the list of levels, without its focus: a notch is the next
            // or the previous level, as far as the ends.
            jkGuiElement* l = &el[E_LIST];
            l->texInfo.numTextEntries = rowCount(l);
            menu->focusedElement = NULL;
            menu->lastMouseOverClickable = l;
            forgetCalls();
            sounds = 0;
            CHECK(wheelFn(menu, -1) == 1 && l->selectedTextEntry == 2 && called("'select 2'"), "a notch down over the list: the next level (%d)",
                  l->selectedTextEntry);
            CHECK(sounds == 0, "no click sound for it");
            forgetCalls();
            CHECK(wheelFn(menu, -3) == 1 && l->selectedTextEntry == 2 && !called("'select 2'"), "no further than the last");
            CHECK(wheelFn(menu, 5) == 1 && l->selectedTextEntry == 0 && called("'select 0'"), "up to the first");
            CHECK(wheelFn(menu, 1) == 1 && l->selectedTextEntry == 0, "and no further");
            wheelFn(menu, -1);
            CHECK(l->selectedTextEntry == 1 && called("'select 1'"), "back to the one it was on");
            js("Module.jkLibrary._M.busy = true, 0");
            wheelFn(menu, -1);
            CHECK(l->selectedTextEntry == 1, "while a level comes in, the list stays on it");
            js("Module.jkLibrary._M.busy = false, 0");
            menu->lastMouseOverClickable = NULL;
            menu->focusedElement = &el[E_LIST];
        }
        break;
    }
    case 7:
        menu->focusedElement = &el[E_TEXT];
        click(menu, &el[E_TAB_FILES], 0);
        CHECK(menu->focusedElement == &el[E_TEXT], "the text keeps the focus from Readme to Files");
        CHECK(hasLine(&el[E_TEXT], "12 KB\talpha.gob  (episode: Alpha Arena)"), "the files");
        listDraws = 0;
        jkGuiRend_UpdateAndDrawClickable(&el[E_TEXT], menu, 1);
        CHECK(listPaddings && listPaddings[0] == 58, "the names after the sizes, at 58");
        click(menu, &el[E_TAB_OVERVIEW], 0);
        CHECK(el[E_SHOT].bIsVisible && el[E_FACTS].bIsVisible && !el[E_TEXT].bIsVisible, "back to Overview");
        CHECK(!focusRunaway && menu->focusedElement == &el[E_LIST], "the hidden text gives the focus to the list");
        break;
    case 8:
        CHECK(js("Module.jkLibrary._M.lastShot[0]") == 1, "the screenshot back");
        click(menu, &el[E_SHOT], 0);
        CHECK(js("Module.jkLibrary._M.nextShots") == 1, "a click on it: the next one");
        js("Module.jkLibrary._M.shots = 1, 0");
        click(menu, &el[E_SHOT], 0);
        CHECK(js("Module.jkLibrary._M.nextShots") == 1, "not with one screenshot");
        js("Module.jkLibrary._M.shots = 0, Module.jkLibrary._M.changed |= 4, 0");
        break;
    case 9:
        CHECK(el[E_NO_SHOT].bIsVisible, "\"No screenshot\" where there is none");
        CHECK(js("Module.jkLibrary._M.lastShot[0]") == 0, "and no picture over it");
        js("Module.jkLibrary._M.shots = 2, Module.jkLibrary._M.changed |= 4, 0");
        click(menu, &el[E_PAGE], 0);
        CHECK(js("Module.jkLibrary._M.pages") == 1, "Massassi Page");
        // Use Level, then Cancel while it comes in.
        click(menu, &el[E_USE], 0);
        CHECK(called("'use'"), "Use Level");
        break;
    case 10:
        CHECK(!el[E_USE].bIsVisible && !el[E_PAGE].bIsVisible, "no Use Level or Massassi Page while it comes in");
        CHECK(wis(el[E_MESSAGE].wstr, "Getting A Very Long Level Title That Goes On And On Past The Column (50%)"),
              "progress %s", show(el[E_MESSAGE].wstr));
        CHECK(wis(el[E_LEVEL_TITLE].wstr, "A Very Long Level Title That Goes On And On Past The Co..."), "the title still cut: %s",
              show(el[E_LEVEL_TITLE].wstr));
        click(menu, &el[E_CANCEL], 0);
        CHECK(menu->lastClicked == 0, "Cancel stops it and the library stays");
        break;
    case 11:
        CHECK(el[E_USE].bIsVisible, "Use Level again");
        CHECK(wis(el[E_MESSAGE].wstr, "One download at a time."), "message back");
        // A row that can't be used.
        el[E_LIST].selectedTextEntry = 2;
        click(menu, &el[E_LIST], 0);
        break;
    case 12:
        CHECK(!el[E_USE].bIsVisible && el[E_PAGE].bIsVisible, "a level with no episode: no Use Level, but its page");
        // A double click on one that can: Use Level, and it comes in.
        el[E_LIST].selectedTextEntry = 0;
        click(menu, &el[E_LIST], 0);
        break;
    case 13:
        js("Module.jkLibrary._M.useFinishes = true, 0");
        forgetCalls();
        click(menu, &el[E_LIST], 1);
        CHECK(called("'use'"), "a double click is Use Level");
        break;
    }
}

static void secondOpen(jkGuiMenu* menu, jkGuiElement* el, int frame)
{
    if (frame == 0)
    {
        CHECK(wis(el[E_SEARCH].wstr, "al"), "the search kept: %s", show(el[E_SEARCH].wstr));
        CHECK(el[E_MINE].selectedTextEntry == 1, "My levels kept");
        CHECK(called("'query \"al\" 11 0'"), "and asked again with them");
        click(menu, &el[E_MINE], 0);
        CHECK(called("'query \"al\" 3 0'"), "My levels off");
        CHECK(wis(el[E_SORT_NAME].wstr, "Name"), "the sort kept");
        CHECK(el[E_TAB_OVERVIEW].type == ELEMENT_TEXT, "the tab kept");
        click(menu, &el[E_CANCEL], 0);
        CHECK(menu->lastClicked == -1, "Cancel with nothing coming in closes it");
    }
}

// The 3D view's frame, then the big 3D view and back.
static jkGuiElement* libEl = NULL;
static int viewStep = 0;

static void viewOpen(jkGuiMenu* menu, jkGuiElement* el, int frame)
{
    if (!libEl)
        libEl = el;
    if (el == libEl && viewStep == 0)
    {
        switch (frame)
        {
        case 0:
            CHECK(wis(el[E_LEVEL_TITLE].wstr, "Alpha Arena"), "on Alpha Arena: %s", show(el[E_LEVEL_TITLE].wstr));
            click(menu, &el[E_PV], 0);
            CHECK(menu->lastClicked == 0, "no 3D view: a click on its frame does nothing");
            js("Module.jkLibrary._M.pv = 1, Module.jkLibrary._M.changed |= 32, 0");
            break;
        case 1:
            CHECK(el[E_PV_TEXT].bIsVisible && wis(el[E_PV_TEXT].wstr, "Loading the 3D view..."), "coming: %s", show(el[E_PV_TEXT].wstr));
            CHECK(el[E_PV].wHintText != NULL, "a hint to click it for the big view");
            js("Module.jkLibrary._M.pv = 2, Module.jkLibrary._M.changed |= 32, 0");
            break;
        case 2:
            CHECK(el[E_PV].bIsVisible && !el[E_PV_TEXT].bIsVisible && el[E_PV].wHintText != NULL, "there: the level, no text over it");
            CHECK(js("Module.jkLibrary._M.lastPreview[0]") == 1, "the page draws it in the frame");
            // How the player left the library, to find again after.
            el[E_FACTS].texInfo.textScrollY = 2;
            sounds = 0;
            click(menu, &el[E_PV], 0);
            CHECK(menu->lastClicked == 3 && sounds == 1, "a click on it: the big 3D view (%d)", menu->lastClicked);
            viewStep = 1;
            break;
        }
        return;
    }
    if (el != libEl && viewStep == 1)
    {
        switch (frame)
        {
        case 0:
            CHECK(js("Module.jkLibrary._M.lastShot[0]") == 0 && js("Module.jkLibrary._M.lastPreview[0]") == 0,
                  "the library's screenshot and small 3D view put away");
            CHECK(js("var s = Module.jkLibrary._M.lastView; s && s.join() === '1,21,59,598,310' ? 1 : 0"), "the big view asked for inside its frame: %s",
                  jsText("String(Module.jkLibrary._M.lastView)"));
            CHECK(js("Module.jkLibrary._M.views") == 1, "once");
            CHECK(!wheelFn, "the library's wheel off while the big view is up");
            CHECK(wis(el[V_TITLE].wstr, "Alpha Arena"), "the level's name over it: %s", show(el[V_TITLE].wstr));
            CHECK(el[V_TITLE].textType == 6, "in the big font (%d)", el[V_TITLE].textType);
            CHECK(wis(el[V_HELP].wstr, "Orbit: drag to turn, wheel to zoom."), "how to move: %s", show(el[V_HELP].wstr));
            CHECK(wis(el[V_NOTE].wstr, "3D view from the test."), "the note: %s", show(el[V_NOTE].wstr));
            CHECK(!el[V_TEXT].bIsVisible, "no text over the level");
            CHECK(wis(el[V_BACK].wstr, "Back") && el[V_BACK].bIsVisible, "Back");
            CHECK(el[V_MODE].bIsVisible && wis(el[V_MODE].wstr, "Walk"), "Walk: %s", show(el[V_MODE].wstr));
            CHECK(el[V_NEXT].bIsVisible && wis(el[V_NEXT].wstr, "Next Map"), "Next Map: %s", show(el[V_NEXT].wstr));
            CHECK(menu->pEscapeKeyShortcutElement == &el[V_BACK] && !menu->pReturnKeyShortcutElement, "Escape is Back, Return nothing");
            CHECK(el[V_BACK].rect.x == libEl[E_CANCEL].rect.x && el[V_BACK].rect.y == libEl[E_CANCEL].rect.y &&
                  el[V_NEXT].rect.x == libEl[E_USE].rect.x, "Back where Cancel was, the buttons along the bottom");
            CHECK(el[V_FRAME].rect.y > el[V_TITLE].rect.y && el[V_HELP].rect.y >= el[V_FRAME].rect.y + el[V_FRAME].rect.height &&
                  el[V_NOTE].rect.y + el[V_NOTE].rect.height <= el[V_HINT].rect.y, "title, frame, help, note, hint, top to bottom");
            forgetCalls();
            click(menu, &el[V_MODE], 0);
            CHECK(called("'viewCommand 1'") && menu->lastClicked == 0, "Walk: the page told, the view stays");
            CHECK(wis(el[V_MODE].wstr, "Orbit") && wis(el[V_NEXT].wstr, "Next Start"), "now Orbit and Next Start: %s, %s",
                  show(el[V_MODE].wstr), wis(el[V_NEXT].wstr, "Next Start") ? "Next Start" : "?");
            CHECK(wis(el[V_HELP].wstr, "Walk: W A S D, drag to look."), "how to walk: %s", show(el[V_HELP].wstr));
            click(menu, &el[V_NEXT], 0);
            CHECK(called("'viewCommand 2'") && menu->lastClicked == 0, "Next Start: the page told");
            break;
        case 1:
            menu->focusedElement = &el[V_MODE];
            // The next map, with a title too long for the big font.
            js("var M = Module.jkLibrary._M; M.pv = 1; M.titles[0] = M.titles[1] + ': ' + M.titles[1] + ' (2 of 3)'; M.changed |= 32; 0");
            break;
        case 2:
            CHECK(!el[V_MODE].bIsVisible, "while another map comes: no Walk");
            CHECK(el[V_NEXT].bIsVisible && wis(el[V_NEXT].wstr, "Next Map"), "Next Map stays: %s", show(el[V_NEXT].wstr));
            forgetCalls();
            click(menu, &el[V_NEXT], 0);
            CHECK(called("'viewCommand 2'") && menu->lastClicked == 0, "a press while it loads goes on to the next map");
            CHECK(el[V_TITLE].textType == 3, "the long title in the medium font (%d)", el[V_TITLE].textType);
            CHECK(width(el[V_TITLE].wstr, wlen(el[V_TITLE].wstr)) <= el[V_TITLE].rect.width - 4 && wlen(el[V_TITLE].wstr) > 0,
                  "and it fits: %s", show(el[V_TITLE].wstr));
            js("Module.jkLibrary._M.titles[0] = 'Alpha Arena', 0");
            CHECK(el[V_TEXT].bIsVisible && wis(el[V_TEXT].wstr, "Loading the 3D view..."), "and it says so: %s", show(el[V_TEXT].wstr));
            CHECK(menu->focusedElement == &el[V_BACK], "the focus from the hidden button to Back");
            // The map comes, and the library's list changes meanwhile.
            js("var M = Module.jkLibrary._M; M.pv = 2; M.rows.push('Delta\t2005 1 MB'); M.titles.push('Delta'); M.status = 'Updated';"
               "M.changed |= 1 | 2 | 32 | 64; 0");
            break;
        case 3:
            CHECK(el[V_MODE].bIsVisible && el[V_NEXT].bIsVisible && !el[V_TEXT].bIsVisible, "the map there: Walk and Next again");
            CHECK(el[V_TITLE].textType == 6 && wis(el[V_TITLE].wstr, "Alpha Arena"), "a short title back in the big font");
            CHECK(rowCount(&libEl[E_LIST]) == 3, "the library waits for the big view to close");
            CHECK(js("Module.jkLibrary._M.lastView[0]") == 1, "the big view kept up");
            click(menu, menu->pEscapeKeyShortcutElement, 0);
            CHECK(menu->lastClicked == 1, "Escape closes it");
            break;
        }
        return;
    }
    if (el == libEl && viewStep == 1)
    {
        switch (frame)
        {
        case 0:
            CHECK(js("Module.jkLibrary._M.lastView[0]") == 0, "the big view put away");
            CHECK(js("var s = Module.jkLibrary._M.lastPreview; s && s.join() === '1,475,105,154,115' ? 1 : 0") &&
                  js("Module.jkLibrary._M.lastShot[0]") == 1, "the screenshot and small 3D view back");
            CHECK(el[E_TAB_OVERVIEW].type == ELEMENT_TEXT && el[E_FACTS].bIsVisible && el[E_FACTS].texInfo.textScrollY == 2 &&
                  el[E_LIST].selectedTextEntry == 0, "the library as it was: its tab, its facts scrolled, its pick");
            CHECK(rowCount(&el[E_LIST]) == 4 && wis(el[E_STATUS].wstr, "Updated"), "with what changed meanwhile: %d rows, %s",
                  rowCount(&el[E_LIST]), show(el[E_STATUS].wstr));
            CHECK(wheelFn && wheelMenu == menu, "the wheel back");
            CHECK(menu->pEscapeKeyShortcutElement == &el[E_CANCEL] && menu->pReturnKeyShortcutElement == &el[E_USE], "Escape is Cancel again");
            js("Module.jkLibrary._M.pv = 3, Module.jkLibrary._M.changed |= 32, 0");
            break;
        case 1:
            CHECK(el[E_PV_TEXT].bIsVisible && wis(el[E_PV_TEXT].wstr, "The 3D view failed.") && el[E_PV].wHintText, "failed: %s",
                  show(el[E_PV_TEXT].wstr));
            forgetCalls();
            click(menu, &el[E_PV], 0);
            CHECK(menu->lastClicked == 0 && called("'viewCommand 3'"), "failed: a click tries again, no big view");
            // On Readme, no 3D view at all.
            click(menu, &el[E_TAB_README], 0);
            CHECK(!el[E_PV].bIsVisible && !el[E_PV_TEXT].bIsVisible && js("Module.jkLibrary._M.lastPreview[0]") == 0, "none over the Readme");
            click(menu, &el[E_TAB_OVERVIEW], 0);
            CHECK(el[E_PV].bIsVisible && js("Module.jkLibrary._M.lastPreview[0]") == 1, "back on the Overview");
            js("Module.jkLibrary._M.pv = 1, Module.jkLibrary._M.changed |= 32, 0");
            break;
        case 2:
            click(menu, &el[E_PV], 0);
            CHECK(menu->lastClicked == 3, "while it comes: the big view");
            viewStep = 2;
            break;
        }
        return;
    }
    if (el != libEl && viewStep == 2)
    {
        if (frame == 0)
        {
            CHECK(js("Module.jkLibrary._M.views") == 2, "the big view again");
            CHECK(el[V_TEXT].bIsVisible && wis(el[V_TEXT].wstr, "Loading the 3D view...") && !el[V_MODE].bIsVisible,
                  "it says it's coming: %s", show(el[V_TEXT].wstr));
            CHECK(el[V_NEXT].bIsVisible && wis(el[V_NEXT].wstr, "Next Map"), "with Next Map");
            CHECK(!el[V_FRAME].wHintText, "nothing to click in the frame");
            js("Module.jkLibrary._M.pv = 3, Module.jkLibrary._M.changed |= 32, 0");
        }
        else if (frame == 1)
        {
            CHECK(el[V_TEXT].bIsVisible && wis(el[V_TEXT].wstr, "The 3D view failed.") && !el[V_MODE].bIsVisible, "failed: %s",
                  show(el[V_TEXT].wstr));
            CHECK(el[V_NEXT].bIsVisible && wis(el[V_NEXT].wstr, "Next Map"), "Next Map still, to the level's other maps");
            CHECK(el[V_FRAME].wHintText != NULL, "a hint to click the frame");
            forgetCalls();
            click(menu, &el[V_FRAME], 0);
            CHECK(menu->lastClicked == 0 && called("'viewCommand 3'"), "a click on the frame tries again");
            click(menu, &el[V_BACK], 0);
            CHECK(menu->lastClicked == 1, "Back closes it");
            viewStep = 3;
        }
        return;
    }
    if (el == libEl && viewStep == 3 && frame == 0)
        click(menu, &el[E_CANCEL], 0);
}

static const char* ctfLabel = "";

static void gameOpen(jkGuiMenu* menu, jkGuiElement* el, int frame)
{
    if (frame == 0)
    {
        CHECK(wis(el[E_CTF].wstr, ctfLabel), "the CTF filter: %s", show(el[E_CTF].wstr));
        click(menu, &el[E_CANCEL], 0);
    }
}

static void emptyOpen(jkGuiMenu* menu, jkGuiElement* el, int frame)
{
    if (frame == 0)
    {
        CHECK(rowCount(&el[E_LIST]) == 0, "no rows");
        CHECK(!el[E_USE].bIsVisible && !el[E_PAGE].bIsVisible, "nothing to use");
        CHECK(el[E_LEVEL_TITLE].wstr && el[E_LEVEL_TITLE].wstr[0] == 0, "no level title");
        CHECK(rowCount(&el[E_FACTS]) <= 1, "no facts");
        click(menu, &el[E_SORT_NEXT], 0);
        click(menu, &el[E_TAB_FILES], 0);
        click(menu, &el[E_LIST], 1);
        click(menu, &el[E_USE], 0);
        click(menu, &el[E_SHOT], 0);
        click(menu, &el[E_PAGE], 0);
        click(menu, &el[E_TAB_OVERVIEW], 0);
        CHECK(!el[E_PV].bIsVisible && !el[E_PV_TEXT].bIsVisible && !el[E_SHOT].bIsVisible && !el[E_NO_SHOT].bIsVisible,
              "no level: no screenshot or 3D view frames");
        CHECK(js("Module.jkLibrary._M.lastShot[0]") == 0 && js("Module.jkLibrary._M.lastPreview[0]") == 0, "and the page draws neither");
        js("Module.jkLibrary._M.pv = 2, 0");
        click(menu, &el[E_PV], 0);
        CHECK(menu->lastClicked == 0, "and there's none to open");
        js("Module.jkLibrary._M.pv = 0, 0");
        click(menu, &el[E_CANCEL], 0);
    }
}

int main()
{
    for (int i = 0; i < 16; i++)
    {
        fonts[i].marginX = SPACE_W;
        fonts[i].marginY = SPACING;
        jkGui_stdFonts[i] = &fonts[i];
    }

    CHECK(jkGuiLibrary_IsAvailable() == 1, "available where the page says so");
    js("Module.jkLibrary._M.available = false, 0");
    CHECK(jkGuiLibrary_IsAvailable() == 0, "not where it doesn't");
    js("Module.jkLibrary._M.available = true, 0");

    script = firstOpen;
    int got = jkGuiLibrary_Show(0, name, sizeof(name));
    CHECK(!wheelFn, "the wheel let go when the library closes");
    CHECK(frames >= 14, "the first open ran its frames (%d)", frames);
    CHECK(!focusRunaway, "no endless focus search");
    CHECK(got == 1 && !strcmp(name, "alpha"), "gives back the level it got: %d \"%s\"", got, name);
    CHECK(called("'end'"), "the page told it's closed");
    CHECK(js("Module.jkLibrary._M.lastShot[0]") == 0, "no screenshot once it's closed");
    CHECK(js("Module.jkLibrary._M.result === '' ? 1 : 0"), "the name taken");

    forgetCalls();
    script = secondOpen;
    got = jkGuiLibrary_Show(0, name, sizeof(name));
    CHECK(got == 0 && name[0] == 0, "Cancel: nothing given back");

    forgetCalls();
    script = viewOpen;
    displays = 0;
    got = jkGuiLibrary_Show(0, name, sizeof(name));
    CHECK(got == 0 && viewStep == 3 && displays == 5, "the big 3D view twice, the library three times (%d, %d)", viewStep, displays);
    CHECK(js("Module.jkLibrary._M.lastView[0]") == 0 && js("Module.jkLibrary._M.lastPreview[0]") == 0, "no 3D view once it's closed");
    js("var M = Module.jkLibrary._M; M.rows.pop(); M.titles.pop(); M.status = ''; M.pv = 0; M.walk = false; 0");

    forgetCalls();
    script = gameOpen;
    ctfLabel = "Kill the Fool";
    got = jkGuiLibrary_Show(1, name, sizeof(name));
    CHECK(got == 0 && called("'begin mots'"), "MotS");
    ctfLabel = "CTF";
    got = jkGuiLibrary_Show(0, name, sizeof(name));
    CHECK(got == 0 && called("'begin jk1'"), "Jedi Knight again");

    js("var M = Module.jkLibrary._M; M.rows = []; M.sel = -1; 0");
    script = emptyOpen;
    got = jkGuiLibrary_Show(0, name, sizeof(name));
    CHECK(got == 0, "an empty list");

    js("Module.jkLibrary._M.beginOk = false, 0");
    script = NULL;
    frames = -1;
    got = jkGuiLibrary_Show(0, name, sizeof(name));
    CHECK(got == 0 && frames == -1, "no library: nothing shown, nothing given back");

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
