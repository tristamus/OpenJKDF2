#ifndef _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAY_H
#define _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAY_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PLATFORM_WSRELAY

// What stdComm_Open returns when the engine has to restart into the host's
// mod before joining (g_should_exit is already set).
#define STDCOMM_WSRELAY_RESTART (0x4A4B5752)

void stdComm_WSRelay_Startup();
void stdComm_WSRelay_Shutdown();

// Writes a line about the mod of listed game idx for the join screen, empty
// if there's nothing to say.
void stdComm_WSRelay_DescribeMod(int idx, char16_t* pOut, int maxChars);

#endif // PLATFORM_WSRELAY

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAY_H
