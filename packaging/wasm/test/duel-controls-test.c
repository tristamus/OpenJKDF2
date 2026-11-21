// Compile the real control tables and functions with the engine's own types.
// Only device state, the file handle, and the final gameplay command are fake.
#include "types.h"
#include <assert.h>
#include <ctype.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _memset memset
#define _sscanf sscanf
#define _strcmp strcmp
#define _strtok strtok
#define _strchr strchr
#define _atoi atoi
#define _atof atof
#define SITH_ASSERTREL assert

static int sithControl_inputFuncToControlType[INPUT_FUNC_MAX];
static stdControlKeyInfo sithControl_aInputFuncToKeyinfo[INPUT_FUNC_MAX];
int32_t Main_bMotsCompat;
static int g_debugmodeFlags, stdControl_bControlsIdle, stdControl_bDisableKeyboard;
static int stdControl_bControlsActive = 1;
static int stdControl_aKeyInfo[JK_NUM_KEYS], stdControl_aKeyPressed[JK_NUM_KEYS];
static unsigned int sithWeapon_controlOptions;
static StdConffileEntry stdConffile_g_entry;
static char stdConffile_g_aLine[256];
static FILE *profile;

static void stdControl_Reset(void) {}
static int stdConffile_ReadLine(void) { return fgets(stdConffile_g_aLine, sizeof(stdConffile_g_aLine), profile) != NULL; }
static int stdConffile_WriteLine(const char *s) { return fputs(s, profile) >= 0; }
static int stdConffile_Printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfprintf(profile, format, args);
    va_end(args);
    return result >= 0;
}
// Physical mouse axes have no device here. Buttons take the engine's key path.
static stdControlKeyInfoEntry *sithControl_BindAxis(int func, int axis, uint32_t flags)
{
    (void)func; (void)axis; (void)flags;
    return NULL;
}

#ifdef PLATFORM_WSRELAY
static int stdComm_WSRelay_bTestInput, jkHud_bChatOpen, jkQuakeConsole_bOpen;
static int phase, commands, messages;
static int sithNet_isMulti = 1, jkHud_dword_552D10 = -1, playerThingIdx;
static char std_g_genBuffer[256], sithCommand_aTmpCommandExecute[512], lastArgs[64], lastChat[64];
#ifdef PLATFORM_DROIDWORKS
int32_t Main_bDroidWorks;
static int dwGuiScreen_ExecCheatCode(const char *line) { (void)line; return 0; }
#endif
#define __strnicmp strncasecmp
#define _sprintf sprintf
static void jkDev_DebugLog(const char *line) { (void)line; }
static int jkDev_TryCommand(const char *line) { (void)line; return 0; }
static void sithConsole_ExeCommand(const char *line) { (void)line; abort(); }
static void sithMulti_SendChat(const char *line, int target, int slot)
{
    assert(target == -1 && slot == playerThingIdx);
    snprintf(lastChat, sizeof(lastChat), "%s", line);
    messages++;
}
static SithThing self, other;
static SithThing *sithPlayer_g_pLocalPlayerThing = &self;
static int stdComm_WSRelay_TestKey(int func, int *presses)
{
    assert(func == INPUT_FUNC_DUEL && presses);
    *presses = 1; // scripted quick tap bypasses physical devices
    return 0;
}
static int sithDuelCombat_Phase(const SithThing *player) { assert(player == &self); return phase; }
static int sithDuel_Command(stdDebugConsoleCmd *command, const char *args)
{
    assert(command == NULL && args);
    snprintf(lastArgs, sizeof(lastArgs), "%s", args);
    commands++;
    return 1;
}
#endif

#include "control-functions.inc"

#ifdef PLATFORM_WSRELAY
static void checkLabel(const char *path)
{
    FILE *table = fopen(path, "r");
    assert(table);
    int declared = 0, entries = 0, matches = 0;
    char line[512], key[128], value[300];
    assert(fgets(line, sizeof(line), table) && sscanf(line, "MSGS %d", &declared) == 1);
    while (fgets(line, sizeof(line), table)) {
        if (sscanf(line, " \"%127[^\"]\" %*d \"%299[^\"]", key, value) != 2) continue;
        entries++;
        if (!strcmp(key, sithControl_aFunctionStrs[INPUT_FUNC_DUEL])) {
            assert(!strcmp(value, "Challenge to a duel"));
            matches++;
        }
    }
    fclose(table);
    assert(matches == 1 && entries == declared);
}

