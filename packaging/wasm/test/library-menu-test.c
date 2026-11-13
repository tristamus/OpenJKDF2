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
    E_LEVEL_TITLE, E_FACTS, E_TEXT, E_MESSAGE, E_CANCEL, E_PAGE, E_USE, E_COUNT
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

void jkGuiRend_MenuSetReturnKeyShortcutElement(jkGuiMenu *menu, jkGuiElement *element) { menu->pReturnKeyShortcutElement = element; }
void jkGuiRend_MenuSetEscapeKeyShortcutElement(jkGuiMenu *menu, jkGuiElement *element) { menu->pEscapeKeyShortcutElement = element; }

static void drawElement(jkGuiElement* el, jkGuiMenu* menu)
{
    CHECK(menu == displayed && menu, "draws only while the menu is up");
    draws++;
    if (el->bIsVisible && el->drawFuncOverride)
        el->drawFuncOverride(el, menu, NULL, 1);
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
    {
        if (el->bIsVisible && el->drawFuncOverride)
            el->drawFuncOverride(el, menu, NULL, 0);
    }
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

int32_t jkGuiRend_DisplayAndReturnClicked(jkGuiMenu *menu)
{
    displayed = menu;
    menu->lastClicked = 0;
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
        CHECK(wis(el[E_SORT_NAME].wstr, "Name"), "sort %s", show(el[E_SORT_NAME].wstr));
        CHECK(wis(el[E_CANCEL].wstr, "Cancel"), "Cancel's text from the string table");
        CHECK(wis(el[E_CTF].wstr, "CTF"), "Jedi Knight's CTF");
        CHECK(el[E_MP].selectedTextEntry == 1 && el[E_CTF].selectedTextEntry == 1 && el[E_COOP].selectedTextEntry == 0,
              "Multiplayer and CTF on, Co-op off, at first");
        CHECK(called("'query \"\" 3 0'"), "asked for the multiplayer and CTF levels by name");
        CHECK(rowCount(&el[E_LIST]) == 3, "3 rows, has %d", rowCount(&el[E_LIST]));
        CHECK(wis(el[E_LIST].unistr[0].str, "Alpha Arena\t1999  12 KB"), "row 0 %s", show(el[E_LIST].unistr[0].str));
        // A title too wide for its column ends in "..." in it.
        const char16_t* r1 = el[E_LIST].unistr[1].str;
        int tab = 0;
        while (r1[tab] && r1[tab] != '\t')
            tab++;
        CHECK(tab > 3 && r1[tab - 1] == '.' && r1[tab - 2] == '.' && r1[tab - 3] == '.' && r1[tab - 4] != ' ', "row 1 cut: %s", show(r1));
        CHECK(width(r1, tab) <= 156, "row 1's title fits its column (%d)", width(r1, tab));
        CHECK(wstarts(r1, "* A Very Long"), "row 1 keeps its saved mark: %s", show(r1));
        CHECK(wis(r1 + tab, "\tCTF 2003  3.4 MB"), "row 1 keeps the rest: %s", show(r1 + tab));
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
        checkLinesFit(&el[E_FACTS], 72, "facts");
        int longWord = 0;
        for (int i = 0; el[E_FACTS].unistr[i].str; i++)
            longWord += wstarts(el[E_FACTS].unistr[i].str, "xxxxxxxxxx");
        CHECK(longWord >= 2, "a word too long for a line is broken (%d lines)", longWord);
        CHECK(el[E_SHOT].bIsVisible && !el[E_NO_SHOT].bIsVisible && el[E_FACTS].bIsVisible && !el[E_TEXT].bIsVisible,
              "Overview: the screenshot frame and the facts");
        CHECK(el[E_SHOT].wHintText != NULL, "a hint for the next screenshot");
        CHECK(js("var s = Module.jkLibrary._M.lastShot; s && s.join() === '1,311,105,318,178' ? 1 : 0"),
              "the screenshot asked for inside the frame: %s", jsText("String(Module.jkLibrary._M.lastShot)"));
        CHECK(el[E_TAB_OVERVIEW].type == ELEMENT_TEXT && el[E_TAB_OVERVIEW].textType == 3, "Overview is the open tab");
        CHECK(el[E_TAB_README].type == ELEMENT_TEXTBUTTON && el[E_TAB_README].textType == 2, "Readme is a button");
        CHECK(el[E_USE].bIsVisible && el[E_PAGE].bIsVisible, "Use Level and Massassi Page");
        CHECK(wis(el[E_MESSAGE].wstr, "One download at a time."), "message %s", show(el[E_MESSAGE].wstr));
        CHECK(menu->pReturnKeyShortcutElement == &el[E_USE] && menu->pEscapeKeyShortcutElement == &el[E_CANCEL],
              "Return is Use Level, Escape is Cancel");
        // The facts are text, not things to pick: no line lit, labels at 72.
        listDraws = 0;
        jkGuiRend_UpdateAndDrawClickable(&el[E_FACTS], menu, 1);
        CHECK(listDraws == 1 && listFonts[0] == listFonts[1], "the facts draw with no line lit");
        CHECK(listPaddings && listPaddings[0] == 72, "the facts' labels end at 72");
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
        CHECK(wis(el[E_LEVEL_TITLE].wstr, "A Very Long Level Title That Goes On And On Past The Column"), "the new level's title");
        CHECK(el[E_FACTS].texInfo.textScrollY == 0, "its facts from the top");
        // A row the page won't pick (busy, say): the list goes back to its pick.
        js("Module.jkLibrary._M.busy = true, 0");
        el[E_LIST].selectedTextEntry = 2;
        click(menu, &el[E_LIST], 0);
        CHECK(el[E_LIST].selectedTextEntry == 1, "the list stays on the page's pick");
        js("Module.jkLibrary._M.busy = false, 0");
        // Readme.
        int before = paints;
        click(menu, &el[E_TAB_README], 0);
        CHECK(paints > before, "a tab paints the menu again");
        CHECK(el[E_TAB_README].type == ELEMENT_TEXT && el[E_TAB_OVERVIEW].type == ELEMENT_TEXTBUTTON, "Readme is the open tab");
        CHECK(!el[E_SHOT].bIsVisible && !el[E_FACTS].bIsVisible && el[E_TEXT].bIsVisible && !el[E_LEVEL_TITLE].bIsVisible,
              "Readme: the text, no screenshot");
        CHECK(js("Module.jkLibrary._M.lastShot[0]") == 0, "no screenshot over the Readme");
        CHECK(hasLine(&el[E_TEXT], "--- readme.txt ---") && hasLine(&el[E_TEXT], "A readme line."), "the readme's lines");
        checkLinesFit(&el[E_TEXT], 0, "readme");
        listDraws = 0;
        jkGuiRend_UpdateAndDrawClickable(&el[E_TEXT], menu, 1);
        CHECK(listDraws == 1 && listFonts[0] == listFonts[1] && !listPaddings, "the readme draws plain, no columns");
        break;
    }
    case 7:
        click(menu, &el[E_TAB_FILES], 0);
        CHECK(hasLine(&el[E_TEXT], "12 KB\talpha.gob  (episode: Alpha Arena)"), "the files");
        listDraws = 0;
        jkGuiRend_UpdateAndDrawClickable(&el[E_TEXT], menu, 1);
        CHECK(listPaddings && listPaddings[0] == 58, "the names after the sizes, at 58");
        click(menu, &el[E_TAB_OVERVIEW], 0);
        CHECK(el[E_SHOT].bIsVisible && el[E_FACTS].bIsVisible && !el[E_TEXT].bIsVisible, "back to Overview");
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
        CHECK(called("'query \"al\" 3 0'"), "and asked again with it");
        CHECK(wis(el[E_SORT_NAME].wstr, "Name"), "the sort kept");
        CHECK(el[E_TAB_OVERVIEW].type == ELEMENT_TEXT, "the tab kept");
        click(menu, &el[E_CANCEL], 0);
        CHECK(menu->lastClicked == -1, "Cancel with nothing coming in closes it");
    }
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
    CHECK(frames >= 14, "the first open ran its frames (%d)", frames);
    CHECK(got == 1 && !strcmp(name, "alpha"), "gives back the level it got: %d \"%s\"", got, name);
    CHECK(called("'end'"), "the page told it's closed");
    CHECK(js("Module.jkLibrary._M.lastShot[0]") == 0, "no screenshot once it's closed");
    CHECK(js("Module.jkLibrary._M.result === '' ? 1 : 0"), "the name taken");

    forgetCalls();
    script = secondOpen;
    got = jkGuiLibrary_Show(0, name, sizeof(name));
    CHECK(got == 0 && name[0] == 0, "Cancel: nothing given back");

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
