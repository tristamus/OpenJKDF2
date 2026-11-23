/* Integration fixture: real production duel modules and engine types.
 * Engine-world setup/callbacks and transport are supplied here; no duel state
 * transition, request validation, eligibility, isolation or reward is mocked.
 * This is not the full engine, renderer, COG VM, or production DSS decoder.
 */
#include "Gameplay/sithDuel.h"
#include "Gameplay/sithDuelState.h"
#include "Gameplay/sithDuelCombat.h"
#include "Gameplay/sithInventory.h"
#include "Gameplay/jkSaber.h"
#include "Devices/sithComm.h"
#include "Devices/sithConsole.h"
#include "Dss/sithMulti.h"
#include "Engine/sithCollision.h"
#include "Engine/sithPhysics.h"
#include "Primitives/rdMatrix.h"
#include "Primitives/rdVector.h"
#include "World/sithThing.h"
#include "World/sithWeapon.h"
#include "General/stdString.h"
#include "stdPlatform.h"
#include "globals.h"
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int32_t sithNet_isMulti = 1, sithNet_isServer, sithNet_MultiModeFlags;
int32_t sithNet_bNeedsFullThingSyncForLeaveJoin, sithMulti_newPlayerId;
uint32_t sithMulti_curWelcomePlayerNum, jkPlayer_maxPlayers = 3;
int stdComm_dplayIdSelf, stdComm_bIsServer, stdComm_Imperial_bActive;
int stdComm_WSRelay_bViewer, jkGuiNetHost_bIsDedicated, playerThingIdx;
int Main_bMotsCompat, sithWeapon_8BD024 = -1;
int sithInventory_bUnkPower, sithInventory_g_bSendDeactivateMessage;
SithControlBinding sithInventory_powerKeybinds[SITHINVENTORY_NUM_POWERKEYBINDS];
SithPlayer jkPlayer_playerInfos[JKPLAYER_NUM_INFOS];
SithInventoryType sithInventory_g_aTypes[SITHBIN_NUMBINS];
SithThing *sithPlayer_g_pLocalPlayerThing;
jkEpisodeLoad jkEpisode_mLoad;

static SithThing people[3];
static SithSector sector;
static sithCog saberCog, jumpCog;
static uint32_t clockMs = 1000;
static int connected, sight = 1, matchOn = 1;
static int disables, weaponResets, movementResets, inventoryWrites;
static int aimTarget, aimWorld, aimCount, aimNext, aimSearches, aimReleases;
static SithCollision aimContacts[2];
static uint8_t lastSnapshot[DUEL_WIRE_SIZE];
static DuelState observed, observedPairs[DUEL_MAX_PAIRS];
static DuelHeader observedHeader;
static char report[16384];
static size_t used;

static int allwrite(const void *buf, size_t n)
{
    const unsigned char *p = buf;
    while (n) {
        ssize_t k = send(connected, p, n, MSG_NOSIGNAL);
        if (k <= 0) return 0;
        p += k; n -= (size_t)k;
    }
    return 1;
}

static int allread(void *buf, size_t n)
{
    unsigned char *p = buf;
    while (n) {
        ssize_t k = recv(connected, p, n, 0);
        if (k <= 0) return 0;
        p += k; n -= (size_t)k;
    }
    return 1;
}

/* The router reads only routing headers. Sender identity is assigned from
 * the connected peer, never taken from an action body or asserted by a peer.
 */
static void envelope(uint32_t kind, uint32_t to, const void *data, uint32_t n)
{
    uint32_t head[3] = {kind, to, n};
    if (!allwrite(head, sizeof(head)) || (n && !allwrite(data, n))) exit(4);
}

/* "last" is the newest change to any pair, as a peer saw it; a repeat of a
 * pair's current state is not a change. */
