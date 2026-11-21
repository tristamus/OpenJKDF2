// Asset-free tests against the same decoder and transitions the browser uses.
#include "Gameplay/sithDuelState.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    DuelState h = {0}, client = {0};
    unsigned char packet[DUEL_WIRE_SIZE];
    DuelRequest r = {0};
    sithDuelState_Start(&h, 77);
    assert(sithDuelState_Offer(&h, 2, 3, 1000));
    assert(!sithDuelState_Offer(&h, 4, 3, 1001));
    assert(!sithDuelState_Accept(&h, 2, 1002));
    assert(!sithDuelState_Accept(&h, 4, 1002));
    assert(!sithDuelState_Cancel(&h, 4));
    assert(!sithDuelState_Accept(&h, 3, 6000));
    assert(sithDuelState_Tick(&h, 6000));
    assert(h.phase == DUEL_IDLE && !h.winner && h.reason == DUEL_EXPIRED);
    assert(sithDuelState_Offer(&h, 2, 3, 7000));
    r = (DuelRequest){DUEL_ACCEPT, h.epoch, h.revision, h.generation, 0};
    sithDuelState_WriteRequest(packet, &r);
    DuelRequest got;
    assert(sithDuelState_ReadRequest(packet, sizeof(packet), &got));
    assert(sithDuelState_Matches(&h, &got));
    assert(!sithDuelState_ReadRequest(packet, 3, &got));
    packet[1] = 99;
    assert(!sithDuelState_ReadRequest(packet, sizeof(packet), &got));
    assert(sithDuelState_Accept(&h, 3, 7001));
    assert(!sithDuelState_Matches(&h, &r));
    assert(!sithDuelState_Tick(&h, 9000));
    assert(sithDuelState_Tick(&h, 9001));
    assert(h.phase == DUEL_FIGHT);
    sithDuelState_WriteSnapshot(packet, &h);
    assert(!sithDuelState_ReadSnapshot(&client, packet, sizeof(packet), 2));
    assert(!sithDuelState_ReadSnapshot(&client, packet, sizeof(packet)-1, 1));
    assert(sithDuelState_ReadSnapshot(&client, packet, sizeof(packet), 1));
    assert(!sithDuelState_ReadSnapshot(&client, packet, sizeof(packet), 1));
    assert(!sithDuelState_End(&h, DUEL_DEFEAT, 4));
    assert(sithDuelState_End(&h, DUEL_DEFEAT, 3));
    sithDuelState_WriteSnapshot(packet, &h);
    assert(sithDuelState_ReadSnapshot(&client, packet, sizeof(packet), 1));
    assert(client.winner == 3);
    assert(!sithDuelState_ReadSnapshot(&client, packet, sizeof(packet), 1));
    assert(sithDuelState_Offer(&h, 2, 4, 10000));
    assert(sithDuelState_Accept(&h, 4, 10001));
    assert(!sithDuelState_End(&h, DUEL_DEFEAT, 2));
    assert(sithDuelState_End(&h, DUEL_LEFT, 0));
    assert(!h.winner);
    assert(!sithDuelState_Offer(&h, 4, 4, 0));
    assert(sithDuelState_Offer(&h, 2, 4, 0xfffffff0u));
    assert(!sithDuelState_Tick(&h, 20));
    assert(sithDuelState_Tick(&h, 5000));
    assert(!sithDuelState_Matches(&h, &r));
    assert(sithDuelState_Offer(&h, 2, 4, 20000));
    assert(sithDuelState_Accept(&h, 4, 20001));
    assert(!sithDuelState_End(&h, DUEL_DRAW, 0)); // countdown death is a cancellation
    assert(sithDuelState_Tick(&h, 22001));
    assert(sithDuelState_End(&h, DUEL_DRAW, 0));
    assert(!h.winner && h.reason == DUEL_DRAW);
    sithDuelState_WriteSnapshot(packet, &h);
    packet[24] = 4; // a draw cannot designate a winner
    assert(!sithDuelState_ReadSnapshot(&client, packet, sizeof(packet), 1));
    sithDuelState_Start(&h, 78);
    sithDuelState_WriteSnapshot(packet, &h);
    assert(!sithDuelState_ReadSnapshot(&client, packet, sizeof(packet), 1));
    puts("Private duel state, wire validation and lifecycle passed.");
}
