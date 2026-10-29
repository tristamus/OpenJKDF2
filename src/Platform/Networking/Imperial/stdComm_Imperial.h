#ifndef _PLATFORM_NETWORKING_IMPERIAL_STDCOMM_IMPERIAL_H
#define _PLATFORM_NETWORKING_IMPERIAL_STDCOMM_IMPERIAL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PLATFORM_WSRELAY

// The multiplayer menu's second connection: games on an Imperial Network
// Protocol server, the desktop OpenJKDF2 relay (jkrelay_client.c), which
// browsers reach over a WebSocket (jkrelay_server -w). stdComm_WSRelay.c hands
// each call here while it's the picked connection.
#define STDCOMM_IMPERIAL_CONNECTION (1)

extern int stdComm_Imperial_bActive;

void stdComm_Imperial_Startup();
void stdComm_Imperial_Idle();
int stdComm_Imperial_OpenConnection();
void stdComm_Imperial_CloseConnection();
int stdComm_Imperial_EnumSessions();
int stdComm_Imperial_Open(int idx, char16_t* pwPassword);
void stdComm_Imperial_Close();
int stdComm_Imperial_OpenHost(jkMultiEntry* pEntry);
int stdComm_Imperial_GetSession_passwordidk(jkMultiEntry* pEntry);
int stdComm_Imperial_IdkSessionDesc(jkMultiEntry* pEntry);
void stdComm_Imperial_SetSessionDesc(const char* pJklFname, DWORD maxPlayers);
BOOL stdComm_Imperial_SetSessionFlagidk(int bJoinable);
void stdComm_Imperial_EnumPlayers();
BOOL stdComm_Imperial_Send(DPID idFrom, DPID idTo, void* lpData, DWORD dwDataSize);
int stdComm_Imperial_Receive(int* pIdOut, int* pMsgIdOut, int* pLenOut);
void stdComm_Imperial_DescribeHits(int idx, int bShort, char16_t* pOut, int maxChars);

#endif // PLATFORM_WSRELAY

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_NETWORKING_IMPERIAL_STDCOMM_IMPERIAL_H