static void observe(const unsigned char *data, size_t n)
{
    DuelHeader h = {0};
    if (sithDuelState_ReadHeader(&h, data, n, DUEL_HOST_ID)) {
        observedHeader = h;
        return;
    }
    DuelState parsed = {0};
    if (!sithDuelState_ReadSnapshot(&parsed, data, n, DUEL_HOST_ID))
        return;
    DuelState *seen = &observedPairs[parsed.index];
    if (seen->epoch == parsed.epoch && (int32_t)(parsed.revision - seen->revision) <= 0)
        return;
    *seen = parsed;
    observed = parsed;
    memcpy(lastSnapshot, data, DUEL_WIRE_SIZE);
}

int sithComm_SendMsgToPlayer(SithMessage *msg, int to, int outstream, int flags)
{
    (void)outstream; (void)flags;
    if (stdComm_bIsServer) observe((const unsigned char *)msg->pktData, msg->netMsg.msg_size);
    envelope(1, (uint32_t)to, msg->pktData, msg->netMsg.msg_size);
    return 1;
}

void sithConsole_PrintString(const char *text)
{
    envelope(4, 0, text, (uint32_t)strlen(text));
}
void sithMulti_SendChatTo(const char *text, int a, int b, int to)
{
    (void)a; (void)b;
    envelope(5, (uint32_t)to, text, (uint32_t)strlen(text));
}

uint32_t stdPlatform_GetTimeMsec(void) { return clockMs; }
static char netLog[4096];
int stdPlatform_Printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t used = strlen(netLog);
    int n = vsnprintf(netLog + used, sizeof(netLog) - used, fmt, ap);
    va_end(ap);
    return n;
}
int stdComm_WSRelay_InMatch(void) { return matchOn; }
int stdString_WcharToChar(char *out, const char16_t *in, int n)
{
    int i = 0;
    for (; i < n && in[i]; ++i) out[i] = (char)in[i];
    out[i] = 0;
    return i;
}
flex_t rdVector_Dist3(const rdVector3 *a, const rdVector3 *b)
{
    return sqrtf((a->x-b->x)*(a->x-b->x)+(a->y-b->y)*(a->y-b->y)+(a->z-b->z)*(a->z-b->z));
}
rdVector3 *rdVector_Add3Acc(rdVector3 *a, const rdVector3 *b)
{
    a->x += b->x; a->y += b->y; a->z += b->z; return a;
}
void rdMatrix_PreRotate34(rdMatrix34 *mat, const rdVector3 *angles) { (void)mat; (void)angles; }
int sithCollision_HasLOS(SithThing *a, SithThing *b, int flags) { (void)a; (void)b; (void)flags; return sight; }
SithSector *sithCollision_FindSectorInRadius(SithSector *s, const rdVector3 *a, rdVector3 *b, flex_t r)
{ (void)a; (void)b; (void)r; return s; }
flex_t sithCollision_SearchForCollisions(SithSector *s, SithThing *t, const rdVector3 *p, const rdVector3 *v, flex_t d, flex_t r, int flags)
{
    (void)s; (void)t; (void)p; (void)v; (void)d; (void)r; (void)flags;
    /* Ordered contacts are fixture inputs, not computed 3D geometry. The
     * production aimedPlayer loop decides whether to stop at the wall or
     * resolve the actor behind it. Its target-selection logic is unmodified.
     */
    ++aimSearches; aimCount=aimNext=0;
    if (aimWorld)
        aimContacts[aimCount++]=(SithCollision){.type=SITHCOLLISION_WORLD,.distance=0.1f};
    if (aimTarget)
        aimContacts[aimCount++]=(SithCollision){.type=SITHCOLLISION_THING,.pThingCollided=&people[aimTarget-1],.distance=0.4f};
    return 0;
}
SithCollision *sithCollision_PopStack(void)
{
    return aimNext<aimCount ? &aimContacts[aimNext++] : NULL;
}
void sithCollision_DecreaseStackLevel(void) { ++aimReleases; }
void jkSaber_Disable(SithThing *p) { (void)p; ++disables; }
void sithWeapon_StartupEntry(void) { ++weaponResets; }
void sithPhysics_ResetThingMovement(SithThing *p) { (void)p; ++movementResets; }
int sithWeapon_ProcessWeaponControls(SithThing *p, flex_t dt) { (void)p; (void)dt; return 0; }
SithThing *sithThing_GetThingParent(SithThing *p)
{
    for (int i = 0; p && p->pParent && p->pParent->signature == p->parentSignature && i < 16; ++i)
        p = p->pParent;
    return p;
}
flex_t sithInventory_SetInventory(SithThing *p, int bin, flex_t amount)
{
    ++inventoryWrites;
    return p->actorParams.pPlayer->aItems[bin].amount = amount;
}