static int rows, listedKey;
static int listRow(int32_t func, const char *name, uint32_t type, int32_t index,
                   uint32_t key, int32_t flags, stdControlKeyInfoEntry *entry, Darray *unused)
{
    (void)unused;
    if (func == INPUT_FUNC_DUEL) {
        assert(!strcmp(name, "DUEL") && type == 5 && flags == INPUT_MAPPING_FLAG_DXKEY);
        assert((index < 0) == (entry == NULL));
        listedKey = entry ? (int)key : -1;
        rows++;
    }
    return 1;
}

static void checkMenu(int keyboard, int expected)
{
    rows = 0;
    listedKey = -2;
    // The same enumeration filters as Setup > Controls > Keyboard and Mouse.
    sithControl_EnumBindings(listRow, keyboard, 0, !keyboard, NULL);
    assert(rows == 1 && listedKey == expected);
}

static void frame(SithThing *player, int keyDown, int keyPress, int mouseDown, int mousePress)
{
    stdControl_aKeyInfo[DIK_K] = keyDown;
    stdControl_aKeyPressed[DIK_K] = keyPress;
    stdControl_aKeyInfo[KEY_MOUSE_B4] = mouseDown;
    stdControl_aKeyPressed[KEY_MOUSE_B4] = mousePress;
    sithControl_DuelKey(player);
}

static void roundTrip(void)
{
    profile = tmpfile();
    assert(profile && sithControl_WriteConf());
    rewind(profile);
    char header[100];
    unsigned int savedFlags;
    int marker = 0;
    assert(fgets(header, sizeof(header), profile));
    assert(sscanf(header, "flags=%x dueldefault=%d", &savedFlags, &marker) == 2);
    assert(savedFlags == sithWeapon_controlOptions && marker == 1);
    rewind(profile);
    sithControl_Reset();
    sithWeapon_controlOptions = 0;
    assert(sithControl_ReadConf());
    fclose(profile);
}

// Old profiles contain only their actual bindings. Load through the same
// tokenizer and ReadConf as the game, independently of today's writer.
static void loadProfile(const char *header, int action, int key)
{
    profile = tmpfile();
    assert(profile);
    fprintf(profile, "%s\n", header);
    if (action >= 0) fprintf(profile, "bind %d %d 0x2\n", action, key);
    fputs("end.\n", profile);
    rewind(profile);
    assert(sithControl_ReadConf());
    fclose(profile);
}

static void checkProfiles(void)
{
    loadProfile("flags=24", -1, 0);
    checkMenu(1, DIK_K);
    roundTrip();
    checkMenu(1, DIK_K);
    loadProfile("flags=24 unrelated=1 dueldefault=0", -1, 0);
    checkMenu(1, DIK_K);
    loadProfile("flags=24 unrelated=1 dueldefault=1", -1, 0);
    checkMenu(1, -1);
    assert(!sithControl_IsKeyBound(DIK_K));

    loadProfile("flags=24", INPUT_FUNC_JUMP, DIK_K);
    assert(sithControl_aInputFuncToKeyinfo[INPUT_FUNC_JUMP].numEntries == 1);
    assert(sithControl_aInputFuncToKeyinfo[INPUT_FUNC_JUMP].aEntries[0].dxKeyNum == DIK_K);
    checkMenu(1, -1);
    checkMenu(0, -1);

    loadProfile("flags=24", INPUT_FUNC_DUEL, KEY_MOUSE_B4);
    checkMenu(1, -1);
    checkMenu(0, KEY_MOUSE_B4);
    assert(!sithControl_IsKeyBound(DIK_K));

    // The player explicitly removes K with both K and the duel now free.
    // Reopening the profile must preserve that choice.
    sithControl_Reset();
    sithControl_RegisterKeyboardBindings();
    sithControl_UnbindControl(INPUT_MAPPING_FLAG_DXKEY, DIK_K);
    roundTrip();
    checkMenu(1, -1);
    checkMenu(0, -1);
    assert(!sithControl_IsKeyBound(DIK_K));
}

