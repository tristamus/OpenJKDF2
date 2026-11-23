// Asset-free tests against the same decoder and transitions the browser uses.
#include "Gameplay/sithDuelState.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void lifecycle(void)
{
    DuelState h = {0}, client = {0};
    unsigned char packet[DUEL_WIRE_SIZE];
    DuelRequest r = {0};
    h.index = 5;
    sithDuelState_Start(&h, 77);
    assert(h.index == 5 && h.epoch == 77 && h.revision == 1);
    assert(sithDuelState_Offer(&h, 2, 3, 1000));
    assert(!sithDuelState_Offer(&h, 4, 3, 1001));
    assert(!sithDuelState_Accept(&h, 2, 1002));
    assert(!sithDuelState_Accept(&h, 4, 1002));
    assert(!sithDuelState_Cancel(&h, 4));
    assert(!sithDuelState_Accept(&h, 3, 6000));
    assert(sithDuelState_Tick(&h, 6000));
    assert(h.phase == DUEL_IDLE && !h.winner && h.reason == DUEL_EXPIRED);
    assert(sithDuelState_Offer(&h, 2, 3, 7000));
    r = (DuelRequest){DUEL_ACCEPT, h.epoch, h.revision, h.generation, 0, 5};
    sithDuelState_WriteRequest(packet, &r);
    DuelRequest got;
    assert(sithDuelState_ReadRequest(packet, sizeof(packet), &got));
    assert(got.index == 5 && sithDuelState_Matches(&h, &got));
    got.index = 6; // the same numbers about another pair are another duel
    assert(!sithDuelState_Matches(&h, &got));
    assert(!sithDuelState_ReadRequest(packet, 3, &got));
    packet[1] = 99;
    assert(!sithDuelState_ReadRequest(packet, sizeof(packet), &got));
    assert(sithDuelState_Accept(&h, 3, 7001));
    assert(!sithDuelState_Matches(&h, &r));
    assert(!sithDuelState_Tick(&h, 9000));
    assert(sithDuelState_Tick(&h, 9001));
    assert(h.phase == DUEL_FIGHT);
    sithDuelState_WriteSnapshot(packet, &h);
    assert(sithDuelState_FrameIndex(packet, sizeof(packet)) == 5);
    assert(!sithDuelState_ReadSnapshot(&client, packet, sizeof(packet), 2));
    assert(!sithDuelState_ReadSnapshot(&client, packet, sizeof(packet)-1, 1));
    assert(sithDuelState_ReadSnapshot(&client, packet, sizeof(packet), 1));
    assert(client.index == 5 && client.a == 2 && client.b == 3 && client.phase == DUEL_FIGHT);
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
    packet[18] = 4; // a draw cannot designate a winner
    assert(!sithDuelState_ReadSnapshot(&client, packet, sizeof(packet), 1));
    sithDuelState_WriteSnapshot(packet, &h);
    packet[20] = 1; // the tail stays zero
    assert(!sithDuelState_ReadSnapshot(&client, packet, sizeof(packet), 1));
    sithDuelState_Start(&h, 78);
    sithDuelState_WriteSnapshot(packet, &h);
    assert(!sithDuelState_ReadSnapshot(&client, packet, sizeof(packet), 1));
}