static void append(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(report + used, sizeof(report)-used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(report)-used) exit(6);
    used += (size_t)n;
}

static void array(const char *name, int which)
{
    append(",\"%s\":[", name);
    for (int i=0; i<3; ++i) for (int j=0; j<3; ++j) {
        int value = which == 0 ? sithDuelCombat_AllowsDamage(&people[i], &people[j], 1)
                  : which == 1 ? sithDuelCombat_AllowsDamage(&people[i], &people[j], 0)
                  : which == 2 ? sithDuelCombat_AllowsCollision(&people[i], &people[j])
                               : sithDuelCombat_AllowsSkill(&people[i], &people[j]);
        append("%s%d", (i || j) ? "," : "", value);
    }
    append("]");
}

static void receipt(void)
{
    used = 0;
    append("{\"id\":%d,\"pid\":%d,\"clock\":%u,\"phase\":[%d,%d,%d]", stdComm_dplayIdSelf, getpid(), clockMs,
           sithDuel_PhaseForPlayer(0), sithDuel_PhaseForPlayer(1), sithDuel_PhaseForPlayer(2));
    append(",\"health\":[%.3f,%.3f,%.3f],\"shields\":[%.3f,%.3f,%.3f],\"flags\":[%u,%u,%u]",
        (double)people[0].actorParams.health,(double)people[1].actorParams.health,(double)people[2].actorParams.health,
        (double)jkPlayer_playerInfos[0].aItems[SITHBIN_SHIELDS].amount,
        (double)jkPlayer_playerInfos[1].aItems[SITHBIN_SHIELDS].amount,
        (double)jkPlayer_playerInfos[2].aItems[SITHBIN_SHIELDS].amount,
        people[0].flags,people[1].flags,people[2].flags);
    append(",\"callbacks\":{\"saber_disable\":%d,\"weapon_reset\":%d,\"movement_reset\":%d,\"inventory_writes\":%d}",
        disables,weaponResets,movementResets,inventoryWrites);
    append(",\"aim_fixture\":{\"target\":%d,\"world_obstruction\":%d,\"searches\":%d,\"releases\":%d,\"popped\":%d}",
        aimTarget,aimWorld,aimSearches,aimReleases,aimNext);
    append(",\"last\":{\"epoch\":%u,\"revision\":%u,\"generation\":%u,\"phase\":%d,\"reason\":%d,\"a\":%u,\"b\":%u,\"winner\":%u},\"wire\":\"",
        observed.epoch,observed.revision,observed.generation,observed.phase,observed.reason,observed.a,observed.b,observed.winner);
    for (int i=0;i<DUEL_WIRE_SIZE;++i) append("%02x",lastSnapshot[i]);
    append("\",\"header\":{\"epoch\":%u,\"revision\":%u,\"allowed\":%d},\"pairs\":[",
        observedHeader.epoch,observedHeader.revision,observedHeader.allowed);
    for (int i=0,n=0;i<DUEL_MAX_PAIRS;++i) if (observedPairs[i].epoch) {
        const DuelState *v=&observedPairs[i];
        append("%s{\"index\":%u,\"phase\":%d,\"reason\":%d,\"a\":%u,\"b\":%u,\"winner\":%u}",
            n++?",":"",v->index,v->phase,v->reason,v->a,v->b,v->winner);
    }
    append("]");
    array("damage_saber",0); array("damage_other",1); array("collision",2); array("skill",3);
    append(",\"claims\":[");
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) for(int k=0;k<3;++k)
        append("%s%d",(i||j||k)?",":"",sithDuelCombat_AllowsClaim(&people[i],&people[j],&people[k],1));
    append("]");
    SithThing shot = {.type=SITH_THING_WEAPON,.signature=44,.pParent=&people[2],.parentSignature=people[2].signature};
    SithThing blast = {.type=SITH_THING_EXPLOSION,.pParent=&shot,.parentSignature=shot.signature};
    SithThing loose = {.type=SITH_THING_COG,.moveType=SITH_MT_PHYSICS};
    SithThing door = {.type=SITH_THING_COG,.moveType=SITH_MT_PATH};
    SithThing item = {.type=SITH_THING_ITEM};
    append(",\"objects\":{\"shot_damage\":%d,\"blast_damage\":%d,\"crate_damage\":%d,\"door_damage\":%d,\"self_hazard\":%d,\"shot_contact\":%d,\"shot_reverse_contact\":%d,\"crate_contact\":%d,\"item_contact\":%d,\"item_reverse_contact\":%d,\"door_contact\":%d}",
        sithDuelCombat_AllowsDamage(&people[0],&shot,0),sithDuelCombat_AllowsDamage(&people[0],&blast,0),
        sithDuelCombat_AllowsDamage(&people[0],&loose,0),sithDuelCombat_AllowsDamage(&people[0],&door,0),
        sithDuelCombat_AllowsDamage(&people[0],&people[0],0),sithDuelCombat_AllowsCollision(&people[0],&shot),
        sithDuelCombat_AllowsCollision(&shot,&people[0]),sithDuelCombat_AllowsCollision(&people[0],&loose),
        sithDuelCombat_AllowsCollision(&people[0],&item),sithDuelCombat_AllowsCollision(&item,&people[0]),
        sithDuelCombat_AllowsCollision(&people[0],&door));
    append(",\"permissions\":[");
    for(int i=0;i<3;++i) append("%s[%d,%d,%d,%d,%d]",i?",":"",
        sithDuelCombat_AllowsBin(&people[i],SITHBIN_F_JUMP),
        sithDuelCombat_AllowsBin(&people[i],SITHBIN_F_GRIP),
        sithDuelCombat_AllowsBin(&people[i],SITHBIN_BACTATANK),
        sithDuelCombat_AllowsWeapon(&people[i],SITHBIN_BRYARPISTOL),
        sithDuelCombat_AllowsProjectile(&people[i]));
    append("]}");
    envelope(3,0,report,(uint32_t)used);
}