static void checkConsoleAliases(void)
{
    // A bind loaded from binds.json and one added from the console share
    // this list. The engine's query must work before the list is allocated.
    assert(!sithCommand_IsKeyBound('k') && !sithCommand_IsKeyBound('K'));
    sithCommandBind binding = { .pCmd = "players" };
    sithCommandBind head = { .pNext = &binding };
    for (int i = 0; i < 2; i++) {
        binding.key = i ? 'K' : 'k';
        sithCommand_pBinds = &head;
        assert(sithCommand_IsKeyBound(binding.key) && !sithCommand_IsKeyBound('x'));
        sithControl_Reset();
        sithControl_RegisterKeyboardBindings();
        assert(!sithControl_IsKeyBound(DIK_K));
        loadProfile("flags=24", -1, 0);
        checkMenu(1, -1);

        // An alias added *after* the default takes precedence at input time.
        // Its presence does not disable a second duel binding on the mouse.
        sithCommand_pBinds = NULL;
        sithControl_RegisterKeyboardBindings();
        assert(sithControl_BindControl(INPUT_FUNC_DUEL, KEY_MOUSE_B4, 0));
        sithCommand_pBinds = &head;
        int before = commands;
        frame(&self, 0, 1, 0, 0); assert(commands == before);
        frame(&self, 0, 0, 0, 1); assert(commands == before + 1);
        stdComm_WSRelay_bTestInput = 1;
        frame(&self, 0, 0, 0, 0); assert(commands == before + 2);
        stdComm_WSRelay_bTestInput = 0;
        sithCommand_pBinds = NULL;
        frame(&self, 0, 1, 0, 0); assert(commands == before + 3);
        assert(!lastArgs[0]);
        assert(sithControl_BindControl(INPUT_FUNC_JUMP, DIK_K, 0));
        sithCommand_pBinds = &head;
        int presses = 0;
        sithControl_GetKey(INPUT_FUNC_JUMP, &presses);
        assert(presses == 1); // console precedence applies only to duel's K entry
        sithCommand_pBinds = NULL;
    }
    commands = 0;
}

static void checkConsoleRouting(void)
{
    // Legacy console binds execute through this exact entry point. The HUD
    // chat mode remains set to public after a player finishes talking.
    sithControl_Reset();
    sithControl_RegisterKeyboardBindings();
    checkMenu(1, DIK_K);
    sithCommandBind binding = { .key = 'k', .pCmd = "duel" };
    sithCommandBind head = { .pNext = &binding };
    sithCommand_pBinds = &head;
    sithCommand_HandleBinds('k');
    frame(&self, 1, 1, 0, 0); // the same press reaches the physical controls
    assert(commands == 1 && !lastArgs[0] && messages == 0);
    jkQuakeConsole_ExecuteCommand("DuEL\taccept");
    assert(commands == 2 && !strcmp(lastArgs, "\taccept") && messages == 0);
    jkQuakeConsole_ExecuteCommand("/duel status");
    assert(commands == 3 && !strcmp(lastArgs, " status") && messages == 0);
    jkQuakeConsole_ExecuteCommand("dueling tonight");
    assert(commands == 3 && messages == 1 && !strcmp(lastChat, "dueling tonight"));
    jkQuakeConsole_ExecuteCommand("/dueling");
    assert(commands == 3 && messages == 2 && !strcmp(lastChat, "/dueling"));
    assert(!sithDuel_Chat("duel")); // in normal HUD chat a plain word is text
    sithCommand_pBinds = NULL;
    commands = 0;
}