// A pair's frames never land on another pair's copy.
static void pairs(void)
{
    DuelState h0 = {.index = 0}, h1 = {.index = 1}, c0 = {0}, c1 = {0};
    unsigned char packet[DUEL_WIRE_SIZE];
    sithDuelState_Start(&h0, 90);
    sithDuelState_Start(&h1, 90);
    assert(sithDuelState_Offer(&h0, 2, 3, 100));
    assert(sithDuelState_Offer(&h1, 4, 5, 100));
    sithDuelState_WriteSnapshot(packet, &h0);
    assert(sithDuelState_ReadSnapshot(&c0, packet, sizeof(packet), 1));
    sithDuelState_WriteSnapshot(packet, &h1);
    assert(sithDuelState_FrameIndex(packet, sizeof(packet)) == 1);
    assert(!sithDuelState_ReadSnapshot(&c0, packet, sizeof(packet), 1));
    assert(c0.a == 2 && c0.index == 0);
    assert(sithDuelState_ReadSnapshot(&c1, packet, sizeof(packet), 1));
    assert(c1.a == 4 && c1.index == 1);
    packet[19] = DUEL_MAX_PAIRS; // no such pair
    assert(sithDuelState_FrameIndex(packet, sizeof(packet)) < 0);
    assert(!sithDuelState_ReadSnapshot(&c1, packet, sizeof(packet), 1));

    // A challenge names its target and no pair; every other action names a pair.
    DuelRequest r = {DUEL_CHALLENGE, 90, 0, 0, 3, DUEL_NO_PAIR}, got;
    sithDuelState_WriteRequest(packet, &r);
    assert(packet[2] == DUEL_NO_PAIR);
    assert(sithDuelState_ReadRequest(packet, sizeof(packet), &got));
    assert(got.action == DUEL_CHALLENGE && got.target == 3 && got.index == DUEL_NO_PAIR);
    packet[2] = 4;
    assert(!sithDuelState_ReadRequest(packet, sizeof(packet), &got));
    r = (DuelRequest){DUEL_CANCEL, 90, h1.revision, h1.generation, 0, DUEL_NO_PAIR};
    sithDuelState_WriteRequest(packet, &r);
    assert(!sithDuelState_ReadRequest(packet, sizeof(packet), &got));
    r.index = 1;
    sithDuelState_WriteRequest(packet, &r);
    assert(sithDuelState_ReadRequest(packet, sizeof(packet), &got) && got.index == 1);
    assert(sithDuelState_Matches(&h1, &got) && !sithDuelState_Matches(&h0, &got));
    packet[3] = 1;
    assert(!sithDuelState_ReadRequest(packet, sizeof(packet), &got));
    sithDuelState_WriteRequest(packet, &r);
    packet[0] = 1; // the single-pair version is refused
    assert(!sithDuelState_ReadRequest(packet, sizeof(packet), &got));
}

static void header(void)
{
    DuelHeader h = {90, 1, 0}, client = {0};
    unsigned char packet[DUEL_WIRE_SIZE];
    sithDuelState_WriteHeader(packet, &h);
    assert(sithDuelState_FrameIndex(packet, sizeof(packet)) < 0);
    DuelState pair = {0};
    assert(!sithDuelState_ReadSnapshot(&pair, packet, sizeof(packet), 1));
    DuelRequest request;
    assert(!sithDuelState_ReadRequest(packet, sizeof(packet), &request));
    assert(!sithDuelState_ReadHeader(&client, packet, sizeof(packet), 2));
    assert(!sithDuelState_ReadHeader(&client, packet, sizeof(packet) - 1, 1));
    assert(sithDuelState_ReadHeader(&client, packet, sizeof(packet), 1));
    assert(client.epoch == 90 && client.revision == 1 && !client.allowed);
    assert(sithDuelState_ReadHeader(&client, packet, sizeof(packet), 1)); // the regular repeat
    h.revision = 2;
    h.allowed = 1;
    sithDuelState_WriteHeader(packet, &h);
    assert(packet[2] == 1);
    assert(sithDuelState_ReadHeader(&client, packet, sizeof(packet), 1) && client.allowed);
    h.revision = 1;
    h.allowed = 0;
    sithDuelState_WriteHeader(packet, &h);
    assert(!sithDuelState_ReadHeader(&client, packet, sizeof(packet), 1) && client.allowed);
    h = (DuelHeader){91, 3, 0};
    sithDuelState_WriteHeader(packet, &h);
    assert(!sithDuelState_ReadHeader(&client, packet, sizeof(packet), 1) && client.epoch == 90);
    h = (DuelHeader){90, 3, 1};
    sithDuelState_WriteHeader(packet, &h);
    packet[2] = 2;
    assert(!sithDuelState_ReadHeader(&client, packet, sizeof(packet), 1));
    sithDuelState_WriteHeader(packet, &h);
    packet[12] = 1;
    assert(!sithDuelState_ReadHeader(&client, packet, sizeof(packet), 1));
    sithDuelState_WriteHeader(packet, &h);
    memset(packet + 4, 0, 4); // no session
    assert(!sithDuelState_ReadHeader(&client, packet, sizeof(packet), 1));
}

int main(void)
{
    lifecycle();
    pairs();
    header();
    puts("Private duel state, wire validation and lifecycle passed.");
}