static void setup(void)
{
    memset(people,0,sizeof(people)); memset(jkPlayer_playerInfos,0,sizeof(jkPlayer_playerInfos));
    memset(sithInventory_g_aTypes,0,sizeof(sithInventory_g_aTypes));
    memset(sithInventory_powerKeybinds,0,sizeof(sithInventory_powerKeybinds));
    memset(&jkEpisode_mLoad,0,sizeof(jkEpisode_mLoad));
    memset(&observed,0,sizeof(observed)); memset(lastSnapshot,0,sizeof(lastSnapshot));
    memset(observedPairs,0,sizeof(observedPairs)); memset(&observedHeader,0,sizeof(observedHeader));
    sithDuel_bAllowDuels=1;
    sithNet_isMulti=1; sithNet_MultiModeFlags=0; matchOn=sight=1; clockMs=1000;
    stdComm_Imperial_bActive=stdComm_WSRelay_bViewer=jkGuiNetHost_bIsDedicated=0;
    sithNet_bNeedsFullThingSyncForLeaveJoin=0; sithMulti_curWelcomePlayerNum=0; sithMulti_newPlayerId=0;
    disables=weaponResets=movementResets=inventoryWrites=0;
    aimTarget=aimWorld=aimCount=aimNext=aimSearches=aimReleases=0;
    sithWeapon_8BD024=-1; sithInventory_bUnkPower=sithInventory_g_bSendDeactivateMessage=0;
    strcpy(sithInventory_g_aTypes[SITHBIN_LIGHTSABER].fpath,"Lightsaber");
    sithInventory_g_aTypes[SITHBIN_LIGHTSABER].flags=SITHINVENTORY_TYPE_REGISTERED|ITEMINFO_WEAPON;
    sithInventory_g_aTypes[SITHBIN_LIGHTSABER].cog=&saberCog;
    strcpy(sithInventory_g_aTypes[SITHBIN_F_JUMP].fpath,"Force_Jump");
    sithInventory_g_aTypes[SITHBIN_F_JUMP].flags=SITHINVENTORY_TYPE_AUTOAIM|SITHINVENTORY_TYPE_REGISTERED;
    sithInventory_g_aTypes[SITHBIN_F_JUMP].cog=&jumpCog;
    sithInventory_g_aTypes[SITHBIN_SHIELDS].flags=SITHINVENTORY_TYPE_REGISTERED;
    sithInventory_g_aTypes[SITHBIN_SHIELDS].max=237;
    for(int i=0;i<3;++i) {
        SithThing *p=&people[i]; SithPlayer *info=&jkPlayer_playerInfos[i];
        p->type=SITH_THING_PLAYER; p->signature=100+i; p->sector=&sector; p->position.x=(float)i*0.4f;
        p->actorParams.pPlayer=info; p->actorParams.health=50; p->actorParams.maxHealth=117;
        info->pLocalPlayer=p; info->flags=1; info->playerNetId=i+1; info->curWeaponID=SITHBIN_LIGHTSABER;
        info->aItems[SITHBIN_LIGHTSABER].amount=1; info->aItems[SITHBIN_LIGHTSABER].state=SITHINVENTORY_ITEM_AVAILABLE;
        info->aItems[SITHBIN_SHIELDS].amount=23;
        const char *name=i==0?"Host":i==1?"Challenger":"Observer";
        for(size_t j=0;j<strlen(name);++j) info->player_name[j]=(char16_t)name[j];
    }
    playerThingIdx=stdComm_dplayIdSelf-1;
    sithPlayer_g_pLocalPlayerThing=&people[playerThingIdx];
    sithDuel_Reset();
}

