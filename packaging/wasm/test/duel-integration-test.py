"""Three independent production-module peers, routed over loopback TCP.

The router only forwards opaque game bytes and stamps connection identity.
Commands configure minimal world fixtures or invoke real production entry
points. No Python implementation of the duel state machine or policies exists.
"""
from pathlib import Path
import argparse
import hashlib
import json
import selectors
import socket
import struct
import subprocess
import time
import traceback

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--peer', type=Path, required=True)
parser.add_argument('--repo', type=Path, required=True)
parser.add_argument('--workdir', type=Path, required=True)
parser.add_argument('--receipt', type=Path, help='Optional detailed JSON result including source hashes and packet traces')
args = parser.parse_args()
ROOT = args.workdir
REPO = args.repo
SOURCES = ['src/Gameplay/sithDuel.c', 'src/Gameplay/sithDuelState.c', 'src/Gameplay/sithDuelCombat.c']
records = {'scope': 'Production adapter, state codec, and combat module across three independent native processes and loopback TCP',
           'not_covered': ['WASM browser runtime', 'actual renderer', 'live engine collision and damage callsites',
                           'COG execution', 'DSS_DEATH packet decoding', 'WebRTC or browser WebSocket path',
                           'real-map line-of-sight and aim geometry', 'physical input and binding persistence'],
           'source_sha256': {p: hashlib.sha256((REPO/p).read_bytes()).hexdigest() for p in SOURCES},
           'cases': [], 'snapshots': {}, 'wire': [], 'logs': []}

def check(name, value, detail=None):
    records['cases'].append({'name': name, 'passed': bool(value), 'detail': detail})
    if not value:
        raise AssertionError(name + ': ' + repr(detail))