static void checkDispatch(void)
{
    frame(&self, 1, 1, 0, 0); assert(commands == 1 && !lastArgs[0]); // aimed challenge/accept
    frame(&self, 1, 0, 0, 0); assert(commands == 1); // held
    frame(&self, 0, 0, 0, 0); assert(commands == 1); // release
    frame(&self, 0, 1, 0, 0); assert(commands == 2); // both edges in one frame
    frame(&self, 0, 0, 0, 1); assert(commands == 3); // quick mouse tap
    frame(&self, 1, 1, 1, 1); assert(commands == 4); // two binds, one action
    jkHud_bChatOpen = 1;
    frame(&self, 0, 1, 0, 0); assert(commands == 4);
    jkHud_bChatOpen = 0;
    jkQuakeConsole_bOpen = 1;
    frame(&self, 0, 0, 0, 1); assert(commands == 4);
    jkQuakeConsole_bOpen = 0;
    frame(&other, 0, 1, 0, 0); assert(commands == 4);
    for (phase = 1; phase <= 2; phase++) {
        frame(&self, 0, 1, 0, 0); assert(commands == 4);
    }
    phase = 0;
    frame(&self, 0, 1, 0, 0); assert(commands == 5);
}
#endif

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    sithControl_RegisterControlFunctions();
    assert(INPUT_FUNC_ACTIVATE31 == 73);
    assert(!strcmp(sithControl_aFunctionStrs[INPUT_FUNC_ACTIVATE31], "ACTIVATE31"));
    assert(!strcmp(sithControl_aFunctionStrs[INPUT_FUNC_MAX], "INPUT_FUNC_MAX"));
#ifdef QOL_IMPROVEMENTS
    assert(INPUT_FUNC_USELASTSELECTED == 74 && !strcmp(sithControl_aFunctionStrs[74], "USELASTSELECTED"));
    assert(sithControl_inputFuncToControlType[74] == 5);
#endif
#ifdef PLATFORM_WSRELAY
    assert(INPUT_FUNC_DUEL == 75 && INPUT_FUNC_MAX == 76);
#ifndef QOL_IMPROVEMENTS
    assert(!sithControl_inputFuncToControlType[74]); // reserved prior optional slot
#endif
    checkLabel(argv[1]);
    for (Main_bMotsCompat = 0; Main_bMotsCompat <= 1; Main_bMotsCompat++) {
        sithControl_Reset();
        sithControl_RegisterKeyboardBindings();
        sithControl_RegisterMouseBindings();
        assert(sithControl_aInputFuncToKeyinfo[INPUT_FUNC_DUEL].numEntries == 1);
        checkMenu(1, DIK_K);
        checkMenu(0, -1);
    }
    checkProfiles();
    checkConsoleAliases();
    checkConsoleRouting();
    sithControl_Reset();
    assert(sithControl_BindControl(INPUT_FUNC_DUEL, DIK_K, 0));
    assert(sithControl_BindControl(INPUT_FUNC_DUEL, KEY_MOUSE_B4, 0));
    assert(sithControl_BindControl(INPUT_FUNC_JUMP, DIK_SPACE, 0)); // preserve other actions
    sithWeapon_controlOptions = 0x24;
    roundTrip();
    assert(sithWeapon_controlOptions == 0x24 && sithControl_aInputFuncToKeyinfo[INPUT_FUNC_DUEL].numEntries == 2);
    assert(sithControl_aInputFuncToKeyinfo[INPUT_FUNC_JUMP].aEntries[0].dxKeyNum == DIK_SPACE);
    checkMenu(1, DIK_K);
    checkMenu(0, KEY_MOUSE_B4);
    checkDispatch();
    puts("Browser duel control: defaults, both menus, profile, key and mouse dispatch passed.");
#else
#ifdef QOL_IMPROVEMENTS
    assert(INPUT_FUNC_MAX == 75);
#else
    assert(INPUT_FUNC_MAX == 74);
#endif
    for (int i = 0; i < INPUT_FUNC_MAX; i++)
        assert(strcmp(sithControl_aFunctionStrs[i], "DUEL"));
    for (Main_bMotsCompat = 0; Main_bMotsCompat <= 1; Main_bMotsCompat++) {
        sithControl_Reset();
        sithControl_RegisterKeyboardBindings();
        assert(!sithControl_IsKeyBound(DIK_K));
    }
    // A shared browser profile has action 75. Native loads its existing
    // bindings and skips the action outside its own table.
    profile = tmpfile();
    assert(profile);
    fprintf(profile, "flags=24 dueldefault=1\nbind 75 %d 0x2\nbind %d %d 0x2\nend.\n",
            DIK_K, INPUT_FUNC_JUMP, DIK_SPACE);
    rewind(profile);
    assert(sithControl_ReadConf());
    fclose(profile);
    assert(sithWeapon_controlOptions == 0x24 && !sithControl_IsKeyBound(DIK_K) && sithControl_IsKeyBound(DIK_SPACE));
    puts("Native controls preserved; browser-only binding skipped.");
#endif
}
