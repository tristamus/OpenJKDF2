// How fast a mod goes from a host to a joiner, with the page's own maps.js
// and mods.js on two "tabs" in one headless Chromium page. Not part of CI:
// it needs Playwright's Chromium and takes a while.
//
//   node packaging/wasm/test/transfer-bench.mjs --payload some.gob [options]
//
// The direct way is a real WebRTC data channel, like the game's ('jk-map',
// stdComm_WSRelay.c), whose packets go through a UDP forwarder here that
// holds each one for half the round trip and lets them out no faster than
// the upload rate, as a home connection would. The relay way hands frames
// between the tabs after half the round trip, at most 64 KiB each, as the
// lobby would; it doesn't model the lobby's own speed.
//
// Options:
//   --payload <file>   the mod: one file put in a mod folder (required)
//   --src <dir>        where maps.js and mods.js come from (packaging/wasm)
//   --via direct|relay (direct)
//   --rtt <ms>         round trip (40)
//   --up <Mbit/s>      each way's rate, 0 for no limit (0)
//   --queue <KiB>      packets the forwarder holds before dropping (512)
//   --loss <percent>   packets dropped at random (0)
//   --runs <n>         (1)
//   --lanes <n>        direct links to spread a download over (net.mapLanes,
//                      stdComm_WSRelay.c), all set up before it starts (1);
//                      a maps.js from before them only uses the first
//   --raw <bytes>      only the channel: the host sends messages of that size
//   --pcs <n>          with --raw, over this many links at once (1)

import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createSocket } from 'node:dgram';
import { fileURLToPath } from 'node:url';
import { dirname, join, resolve, basename } from 'node:path';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
function opt(name, def) {
    const i = args.indexOf('--' + name);
    return i >= 0 ? args[i + 1] : def;
}
const payloadPath = opt('payload');
if (!payloadPath) {
    console.error('--payload <file> is needed');
    process.exit(2);
}
const src = resolve(opt('src', join(here, '..')));
const via = opt('via', 'direct');
const rtt = Number(opt('rtt', 40));
const upMbit = Number(opt('up', 0));
const queueBytes = Number(opt('queue', 512)) * 1024;
const loss = Number(opt('loss', 0)) / 100;
const runs = Number(opt('runs', 1));
const raw = Number(opt('raw', 0));
const pcsWanted = Number(opt('pcs', 1));
const lanes = Math.max(1, Number(opt('lanes', 1)));