class Router:
    def __init__(self):
        self.sel = selectors.DefaultSelector()
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.bind(('127.0.0.1', 0)); self.server.listen(3); self.server.settimeout(4)
        self.port = self.server.getsockname()[1]
        self.peers, self.buffers, self.replies, self.processes = {}, {}, {}, {}
        self.logfiles = []
        self.drop = None

    def start(self):
        for ident in (1, 2, 3):
            err = (ROOT/f'peer-{ident}-stderr.txt').open('w')
            self.logfiles.append(err)
            self.processes[ident] = subprocess.Popen([str(args.peer), str(ident), str(self.port)], stdout=err, stderr=err)
            conn, _ = self.server.accept()
            handshake = b''
            while len(handshake) < 4:
                received = conn.recv(4-len(handshake))
                if not received:
                    raise RuntimeError(f'peer {ident} closed during handshake')
                handshake += received
            got, = struct.unpack('<I', handshake)
            assert got == ident
            conn.setblocking(False)
            self.peers[ident], self.buffers[ident], self.replies[ident] = conn, bytearray(), []
            self.sel.register(conn, selectors.EVENT_READ, ident)
        self.settle()

    def send(self, target, kind, actor, data):
        if target in self.peers:
            self.peers[target].sendall(struct.pack('<III', kind, actor, len(data)) + data)

    def pump(self, timeout=0.03):
        events = self.sel.select(timeout)
        for key, _ in events:
            ident = key.data
            data = key.fileobj.recv(65536)
            if not data:
                raise RuntimeError(f'peer {ident} closed; exit={self.processes[ident].poll()}')
            buf = self.buffers[ident]; buf.extend(data)
            while len(buf) >= 12:
                kind, target, size = struct.unpack_from('<III', buf)
                if len(buf) < 12+size:
                    break
                payload = bytes(buf[12:12+size]); del buf[:12+size]
                if kind == 1:
                    dropped = bool(self.drop and self.drop(ident, target, payload))
                    records['wire'].append({'from': ident, 'to': target, 'hex': payload.hex(), 'dropped': dropped})
                    if not dropped:
                        self.send(target, 1, ident, payload)
                elif kind == 3:
                    self.replies[ident].append(json.loads(payload))
                elif kind in (4, 5):
                    records['logs'].append({'peer': ident, 'to': target if kind == 5 else ident, 'text': payload.decode()})
                else:
                    raise RuntimeError(f'unknown envelope kind {kind}')
        return bool(events)

    def settle(self):
        end = time.monotonic()+3
        while self.pump(0.04):
            if time.monotonic() > end:
                raise TimeoutError('transport never settled')

    def ask(self, peer, text):
        before = len(self.replies[peer]); self.send(peer, 2, 0, text.encode())
        until = time.monotonic()+3
        while len(self.replies[peer]) == before:
            self.pump()
            if time.monotonic() > until:
                raise TimeoutError(f'no reply from peer {peer} to {text}')
        self.settle()
        return self.replies[peer][-1]

    def all(self, name=None):
        result = {str(i): self.ask(i, 'report') for i in (1,2,3)}
        if name:
            records['snapshots'][name] = result
        return result

    def reset(self):
        self.drop = None
        for peer in (1,2,3): self.ask(peer, 'reset')
        for peer in (2,3,1): self.ask(peer, 'hello')
        return self.all()

    def offer(self):
        self.ask(2, 'chat /duel 0')
        result = self.all()
        check('host accepts a valid client challenge', result['1']['last']['phase'] == 1, result['1']['last'])
        return result

    def prepare(self):
        self.offer(); self.ask(1, 'chat /duel accept')
        result = self.all()
        check('all peers enter preparation for the same two players', all(p['phase'] == [1,1,0] for p in result.values()), result)
        return result

    def fight(self):
        self.prepare(); self.ask(1, 'time 3000')
        result = self.all()
        check('host starts the fight after mutual readiness and two seconds', all(p['phase'] == [2,2,0] for p in result.values()), result)
        return result

    def death(self, victim):
        # The engine invokes PlayerDied only on the owning actor. Remote
        # replicas receive the owner-confirmed event through OwnerDeath.
        # Actual DSS_DEATH packet parsing is outside this fixture's scope.
        self.ask(victim, f'dead {victim}')
        for peer in (1, 2, 3):
            if peer != victim:
                self.ask(peer, f'ownerdeath {victim} {victim}')
                self.ask(peer, f'dead {victim}')

    def close(self):
        for conn in self.peers.values():
            try: conn.close()
            except OSError: pass
        self.sel.close(); self.server.close()
        for proc in self.processes.values():
            try: proc.wait(timeout=2)
            except subprocess.TimeoutExpired: proc.terminate(); proc.wait(timeout=2)
        for log in self.logfiles:
            log.close()