static void command(char *text)
{
    int a=0,b=0; unsigned t=0; float x=0,y=0;
    if (!strcmp(text,"reset")) setup();
    else if (!strcmp(text,"hello")) {
        sithDuel_Frame();
        for(int i=0;i<3;++i) if(i+1!=stdComm_dplayIdSelf) sithDuel_Hello(i,i+1,1);
    }
    else if (!strncmp(text,"chat ",5)) sithDuel_Chat(text+5);
    else if (!strcmp(text,"button")) sithDuel_Command(NULL, "");
    else if (sscanf(text,"aim %d %d",&a,&b)==2 && a>=0 && a<=3 && b>=0 && b<=1) {
        aimTarget=a; aimWorld=b;
    }
    else if (sscanf(text,"time %u",&t)==1) {clockMs=t;sithDuel_Frame();}
    else if (sscanf(text,"clock %u",&t)==1) clockMs=t;
    else if (!strcmp(text,"tick")) sithDuel_Frame();
    else if (sscanf(text,"health %d %f %f",&a,&x,&y)==3 && a>=1 && a<=3) {
        people[a-1].actorParams.health=x; jkPlayer_playerInfos[a-1].aItems[SITHBIN_SHIELDS].amount=y;
    }
    else if (sscanf(text,"dead %d",&a)==1 && a>=1 && a<=3) {
        people[a-1].flags|=SITH_TF_DEAD; people[a-1].actorParams.health=0; sithDuel_PlayerDied(&people[a-1]);
    }
    else if (sscanf(text,"proxydead %d",&a)==1 && a>=1 && a<=3) {
        people[a-1].flags|=SITH_TF_DEAD; people[a-1].actorParams.health=0;
    }
    else if (sscanf(text,"ownerdeath %d %d",&a,&b)==2 && a>=1 && a<=3) {
        sithDuel_OwnerDeath(&people[a-1],(uint32_t)b);
    }
    else if (sscanf(text,"respawn %d",&a)==1 && a>=1 && a<=3) {
        sithDuel_Respawn(&people[a-1]); people[a-1].flags=0; people[a-1].actorParams.health=117;
    }
    else if (sscanf(text,"leave %d",&a)==1 && a>=1 && a<=3) {
        sithDuel_Forget(a-1,a); jkPlayer_playerInfos[a-1].flags=0;
    }
    else if (sscanf(text,"capable %d %d",&a,&b)==2 && a>=1 && a<=3) sithDuel_Hello(a-1,a,b);
    else if (sscanf(text,"joining %d %d",&a,&b)==2 && a>=1 && a<=3) sithDuel_Joining(a-1,a,b);
    else if (sscanf(text,"welcome %d",&a)==1 && a>=1 && a<=3) sithDuel_Welcome(a-1,a);
    else if (sscanf(text,"pos %d %f",&a,&x)==2 && a>=1 && a<=3) people[a-1].position.x=x;
    else if (sscanf(text,"sight %d",&a)==1) sight=a;
    else if (sscanf(text,"saber %d",&a)==1) jkPlayer_playerInfos[playerThingIdx].curWeaponID=a?SITHBIN_LIGHTSABER:SITHBIN_BRYARPISTOL;
    else if (sscanf(text,"team %d",&a)==1) sithNet_MultiModeFlags=a?MULTIMODEFLAG_TEAMS:0;
    else if (sscanf(text,"allow %d",&a)==1) sithDuel_bAllowDuels=a;
    else if (!strncmp(text,"emit ",5)) {
        char hex[4096];
        if(sscanf(text+5,"%d %4095s",&a,hex)==2) {
            unsigned char bytes[2048]; size_t n=strlen(hex)/2;
            for(size_t i=0;i<n;++i) {unsigned v=0;sscanf(hex+2*i,"%2x",&v);bytes[i]=(unsigned char)v;}
            envelope(1,(uint32_t)a,bytes,(uint32_t)n);
        }
    }
    receipt();
}