const files = {
    '/maps.js': readFileSync(join(src, 'maps.js')),
    '/mods.js': readFileSync(join(src, 'mods.js')),
    '/payload': readFileSync(payloadPath),
    '/': Buffer.from('<!doctype html><title>bench</title><body></body>'),
};
const server = createServer((req, res) => {
    const body = files[req.url.split('?')[0]];
    if (!body) {
        res.writeHead(404);
        return res.end();
    }
    const type = req.url === '/' ? 'text/html' : req.url.endsWith('.js') ? 'text/javascript' : 'application/octet-stream';
    res.writeHead(200, { 'content-type': type });
    res.end(body);
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
const base = 'http://127.0.0.1:' + server.address().port;

// One way of the forwarder: holds each packet for half the round trip, and
// with a rate, lets them out one after another as a link of that speed
// would, dropping what doesn't fit in its queue. Every link between the two
// shares each way's rate, as they'd share a home connection.
function limiter() {
    return { busyUntil: 0, queued: 0 };
}
const upLimit = limiter(), downLimit = limiter();

function lane(out, toPort, toHost, lim) {
    const bytesPerMs = upMbit > 0 ? upMbit * 1e6 / 8 / 1000 : 0;
    return msg => {
        if (loss && Math.random() < loss)
            return;
        const now = performance.now();
        let leave = now;
        if (bytesPerMs) {
            if (lim.queued + msg.length > queueBytes)
                return;
            leave = Math.max(now, lim.busyUntil) + msg.length / bytesPerMs;
            lim.busyUntil = leave;
            lim.queued += msg.length;
        }
        setTimeout(() => {
            if (bytesPerMs)
                lim.queued -= msg.length;
            out.send(msg, toPort, toHost);
        }, leave - now + rtt / 2);
    };
}

const sockets = [];
function bind() {
    return new Promise(r => {
        const s = createSocket('udp4');
        sockets.push(s);
        s.bind(0, '127.0.0.1', () => r(s));
    });
}

const browser = await chromium.launch({
    args: ['--disable-features=WebRtcHideLocalIpsWithMdns'],
});
const page = await browser.newPage();
page.on('console', m => {
    const t = m.text();
    if (process.env.BENCH_DEBUG || /OpenJKDF2|bench/.test(t))
        console.log('  [page] ' + t);
});
page.on('pageerror', e => console.log('  [page error] ' + e));

// The forwarder for one pair: A's packets reach B from sa, B's reach A from
// sb, so each sees the other at the address its description gives.
let pair = null;
await page.exposeFunction('benchForA', async (host, port) => {
    pair = { sa: await bind(), sb: await bind(), a: { host, port } };
    return pair.sa.address().port;
});
await page.exposeFunction('benchForB', async (host, port) => {
    pair.b = { host, port };
    // sa hears B (the joiner), sb hears A (the host): A's are its upload.
    pair.sa.on('message', lane(pair.sb, pair.a.port, pair.a.host, downLimit));
    pair.sb.on('message', lane(pair.sa, pair.b.port, pair.b.host, upLimit));
    return pair.sb.address().port;
});

if (process.env.BENCH_DEBUG)
    console.error('bench: page at ' + base);
await page.goto(base + '/');

const result = await page.evaluate(async ({ via, rtt, runs, raw, pcsWanted, lanes, payloadName }) => {
    const [mapsSource, modsSource, payload] = await Promise.all([
        fetch('/maps.js').then(r => r.text()),
        fetch('/mods.js').then(r => r.text()),
        fetch('/payload').then(r => r.arrayBuffer()).then(b => new Uint8Array(b)),
    ]);

    // As helpers.mjs's makeFS: the FS calls maps.js and mods.js make.
    function makeFS(files, cwd) {
        const map = new Map(Object.entries(files));
        const DIR = 0o40000, FILE = 0o100000;
        const extra = new Set();
        const dirs = () => {
            const d = new Set(['/', ...extra]);
            for (const p of map.keys()) {
                const parts = p.split('/');
                for (let i = 2; i < parts.length; i++)
                    d.add(parts.slice(0, i).join('/'));
            }
            return d;
        };
        return {
            cwd: () => cwd,
            readdir(path) {
                path = path.replace(/\/+$/, '') || '/';
                const all = dirs();
                if (!all.has(path))
                    throw new Error('ENOENT ' + path);
                const names = new Set(['.', '..']);
                for (const p of [...map.keys(), ...all]) {
                    if (p.startsWith(path + '/'))
                        names.add(p.slice(path.length + 1).split('/')[0]);
                }
                return [...names];
            },
            stat(path) {
                if (map.has(path))
                    return { mode: FILE, size: map.get(path).length };
                if (dirs().has(path))
                    return { mode: DIR, size: 4096 };
                throw new Error('ENOENT ' + path);
            },
            isFile: mode => mode === FILE,
            isDir: mode => mode === DIR,
            readFile(path) {
                if (!map.has(path))
                    throw new Error('ENOENT ' + path);
                return map.get(path).slice();
            },
            lookupPath(path) {
                if (!map.has(path))
                    throw new Error('ENOENT ' + path);
                const data = map.get(path);
                return { node: { contents: data, usedBytes: data.length } };
            },
            writeFile(path, bytes, opts) { map.set(path, opts && opts.canOwn ? bytes : new Uint8Array(bytes)); },
            unlink(path) { map.delete(path); },
            mkdirTree(path) { extra.add(path.replace(/\/+$/, '')); },
            rename(from, to) { map.set(to, map.get(from)); map.delete(from); },
            open(path) { return { path }; },
            read(stream, buf, offset, length, position) {
                const part = map.get(stream.path).subarray(position, position + length);
                buf.set(part, offset);
                return part.length;
            },
            close() {},
        };
    }

    // No IndexedDB: every run downloads.
    const noIDB = { open() { const req = {}; setTimeout(() => req.onerror && req.onerror()); return req; } };

    async function tab(files, args, folders) {
        const t = { FS: makeFS(files, '/jk1'), arguments: args, jkModFolders: folders, preRun: [], deps: new Set(),
                    addRunDependency(d) { this.deps.add(d); }, removeRunDependency(d) { this.deps.delete(d); } };
        new Function('Module', 'indexedDB', mapsSource)(t, noIDB);
        new Function('Module', 'indexedDB', modsSource)(t, noIDB);
        for (const step of t.preRun)
            step();
        while (t.deps.size)
            await new Promise(r => setTimeout(r, 20));
        return t;
    }

    // Its first IPv4 host candidate: gathering may not finish for a while.
    function hostCandidate(pc) {
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error('no IPv4 host candidate')), 10000);
            const look = () => {
                const m = /a=candidate:\S+ 1 udp \d+ (\d+\.\d+\.\d+\.\d+) (\d+) typ host/.exec(pc.localDescription.sdp);
                if (!m)
                    return false;
                clearTimeout(timer);
                resolve({ host: m[1], port: Number(m[2]) });
                return true;
            };
            if (!look())
                pc.addEventListener('icecandidate', look);
        });
    }

    // The description with one candidate, at the forwarder, in place of
    // this machine's own.
    function retarget(sdp, port) {
        const lines = sdp.split('\r\n').filter(l => !/^a=(candidate|end-of-candidates)/.test(l));
        const out = [];
        for (const l of lines) {
            out.push(l);
            if (/^m=application/.test(l))
                out.push('a=candidate:1 1 udp 2122260223 127.0.0.1 ' + port + ' typ host generation 0');
        }
        return out.join('\r\n');
    }

    async function link() {
        const a = new RTCPeerConnection({ iceServers: [] }), b = new RTCPeerConnection({ iceServers: [] });
        const games = [];
        const chans = [a, b].map(pc => {
            const dc = pc.createDataChannel('jk', { negotiated: true, id: 0 });
            dc.binaryType = 'arraybuffer';
            games.push(dc);
            const mc = pc.createDataChannel('jk-map', { negotiated: true, id: 1 });
            mc.binaryType = 'arraybuffer';
            return mc;
        });
        await a.setLocalDescription(await a.createOffer());
        const ca = await hostCandidate(a);
        const pa = await window.benchForA(ca.host, ca.port);
        await b.setRemoteDescription({ type: 'offer', sdp: retarget(a.localDescription.sdp, pa) });
        await b.setLocalDescription(await b.createAnswer());
        const cb = await hostCandidate(b);
        const pb = await window.benchForB(cb.host, cb.port);
        console.log('bench: linking ' + ca.host + ':' + ca.port + ' as ' + pa + ' and ' + cb.host + ':' + cb.port + ' as ' + pb);
        a.onconnectionstatechange = () => console.log('bench: host link ' + a.connectionState);
        await a.setRemoteDescription({ type: 'answer', sdp: retarget(b.localDescription.sdp, pb) });
        await Promise.all(chans.map(c => c.readyState === 'open' ? null : new Promise(r => c.onopen = r)));
        return { a, b, ma: chans[0], mb: chans[1], ga: games[0], gb: games[1] };
    }

    // --raw: just the channel, the host sending as fast as it takes them.
    if (raw) {
        const links = [];
        for (let i = 0; i < pcsWanted; i++)
            links.push(await link());
        const other = await link();
        const msg = new Uint8Array(raw);
        const total = payload.length;
        let got = 0;
        const t0 = performance.now();
        let finish;
        const done = new Promise(r => finish = r);
        links.forEach((pcs, i) => {
            const share = Math.ceil(total / links.length);
            pcs.mb.onmessage = ev => { got += ev.data.byteLength; if (got >= total) finish(); };
            let sent = 0;
            pcs.ma.bufferedAmountLowThreshold = 1 << 20;
            const fill = () => {
                while (sent < share && pcs.ma.bufferedAmount < (4 << 20)) {
                    pcs.ma.send(msg);
                    sent += msg.length;
                }
            };
            pcs.ma.onbufferedamountlow = fill;
            fill();
        });
        // A game packet each 50 ms, echoed, to see what the transfer does to
        // the game's round trip.
        // On a link of its own, as another player's would be: it only shares
        // the host's connection.
        const pings = [];
        other.gb.onmessage = ev => other.gb.send(ev.data);
        other.ga.onmessage = ev => pings.push(performance.now() - new Float64Array(ev.data)[0]);
        const pinger = setInterval(() => other.ga.send(new Float64Array([performance.now()]).buffer), 50);
        await done;
        clearInterval(pinger);
        pings.sort((x, y) => x - y);
        const q = f => pings.length ? Math.round(pings[Math.min(pings.length - 1, Math.floor(f * pings.length))]) : -1;
        console.log('bench: game round trip during the transfer: median ' + q(0.5) + ' ms, 95% ' + q(0.95) + ' ms, max ' + q(1) + ' ms');
        return { size: total, runs: [{ ms: performance.now() - t0, state: 3, prepMs: 0 }] };
    }

    const folder = 'jkmods/bench';
    const host = await tab({ ['/jk1/' + folder + '/' + payloadName]: payload }, ['-path', folder],
                           [{ name: 'Bench', path: folder }]);
    if (!host.jkMods.current())
        throw new Error('the host has no mod');

    // The map channel, then the transfer links: 'direct', 'direct1', ...
    const links = [];
    if (via === 'direct') {
        for (let i = 0; i < lanes; i++)
            links.push(await link());
    }
    const names = links.map((l, i) => i ? 'direct' + i : 'direct');
    const linkOf = how => links[how === 'direct' ? 0 : Number(how.slice(6))];

    // The lobby: frames after half the round trip, at most 64 KiB.
    function relay(to, from) {
        return bytes => {
            if (bytes.byteLength > 64 * 1024)
                throw new Error('relay frame over 64 KiB: ' + bytes.byteLength);
            const copy = bytes.slice(0);
            setTimeout(() => to.jkMaps.onFrame(from, copy, 'relay'), rtt / 2);
            return 'relay';
        };
    }

    const out = [];
    for (let run = 0; run < runs; run++) {
        const join = await tab({ '/jk1/episode/.keep': new Uint8Array(1) }, [], []);
        const toHost = relay(host, 2), toJoin = relay(join, 1);
        host.jkNet = {
            mapDirect: id => via === 'direct',
            mapSend(id, bytes, how) {
                if (via === 'direct' && how !== 'relay') {
                    linkOf(how).ma.send(bytes);
                    return how;
                }
                return toJoin(bytes);
            },
            rtcEnabled: () => via === 'direct',
        };
        join.jkNet = {
            mapDirect: id => via === 'direct',
            mapLanes: id => names,
            mapBulk() {},
            mapSend(id, bytes, how) {
                if (via === 'direct' && how !== 'relay') {
                    linkOf(how).mb.send(bytes);
                    return how;
                }
                return toHost(bytes);
            },
            rtcEnabled: () => via === 'direct',
        };
        links.forEach((l, i) => {
            l.ma.onmessage = ev => host.jkMaps.onFrame(2, ev.data, names[i]);
            l.mb.onmessage = ev => join.jkMaps.onFrame(1, ev.data, names[i]);
        });
        // Timed from when the host is ready, as it is long before anyone
        // joins in a real game.
        let ready = false;
        const log = console.log;
        console.log = function(line) {
            if (/ready to send mod/.test(line))
                ready = true;
            return log.apply(this, arguments);
        };
        const tp = performance.now();
        host.jkMods.host();
        while (!ready)
            await new Promise(r => setTimeout(r, 10));
        console.log = log;
        const prepMs = performance.now() - tp;
        const tail = host.jkMods.tail();
        const t0 = performance.now();
        join.jkMods.fetch(tail);
        while (join.jkMods.state() === 0)
            await new Promise(r => setTimeout(r, 20));
        const ms = performance.now() - t0;
        const state = join.jkMods.state();
        out.push({ ms, state, prepMs });
        console.log('bench: run ' + (run + 1) + ': state ' + state + ' in ' + Math.round(ms) + ' ms');
        host.jkMods.unhost();
    }
    for (const l of links) {
        l.a.close();
        l.b.close();
    }
    return { size: payload.length, runs: out };
}, { via, rtt, runs, raw, pcsWanted, lanes, payloadName: basename(payloadPath) });

const mb = result.size / 1048576;
for (const r of result.runs) {
    console.log(`${via}${via === 'direct' && !raw ? ' over ' + lanes + (lanes > 1 ? ' links' : ' link') : ''}, ` +
                `${rtt} ms round trip, ${upMbit ? upMbit + ' Mbit/s' : 'no rate limit'}${loss ? ', ' + loss * 100 + '% loss' : ''}: ` +
                `${mb.toFixed(1)} MiB in ${(r.ms / 1000).toFixed(1)} s = ${(mb / (r.ms / 1000)).toFixed(2)} MiB/s` +
                ` (host got ready in ${(r.prepMs / 1000).toFixed(1)} s)` +
                (r.state === 3 ? '' : ' (state ' + r.state + ', not a success)'));
}
await browser.close();
for (const s of sockets)
    s.close();
server.close();