router = None
try:
    router = Router()
    router.start()
    state = router.reset()
    check('three distinct operating-system processes', len({p['pid'] for p in state.values()}) == 3, {i:p['pid'] for i,p in state.items()})
    check('all peers learn the host initial snapshot', len({p['last']['epoch'] for p in state.values()}) == 1 and state['1']['last']['epoch'] > 0)
    check('ordinary combat and contact allowed before any duel', all(all(p[k]) for p in state.values() for k in ('damage_saber','collision','skill')))

    # Invoke the exact no-argument command used by the native control binding.
    # Contact order and visibility are explicit world fixtures. Real 3D aim,
    # physical device events, and saved bindings are outside this fixture.
    router.ask(2,'aim 0 0'); router.ask(2,'button')
    empty_aim=router.all('aim-no-target')
    check('the button-style command creates no invitation with no aimed player', all(p['last']['phase']==0 for p in empty_aim.values()) and empty_aim['2']['aim_fixture']['searches']==1)
    router.ask(2,'aim 1 1'); router.ask(2,'button')
    blocked_aim=router.all('aim-world-obstruction')
    check('an obstructing world contact prevents challenging the player behind it', all(p['last']['phase']==0 for p in blocked_aim.values()) and blocked_aim['2']['aim_fixture']['popped']==1 and blocked_aim['2']['aim_fixture']['searches']==blocked_aim['2']['aim_fixture']['releases'], blocked_aim['2'])

    router.ask(1,'aim 2 0'); router.ask(1,'button')
    aimed_offer=router.all('aim-host-offer')
    check('the host button-style command challenges its aimed player', all(p['last']['phase']==1 and p['last']['a']==1 and p['last']['b']==2 and p['phase']==[0,0,0] for p in aimed_offer.values()), aimed_offer)
    aimed_revision=aimed_offer['1']['last']['revision']
    router.ask(2,'aim 0 0'); router.ask(2,'button')
    check('an incoming challenge is not accepted by a button press without an aimed target', all(p['last']['phase']==1 and p['last']['revision']==aimed_revision and p['phase']==[0,0,0] for p in router.all().values()))
    router.ask(2,'aim 1 1'); router.ask(2,'button')
    check('an incoming challenge is not accepted through an obstructing world contact', all(p['last']['phase']==1 and p['last']['revision']==aimed_revision for p in router.all().values()))
    router.ask(2,'aim 3 0'); router.ask(2,'button')
    wrong_aim=router.all('aim-wrong-player')
    check('aiming at an unrelated player cannot accept or replace the incoming challenge', all(p['last']['phase']==1 and p['last']['revision']==aimed_revision and p['last']['a']==1 and p['last']['b']==2 for p in wrong_aim.values()), wrong_aim)
    router.ask(3,'aim 1 0'); router.ask(3,'button')
    check('an outsider aiming at the challenger cannot accept another player invitation', all(p['last']['phase']==1 and p['last']['revision']==aimed_revision for p in router.all().values()))
    router.ask(2,'aim 1 0'); router.ask(2,'button')
    aimed_prepare=router.all('aim-reciprocal-acceptance')
    check('aiming back at the challenger and pressing the same command accepts mutually', all(p['phase']==[1,1,0] and p['last']['a']==1 and p['last']['b']==2 for p in aimed_prepare.values()), aimed_prepare)
    check('production aiming releases every supplied collision stack', all(p['aim_fixture']['searches']==p['aim_fixture']['releases'] for p in aimed_prepare.values()))
    router.ask(1,'time 3000')
    check('a mutually aimed challenge proceeds through the normal ready countdown', all(p['phase']==[2,2,0] for p in router.all().values()))

    router.reset(); router.ask(2,'aim 1 0'); router.ask(2,'button')
    reverse_aim=router.all('aim-client-offer')
    check('a client button-style command sends its aimed challenge to the host', all(p['last']['phase']==1 and p['last']['a']==2 and p['last']['b']==1 for p in reverse_aim.values()), reverse_aim)
    router.ask(1,'aim 2 0'); router.ask(1,'button')
    check('the host can accept a client invitation by aiming back with the same command', all(p['phase']==[1,1,0] for p in router.all().values()))

    router.reset()
    offered = router.offer()
    offer_wire = next(w['hex'] for w in reversed(records['wire']) if w['from']==2 and w['to']==1 and bytes.fromhex(w['hex'])[1]==1)
    rev = offered['1']['last']['revision']
    check('an offer alone imposes no combat restrictions', all(p['phase']==[0,0,0] and all(p['damage_saber']) for p in offered.values()))
    for cmd in ('accept', 'cancel', '0'):
        router.ask(3, 'chat /duel '+cmd)
    result = router.all('outsider-rejected')
    check('outsider cannot accept, cancel, or replace another pair offer', result['1']['last']['revision']==rev and result['1']['last']['phase']==1)
    for packet in ('0102', '02'+offer_wire[2:], offer_wire[:-2], offer_wire+'00'):
        router.ask(3, 'emit 1 '+packet)
    check('malformed and truncated network requests do not mutate the duel', router.all()['1']['last']['revision']==rev)

    router.ask(1, 'chat /duel 1')
    result=router.all('prepare')
    check('reciprocal challenge is consent', all(p['phase']==[1,1,0] for p in result.values()))
    check('preparation blocks even partner saber damage and Force jump', all(not p['damage_saber'][1] and not p['damage_saber'][3] and p['permissions'][0]==[0,0,0,0,0] for p in result.values()))
    check('preparation resets only each owner movement/weapon but both saber presentations', all(p['callbacks']['movement_reset']==(1 if i!='3' else 0) and p['callbacks']['weapon_reset']==(1 if i!='3' else 0) and p['callbacks']['saber_disable']==2 for i,p in result.items()), {i:p['callbacks'] for i,p in result.items()})
    router.ask(1,'time 2999')
    check('preparation does not finish one millisecond early', all(p['phase']==[1,1,0] for p in router.all().values()))
    router.ask(1,'time 3000')
    live=router.all('fight')
    check('all peers agree the duel has begun', all(p['phase']==[2,2,0] for p in live.values()))
    check('legal partner saber strikes allowed, other partner damage blocked', all(p['damage_saber'][1] and p['damage_saber'][3] and not p['damage_other'][1] and not p['damage_other'][3] for p in live.values()))
    check('both directions of third-party damage are blocked on every peer', all(not p[k][j] for p in live.values() for k in ('damage_saber','damage_other') for j in (2,5,6,7)))
    check('both directions of outsider body contact and pushing are blocked', all(not p['collision'][j] for p in live.values() for j in (2,5,6,7)))
    check('partners retain body contact', all(p['collision'][1] and p['collision'][3] for p in live.values()))
    check('outside projectiles, splash, loose crates and pickups are excluded', all(not p['objects'][k] for p in live.values() for k in ('shot_damage','blast_damage','crate_damage','shot_contact','shot_reverse_contact','crate_contact','item_contact','item_reverse_contact')))
    check('environmental self damage and moving map doors remain active', all(p['objects'][k] for p in live.values() for k in ('self_hazard','door_damage','door_contact')))
    check('Force target effects cannot cross the pair boundary or affect partners', all(not p['skill'][j] for p in live.values() for j in (0,1,2,3,4,5,6,7)))
    check('Force jump alone remains usable, weapons/items/projectiles restricted', all(p['permissions'][0]==[1,0,0,0,0] and p['permissions'][1]==[1,0,0,0,0] and p['permissions'][2]==[1,1,1,1,1] for p in live.values()))
    check('HWYS claim gate accepts partner identity and rejects forged outsider credit', all(p['claims'][4] and p['claims'][9] and not p['claims'][5] and not p['claims'][18] for p in live.values()))
    check('duel participation does not hide or disable the player entities', all(p['flags']==[0,0,0] for p in live.values()))
    revision=live['1']['last']['revision']
    router.ask(2,'emit 1 '+offer_wire)
    router.ask(3,'emit 2 '+live['1']['wire'])
    after=router.all('replay-rejected')
    check('stale request and non-host forged snapshot do not change phase or revision', all(p['phase']==[2,2,0] and p['last']['revision']==revision for p in after.values()))

    for peer in (1,2,3):
        router.ask(peer,'health 2 27 17')
    router.death(1)
    router.ask(1,'tick')
    ended=router.all('winner-client')
    check('completed defeat clears isolation on every peer', all(p['phase']==[0,0,0] and all(p['collision']) for p in ended.values()))
    check('living client winner is restored to exact max HP and shield cap', ended['2']['health'][1]==117 and ended['2']['shields'][1]==237 and ended['2']['callbacks']['inventory_writes']==1, ended['2'])
    check('loser and outsider receive no reward', ended['1']['health'][0]==0 and ended['1']['callbacks']['inventory_writes']==0 and ended['3']['health'][2]==50 and ended['3']['shields'][2]==23 and ended['3']['callbacks']['inventory_writes']==0)
    router.ask(1,'emit 2 '+ended['1']['wire'])
    check('duplicate result cannot grant reward a second time', router.all()['2']['callbacks']['inventory_writes']==1)
    router.ask(2,'reset'); router.ask(1,'welcome 2')
    rejoined=router.all('late-result')
    check('rejoined winner cannot collect a prior-life result', rejoined['2']['health'][1]==50 and rejoined['2']['shields'][1]==23 and rejoined['2']['callbacks']['inventory_writes']==0)

    router.reset(); router.offer(); router.ask(1,'time 6000')
    expired=router.all('expired')
    check('unaccepted offer expires at five seconds without reward', all(p['phase']==[0,0,0] and p['last']['reason']==1 and p['callbacks']['inventory_writes']==0 for p in expired.values()))
    router.ask(1,'chat /duel accept')
    check('an expired offer cannot be accepted', router.all()['1']['last']['phase']==0)

    router.reset(); router.offer(); router.ask(1,'chat /duel decline')
    declined=router.all('declined')
    check('recipient decline clears all peers without reward', all(p['last']['reason']==3 and p['phase']==[0,0,0] and p['callbacks']['inventory_writes']==0 for p in declined.values()))

    router.reset(); router.prepare(); router.ask(2,'saber 0'); router.ask(2,'tick')
    ineligible=router.all('owner-ineligible')
    check('owner equipment invalidation during preparation cancels over the real network', all(p['phase']==[0,0,0] and p['callbacks']['inventory_writes']==0 for p in ineligible.values()), ineligible)

    router.reset(); router.offer()
    router.drop=lambda actor,target,data: actor==2 and target==1 and len(data)==28 and data[1]==5
    router.ask(1,'chat /duel accept'); router.ask(1,'time 3000')
    unready=router.all('unready')
    check('a missing owner-ready acknowledgement never starts the fight', all(p['phase']==[0,0,0] and p['last']['winner']==0 and p['callbacks']['inventory_writes']==0 for p in unready.values()))
    router.drop=None

    router.reset(); router.prepare()
    router.drop=lambda actor,target,data: actor==2 and target==1 and len(data)==28 and data[1]==4
    router.ask(2,'chat /duel cancel')
    cancel_wire=next(w['hex'] for w in reversed(records['wire']) if w['from']==2 and w['to']==1 and w['dropped'])
    router.ask(1,'time 3000')
    pending=router.all('cancel-crossed-countdown')
    check('a cancelling owner cannot fight while its request is in transit', pending['1']['last']['phase']==3 and pending['2']['phase'][1]==1 and not pending['2']['damage_saber'][3], pending)
    router.drop=None; router.ask(2,'emit 1 '+cancel_wire)
    cancelled=router.all('late-cancel')
    check('same-generation cancellation remains valid after countdown revision changes', all(p['phase']==[0,0,0] and p['last']['reason']==2 and p['callbacks']['inventory_writes']==0 for p in cancelled.values()), cancelled)
    router.offer(); router.ask(1,'chat /duel accept'); router.ask(1,'time 5000')
    fresh=router.all()
    router.ask(2,'emit 1 '+cancel_wire)
    replayed=router.all('old-generation-cancel')
    check('a previous-generation cancellation cannot cancel a new duel', all(p['phase']==[2,2,0] and p['last']['generation']==fresh['1']['last']['generation'] and p['last']['revision']==fresh['1']['last']['revision'] for p in replayed.values()))

    router.reset(); router.prepare()
    router.drop=lambda actor,target,data: actor==2 and target==1 and len(data)==28 and data[1]==4
    router.ask(2,'chat /duel cancel')
    cancel_wire=next(w['hex'] for w in reversed(records['wire']) if w['from']==2 and w['to']==1 and w['dropped'])
    router.ask(1,'clock 3000')
    router.drop=None; router.ask(2,'emit 1 '+cancel_wire)
    at_deadline=router.all('cancel-at-deadline')
    check('cancellation received exactly at the preparation deadline wins the frame race', all(p['phase']==[0,0,0] and p['last']['reason']==2 and p['callbacks']['inventory_writes']==0 for p in at_deadline.values()))

    router.reset(); router.offer()
    router.drop=lambda actor,target,data: actor==2 and target==1 and len(data)==28 and data[1]==4
    router.ask(2,'chat /duel cancel')
    cancel_wire=next(w['hex'] for w in reversed(records['wire']) if w['from']==2 and w['to']==1 and w['dropped'])
    router.ask(1,'chat /duel accept'); router.ask(1,'time 3000')
    router.ask(2,'time 1250')  # Exercise a retry after the host advanced phases.
    revoked_offer=router.all('offer-cancel-crossed-countdown')
    check('offer-origin revocation stays held in preparation through acceptance and retries', revoked_offer['1']['last']['phase']==3 and revoked_offer['2']['phase'][1]==1 and revoked_offer['2']['permissions'][1]==[0,0,0,0,0], revoked_offer)
    router.drop=None; router.ask(2,'emit 1 '+cancel_wire)
    check('offer-origin revocation releases all peers without reward when acknowledged', all(p['phase']==[0,0,0] and p['callbacks']['inventory_writes']==0 for p in router.all().values()))

    router.reset(); router.fight()
    router.drop=lambda actor,target,data: actor==2 and target==1 and len(data)==28 and data[1]==4
    router.ask(2,'chat /duel cancel')
    cancel_wire=next(w['hex'] for w in reversed(records['wire']) if w['from']==2 and w['to']==1 and w['dropped'])
    router.ask(2,'time 1250')
    fight_cancel=router.all('fight-cancel-pending')
    check('fight-origin cancellation retains fighting phase on every peer until acknowledged', all(p['phase']==[2,2,0] for p in fight_cancel.values()), fight_cancel)
    check('fight-origin cancellation grants no preparation immunity or weapon reset', fight_cancel['2']['damage_saber'][1] and fight_cancel['2']['damage_saber'][3] and fight_cancel['2']['permissions'][1]==[1,0,0,0,0] and fight_cancel['2']['callbacks']['weapon_reset']==1)
    router.drop=None; router.ask(2,'emit 1 '+cancel_wire)
    check('fight-origin cancellation releases all peers without rewarding either player', all(p['phase']==[0,0,0] and p['last']['winner']==0 and p['callbacks']['inventory_writes']==0 for p in router.all().values()))

    router.reset(); router.fight()
    router.drop=lambda actor,target,data: actor==2 and target==1 and len(data)==28 and data[1]==4
    router.ask(2,'chat /duel cancel'); router.death(1); router.ask(1,'tick')
    revoked_win=router.all('pending-cancel-result')
    check('pending cancellation prevents recovery even if a victory result arrives first', revoked_win['2']['last']['winner']==2 and revoked_win['2']['phase']==[0,0,0] and revoked_win['2']['callbacks']['inventory_writes']==0 and revoked_win['2']['health'][1]==50 and revoked_win['2']['shields'][1]==23, revoked_win['2'])
    router.drop=None

    router.reset(); router.fight(); router.ask(1,'pos 2 8'); router.ask(1,'tick')
    separated=router.all('separated')
    check('eight-JKU separation cancels without healing', all(p['phase']==[0,0,0] and p['last']['reason']==5 and p['callbacks']['inventory_writes']==0 for p in separated.values()))

    router.reset(); router.fight()
    router.death(1); router.death(2)
    router.ask(1,'tick')
    draw=router.all('simultaneous-death')
    check('simultaneous deaths yield no winner and no reward', all(p['phase']==[0,0,0] and p['last']['winner']==0 and p['callbacks']['inventory_writes']==0 for p in draw.values()))

    router.reset(); router.fight()
    router.death(1)
    for peer in (1,2,3): router.ask(peer,'respawn 1')
    router.ask(1,'tick')
    rapid=router.all('rapid-respawn')
    check('same-frame loser respawn cannot erase the scored defeat', all(p['phase']==[0,0,0] and p['last']['winner']==2 for p in rapid.values()) and rapid['2']['callbacks']['inventory_writes']==1, rapid)

    router.reset(); router.fight(); router.death(2)
    router.ask(2,'respawn 2')
    replacement=router.all('local-replacement-life')
    check('a replacement life immediately leaves the duel before the host resolves death', replacement['2']['last']['phase']==3 and replacement['2']['phase'][1]==0 and replacement['1']['phase']==[2,2,0], replacement)
    check('a replacement life has normal weapon Force and outsider contact permissions', replacement['2']['permissions'][1]==[1,1,1,1,1] and replacement['2']['collision'][5] and replacement['2']['collision'][7] and replacement['2']['skill'][4])
    router.ask(1,'tick')
    replacement_result=router.all('replacement-life-result')
    check('replacement life receives no old-life reward and the living opponent wins once', replacement_result['1']['callbacks']['inventory_writes']==1 and replacement_result['2']['callbacks']['inventory_writes']==0 and replacement_result['2']['shields'][1]==23 and all(p['last']['winner']==1 and p['phase']==[0,0,0] for p in replacement_result.values()))

    router.reset(); router.fight(); router.ask(1,'proxydead 2'); router.ask(1,'tick')
    proxy=router.all('proxy-dead-bit')
    check('a remote proxy DEAD bit alone cannot award a winner or healing', all(p['last']['phase']==3 and p['last']['winner']==0 and p['callbacks']['inventory_writes']==0 for p in proxy.values()), proxy)
    router.ask(1,'dead 2'); router.ask(1,'tick')
    check('the local death callback cannot score a nonowned proxy', router.all()['1']['last']['phase']==3)
    router.ask(1,'ownerdeath 2 3'); router.ask(1,'tick')
    wrong_owner=router.all('wrong-owner-death')
    check('a nonowner death proof cannot score a remote player', all(p['last']['phase']==3 and p['last']['winner']==0 and p['callbacks']['inventory_writes']==0 for p in wrong_owner.values()))
    router.ask(1,'ownerdeath 2 2'); router.ask(1,'tick')
    proven=router.all('verified-owner-death')
    check('the matching owner death hook resolves the pending defeat exactly once', proven['1']['last']['winner']==1 and proven['1']['callbacks']['inventory_writes']==1 and proven['1']['health'][0]==117 and proven['1']['shields'][0]==237)

    router.reset(); router.fight(); router.death(1); router.ask(1,'proxydead 2')
    router.ask(1,'tick'); router.ask(1,'time 3749')
    ambiguous=router.all('ambiguous-death-before-deadline')
    check('one owner proof plus an apparently dead survivor waits through 749 milliseconds', all(p['last']['phase']==3 and p['last']['winner']==0 and p['callbacks']['inventory_writes']==0 for p in ambiguous.values()), ambiguous)
    router.ask(1,'time 3750')
    timed_out=router.all('ambiguous-death-timeout')
    check('missing second owner proof releases the duel at 750 milliseconds without recovery', all(p['phase']==[0,0,0] and p['last']['winner']==0 and p['callbacks']['inventory_writes']==0 for p in timed_out.values()) and timed_out['2']['health'][1]==50 and timed_out['2']['shields'][1]==23, timed_out)

    router.reset(); router.fight(); router.death(1); router.ask(1,'proxydead 2')
    router.ask(1,'tick'); router.ask(1,'time 3749'); router.death(2); router.ask(1,'tick')
    corroborated=router.all('corroborated-death-within-grace')
    check('a second owner proof within the grace period resolves as a draw without recovery', all(p['phase']==[0,0,0] and p['last']['reason']==9 and p['last']['winner']==0 and p['callbacks']['inventory_writes']==0 for p in corroborated.values()), corroborated)

    router.reset(); router.ask(1,'capable 3 0'); router.ask(2,'chat /duel 0')
    unsupported=router.all('unsupported-before-offer')
    check('an unsupported active peer prevents a duel from starting', all(p['phase']==[0,0,0] and p['last']['phase']==0 for p in unsupported.values()))
    router.reset(); router.fight(); router.ask(1,'joining 3 0')
    incompatible=router.all('unsupported-arrival')
    check('unsupported arrival immediately releases the existing duelists', all(incompatible[i]['phase']==[0,0,0] and incompatible[i]['callbacks']['inventory_writes']==0 for i in ('1','2')))

    router.reset(); router.ask(1,'sight 0'); router.ask(2,'chat /duel 0')
    check('host enforces the line-of-sight callback', router.all()['1']['last']['phase']==0)
    router.ask(1,'sight 1'); router.ask(1,'pos 2 2.1'); router.ask(2,'chat /duel 0')
    check('host rejects a challenge beyond two JKU', router.all()['1']['last']['phase']==0)
    router.reset(); router.ask(1,'team 1'); router.ask(2,'chat /duel 0')
    check('team mode cannot enter private dueling', router.all()['1']['last']['phase']==0)

    router.reset(); router.fight()
    for peer in (1,2,3):
        router.ask(peer,'health 1 19 9')
    router.death(2)
    router.ask(1,'tick')
    hostwin=router.all('winner-host')
    check('host winner follows the direct host reward path', hostwin['1']['health'][0]==117 and hostwin['1']['shields'][0]==237 and hostwin['1']['callbacks']['inventory_writes']==1, hostwin['1'])
    check('host victory is agreed by clients without rewarding either client', all(p['phase']==[0,0,0] and p['last']['winner']==1 for p in hostwin.values()) and all(hostwin[i]['callbacks']['inventory_writes']==0 for i in ('2','3')))

    router.reset(); router.ask(2,'chat /duel 2'); router.ask(3,'chat /duel accept')
    clientprep=router.all('clients-prepare')
    check('two clients can consent while the host is the outsider', all(p['phase']==[0,1,1] for p in clientprep.values()), clientprep)
    check('outsider host movement is not frozen by client preparation', clientprep['1']['callbacks']['movement_reset']==0 and all(clientprep[i]['callbacks']['movement_reset']==1 for i in ('2','3')))
    router.ask(1,'time 3000')
    clientfight=router.all('clients-fight')
    check('both remote readiness acknowledgements reach the host', all(p['phase']==[0,2,2] for p in clientfight.values()), clientfight)
    check('host-as-outsider is also isolated bidirectionally', all(not p[k][j] for p in clientfight.values() for k in ('damage_saber','damage_other','collision','skill') for j in (1,2,3,6)))
    check('client partners can strike each other and make authentic hit claims', all(p['damage_saber'][5] and p['damage_saber'][7] and p['claims'][17] and not p['claims'][15] for p in clientfight.values()))
    router.death(1)
    router.ask(1,'tick')
    check('outsider host player death does not cancel the clients duel', all(p['phase']==[0,2,2] for p in router.all().values()))
    for peer in (1,2,3):
        router.ask(peer,'health 2 33 11')
    router.death(3)
    router.ask(1,'tick')
    clientresult=router.all('clients-result')
    check('remote-versus-remote result rewards only the living client winner', clientresult['2']['health'][1]==117 and clientresult['2']['shields'][1]==237 and clientresult['2']['callbacks']['inventory_writes']==1 and all(clientresult[i]['callbacks']['inventory_writes']==0 for i in ('1','3')))

    router.reset(); router.fight()
    for peer in (2,3,1): router.ask(peer,'leave 2')
    left=router.all('disconnected')
    check('participant-left callback clears both peer restrictions without reward', all(p['phase']==[0,0,0] and p['callbacks']['inventory_writes']==0 for p in left.values()))
    check('finished adapter returns normal outsider combat and contact', all(all(p['damage_saber']) and all(p['collision']) and all(p['skill']) for p in left.values()))
    records['result']='PASS'
except Exception as exc:
    records['result']='FAIL'; records['error']=str(exc); records['trace']=traceback.format_exc()
finally:
    if router: router.close()
    records['counts']={'cases':len(records['cases']),'passed':sum(c['passed'] for c in records['cases']),
                       'game_frames':len(records['wire']),'log_messages':len(records['logs'])}
    if args.receipt:
        args.receipt.write_text(json.dumps(records,indent=2)+'\n')
    print(json.dumps({'result':records['result'],'counts':records['counts'],'error':records.get('error'),
                      'receipt':str(args.receipt) if args.receipt else None},indent=2))
    if records['result']!='PASS':
        print(records.get('trace', ''))
        for ident in router.processes if router else ():
            stderr = (ROOT/f'peer-{ident}-stderr.txt').read_text()
            if stderr:
                print(f'Peer {ident}: {stderr}')
        raise SystemExit(1)