int main(int argc,char **argv)
{
    if(argc!=3) return 2;
    stdComm_dplayIdSelf=atoi(argv[1]); stdComm_bIsServer=sithNet_isServer=stdComm_dplayIdSelf==1;
    connected=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in dest={.sin_family=AF_INET,.sin_port=htons((uint16_t)atoi(argv[2]))};
    inet_pton(AF_INET,"127.0.0.1",&dest.sin_addr);
    if(connected<0||connect(connected,(struct sockaddr *)&dest,sizeof(dest))<0){perror("connect");return 3;}
    uint32_t id=(uint32_t)stdComm_dplayIdSelf; allwrite(&id,sizeof(id)); setup(); receipt();
    uint32_t head[3];
    while(allread(head,sizeof(head))) {
        if(head[2]>8191) return 5;
        unsigned char data[8192]; if(!allread(data,head[2])) break; data[head[2]]=0;
        if(head[0]==2) command((char *)data);
        else if(head[0]==1) {
            SithMessage msg={0};
            if(head[2]>sizeof(msg.pktData)) continue;
            msg.netMsg.cogMsgId=SITHDUEL_MSG; msg.netMsg.idx=head[1]; msg.netMsg.msg_size=head[2];
            memcpy(msg.pktData,data,head[2]);
            sithDuel_Process(&msg);
            if(!stdComm_bIsServer && head[1]==DUEL_HOST_ID) observe(data,head[2]);
        }
    }
    close(connected);
    return 0;
}
