#ifndef _JKHUD_H
#define _JKHUD_H

#include "types.h"
#include "globals.h"

#define jkHud_Startup_ADDR (0x00407500)
#define jkHud_Shutdown_ADDR (0x00407540)
#define jkHud_Open_ADDR (0x00407560)
#define jkHud_Close_ADDR (0x00407980)
#define jkHud_ClearRects_ADDR (0x00407A20)
#define jkHud_Draw_ADDR (0x00407C10)
#define jkHud_GetWeaponAmmo_ADDR (0x00408CC0)
#define jkHud_Chat_ADDR (0x00408D80)
#define jkHud_SendChat_ADDR (0x00408E50)
#define jkHud_chat2_ADDR (0x00409000)
#define jkHud_idk_time_ADDR (0x004090A0)
#define jkHud_SetTargetColors_ADDR (0x004090D0)
#define jkHud_SetTarget_ADDR (0x00409150)
#define jkHud_EndTarget_ADDR (0x00409170)
#define jkHud_SortPlayerScore_ADDR (0x00409180)
#define jkHud_SortTeamScore_ADDR (0x004091A0)
#define jkHud_Tally_ADDR (0x004091C0)

#ifdef QOL_IMPROVEMENTS
BOOL jkHud_shouldCrosshairBeShownForWeapon(SithThing *player);
#endif // DEBUG

// Added: what the status bars show (jkHud_DrawGPU): this game's player's,
// or, for a game watching the match that follows a player in first person,
// that player's, as their game tells the room's feed (stdComm_WSRelay.c).
typedef struct jkHudValues
{
    int health;
    int maxHealth;
    int shields;
    int force;
    int battery;
    int ammo; // -999: the weapon takes none
    int flags; // JKHUD_VALUES_*
} jkHudValues;
#define JKHUD_VALUES_FIELDLIGHT (0x1)
#define JKHUD_VALUES_FORCESURGE (0x2)
#define JKHUD_VALUES_SUPERSHIELDS (0x4)
#define JKHUD_VALUES_SUPERWEAPON (0x8)
#define JKHUD_VALUES_CROSSHAIR (0x10) // the crosshair shows, as the player set it and for their weapon
#define JKHUD_VALUES_DEAD (0x20)
void jkHud_LocalValues(SithThing* v4, jkHudValues* pOut);

int jkHud_Startup();
void jkHud_Shutdown();
MATH_FUNC int jkHud_Open();
void jkHud_Close();
int jkHud_ClearRects(int unk);
MATH_FUNC void jkHud_Draw();
MATH_FUNC void jkHud_DrawGPU();
int jkHud_GetWeaponAmmo(SithThing *player);
int jkHud_Chat();
void jkHud_SendChat(char a1);
#ifdef ARCH_WASM
void jkHud_SayFromPage(const char* pStr); // Added: the touch controls' Message
#endif
void jkHud_SetTargetColors(int *color_idxs);
void jkHud_SetTarget(SithThing *target);
void jkHud_EndTarget();
int jkHud_SortPlayerScore(const void* a, const void* b);
int jkHud_SortTeamScore(const void* a, const void* b);
void jkHud_Tally();
void jkHud_idk_time();
int jkHud_chat2();

//static int (*jkHud_Startup)() = (void*)jkHud_Startup_ADDR;
//static void (*jkHud_Shutdown)() = (void*)jkHud_Shutdown_ADDR;
//static void (*jkHud_Chat)() = (void*)jkHud_Chat_ADDR;
//static void (*jkHud_Tally)() = (void*)jkHud_Tally_ADDR;

//static int (*jkHud_ClearRects)() = (void*)jkHud_ClearRects_ADDR;
//static void (*jkHud_idk_time)() = (void*)jkHud_idk_time_ADDR;
//static void (*jkHud_SendChat)(char a1) = (void*)jkHud_SendChat_ADDR;
//static void (*jkHud_Draw)() = (void*)jkHud_Draw_ADDR;
//static void (*jkHud_Close)() = (void*)jkHud_Close_ADDR;
//static void (*jkHud_Open)() = (void*)jkHud_Open_ADDR;

#endif // _JKHUD_H
