// The Level Library for the browser build: a game screen the page draws over
// the game when a host presses Level Library... on the Host screen
// (src/Gui/jkGUINetHost.c). Bundled into openjkdf2.js with --pre-js
// (cmake_modules/plat_wasm.cmake), so it always matches the engine it ships
// with.
//
// It lists the community levels the site keeps a copy of. The site's Worker
// (src/levels.js in the jkdf2mp-bb-server repository) copies Massassi.net's
// level lists, pages, zips and screenshots, slowly, and serves
//
//   /levels/jk1.json, /levels/mots.json  the levels that are ready, per game
//   /levels/d/<id>.json                  one level's description, readmes, files
//   /levels/z/<id>.zip?h=<sha256>        its zip
//   /levels/s/<id>-<n>.jpg               its screenshots, from 1
//
// Use Level gets the zip from the site, checks it and each GOB in it against
// the SHA-256s in the list, saves the GOBs in this browser (maps.js's store,
// where maps from hosts go too, so they're written into episode/ each time
// the game starts) and writes them into episode/ now. Nothing goes to the
// player's game folder. The Host screen waits while the library is open
// (jkGuiNetHost_OpenLibrary), then lists the episodes again with the new one
// picked. Players who join get the level from the host, as any other map.
//
// The engine calls in here: available() when the Host screen opens, to show
// the button only where the site serves the library; open(game) for the
// button; state() until it isn't OPEN; take() for the episode to pick, "" if
// none.

(function() {
    var STATE_OPEN = 0, STATE_CLOSED = 1;
    var BASE = '/levels/';
    var MAX_ZIP_BYTES = 1024 * 1024 * 1024;
    var RECHECK_MS = 60000;
    // Massassi dates every level it imported in bulk on this day.
    var BULK_DATE = '2000-06-02';
    // What the Host screen lists (jkGuiNetHost_EnumEpisodes): deathmatch,
    // type 4, CTF, and MotS's saber-only games. Single-player episodes only
    // with Experimental Co-op under Advanced.
    var HOST_TYPES = { jk1: 2 | 4 | 8, mots: 2 | 4 | 8 | 0x10 };
    var EXT = { jk1: 'gob', mots: 'goo' };
    var KINDS = { mp: 'Multiplayer', ctf: 'Capture the Flag', kfy: 'Kill the Fool', sp: 'Single player' };
    var SORTS = [['name', 'Name'], ['new', 'Newest first'], ['old', 'Oldest first'],
                 ['dl', 'Most downloaded'], ['rating', 'Top rated'], ['size', 'Smallest first']];

    var L = {
        state: STATE_CLOSED, result: '', game: 'jk1',
        avail: {}, checkedAt: {},
        index: {}, details: {}, saved: {},
        sel: null, q: '', filters: { mp: true, ctf: true, sp: false }, sort: 0, tab: 'lib', shot: 1,
        job: null, el: null
    };

    function core() { return Module.jkMaps && Module.jkMaps.core; }

    // ---- Whether the site serves the library, asked as the engine starts
    // and again now and then while it doesn't.

    function ask(game) {
        if (typeof fetch !== 'function' || typeof location === 'undefined' || !/^https?:$/.test(location.protocol))
            return;
        var now = Date.now();
        if (L.checkedAt[game] && now - L.checkedAt[game] < RECHECK_MS)
            return;
        L.checkedAt[game] = now;
        fetch(BASE + game + '.json', { method: 'HEAD' }).then(function(res) {
            L.avail[game] = res.ok && /json/.test(res.headers.get('Content-Type') || '');
        }, function() {});
    }

    function available() {
        var game = isMots() ? 'mots' : 'jk1';
        if (!L.avail[game])
            ask(game);
        return !!L.avail[game] && !!core() && typeof document !== 'undefined';
    }

    function isMots() {
        return (Module['arguments'] || []).indexOf('-motsCompat') >= 0;
    }

    // ---- Small helpers

    function esc(s) {
        return String(s == null ? '' : s).replace(/[&<>"']/g, function(c) {
            return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
        });
    }

    function size(bytes) {
        if (!bytes)
            return '?';
        var kb = bytes / 1024;
        return kb >= 1024 ? (kb / 1024).toFixed(1) + ' MB' : Math.max(1, Math.round(kb)) + ' KB';
    }

    var MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
    var MONTHS_LONG = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September',
                       'October', 'November', 'December'];

    // A date as the list has it: 2001-03-04, 2001-03 or 2001, and how
    // precise it is (rp: d, m or y).
    function dateText(iso, precision) {
        var m = /^(\d{4})(?:-(\d{2}))?(?:-(\d{2}))?/.exec(iso || '');
        if (!m)
            return 'unknown';
        if (!m[2] || precision === 'y')
            return m[1];
        if (!m[3] || precision === 'm')
            return MONTHS_LONG[Number(m[2]) - 1] + ' ' + m[1];
        return MONTHS[Number(m[2]) - 1] + ' ' + Number(m[3]) + ', ' + m[1];
    }

    function sortDate(l) { return l.sd || l.rel || l.add || ''; }

    function year(l) { return (l.rel || l.add || '').substring(0, 4) || '?'; }

    function toHex(buf) {
        var b = new Uint8Array(buf), s = '';
        for (var i = 0; i < b.length; i++)
            s += (b[i] < 16 ? '0' : '') + b[i].toString(16);
        return s;
    }

    async function sha256(bytes) {
        return toHex(await crypto.subtle.digest('SHA-256', bytes));
    }

    function fail(text) {
        return Object.assign(new Error(text), { shown: true });
    }

    // ---- Zips: the central directory, and an entry's bytes. Stored and
    // deflated entries are unpacked here; the old methods by the page's
    // unzip.js worker (unzip() in index.html), where there is one.

    function zipEntries(bytes) {
        var v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        var i = bytes.length - 22;
        var stop = Math.max(0, bytes.length - 22 - 65535);
        while (i >= stop && v.getUint32(i, true) !== 0x06054b50)
            i--;
        if (i < stop)
            throw fail("The download isn't a zip.");
        var count = v.getUint16(i + 10, true), cdSize = v.getUint32(i + 12, true), cdAt = v.getUint32(i + 16, true);
        if (cdAt + cdSize > bytes.length)
            throw fail('The zip is cut short.');
        var list = [];
        for (var p = cdAt, n = 0; n < count; n++) {
            if (p + 46 > bytes.length || v.getUint32(p, true) !== 0x02014b50)
                throw fail("The zip's file list is damaged.");
            var flags = v.getUint16(p + 8, true), nameLen = v.getUint16(p + 28, true);
            var extraLen = v.getUint16(p + 30, true), commentLen = v.getUint16(p + 32, true);
            var raw = bytes.subarray(p + 46, p + 46 + nameLen);
            list.push({
                name: ((flags & 0x800) ? new TextDecoder().decode(raw) : String.fromCharCode.apply(null, raw)).replace(/\\/g, '/'),
                flags: flags, method: v.getUint16(p + 10, true), crc: v.getUint32(p + 16, true),
                csize: v.getUint32(p + 20, true), size: v.getUint32(p + 24, true), at: v.getUint32(p + 42, true)
            });
            p += 46 + nameLen + extraLen + commentLen;
        }
        return list;
    }

    async function inflate(data, size) {
        var ds = new DecompressionStream('deflate-raw');
        var out = new Uint8Array(await new Response(new Blob([data]).stream().pipeThrough(ds)).arrayBuffer());
        if (out.length !== size)
            throw fail('A file in the zip came out the wrong size.');
        return out;
    }

    async function zipData(bytes, z) {
        var v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        if (z.at + 30 > bytes.length || v.getUint32(z.at, true) !== 0x04034b50)
            throw fail("The zip's " + z.name + ' is missing.');
        var start = z.at + 30 + v.getUint16(z.at + 26, true) + v.getUint16(z.at + 28, true);
        if (start + z.csize > bytes.length)
            throw fail('The zip is cut short.');
        var data = bytes.subarray(start, start + z.csize);
        if (z.method === 0)
            return data.slice();
        if (z.method === 8)
            return inflate(data, z.size);
        if (typeof unzip === 'function') {
            var out = await unzip(z, data.slice().buffer);
            if (out.length !== z.size)
                throw fail('A file in the zip came out the wrong size.');
            return out;
        }
        throw fail('This zip is packed in a way this page can\'t open.');
    }

    // ---- The list, from the site

    async function getJson(path, signal) {
        var res = await fetch(BASE + path, { signal: signal });
        if (!res.ok)
            throw Object.assign(new Error('HTTP ' + res.status), { status: res.status });
        return res.json();
    }

    async function loadIndex(game) {
        var have = L.index[game];
        if (have && Date.now() - have.at < 5 * 60000)
            return have.data;
        var data = await getJson(game + '.json');
        if (!data || !Array.isArray(data.levels))
            throw new Error('not a level list');
        L.index[game] = { at: Date.now(), data: data };
        return data;
    }

    // The levels saved in this browser, by each GOB's SHA-256: the name it
    // was saved under.
    async function loadSaved(game) {
        var c = core();
        var metas = [];
        try { metas = await c.store.list(game); } catch (e) {}
        var saved = {};
        metas.sort(function(a, b) { return a.used - b.used; });
        for (var m of metas)
            saved[m.sha256] = m.name;
        L.saved[game] = saved;
    }

    // The GOBs of a level the game can list: an episode.jk in it, and this
    // game's kind of GOB.
    function episodeGobs(l, game) {
        var ext = new RegExp('\\.' + EXT[game] + '$', 'i');
        return (l.g || []).filter(function(g) { return g.ep && g.h && ext.test(g.n); });
    }

    function savedName(l, game) {
        var saved = L.saved[game] || {};
        for (var g of episodeGobs(l, game)) {
            if (saved[g.h])
                return saved[g.h];
        }
        return null;
    }

    function levels() {
        var data = L.index[L.game] && L.index[L.game].data;
        return data ? data.levels : [];
    }

    function byId(id) {
        var all = levels();
        for (var i = 0; i < all.length; i++) {
            if (all[i].id === id)
                return all[i];
        }
        return null;
    }

    function visible() {
        var q = L.q.trim().toLowerCase(), f = L.filters;
        var list = levels().filter(function(l) {
            var kind = l.k === 'kfy' ? 'ctf' : l.k;
            if (!f[kind])
                return false;
            return !q || ((l.t || '') + ' ' + (l.a || '')).toLowerCase().indexOf(q) >= 0;
        });
        var name = function(x, y) { return String(x.t).localeCompare(String(y.t), 'en', { sensitivity: 'base' }); };
        var by = {
            name: name,
            'new': function(x, y) { return sortDate(y).localeCompare(sortDate(x)) || name(x, y); },
            old: function(x, y) { return sortDate(x).localeCompare(sortDate(y)) || name(x, y); },
            dl: function(x, y) { return (y.dl || 0) - (x.dl || 0) || name(x, y); },
            rating: function(x, y) { return (y.r || 0) - (x.r || 0) || (y.dl || 0) - (x.dl || 0); },
            size: function(x, y) { return (x.sz || 0) - (y.sz || 0) || name(x, y); }
        }[SORTS[L.sort][0]];
        return list.sort(by);
    }

    // ---- The screen

    var CSS =
        '#jk-lib{position:fixed;inset:0;width:100vw;height:100vh;max-width:none;max-height:none;margin:0;padding:0;border:0;' +
        'overflow:hidden;color-scheme:dark;display:flex;align-items:center;justify-content:center;' +
        '--g-bg1:#24110a;--g-bg2:#5a2a14;--g-bg3:#7a3c1c;--g-side:rgba(160,80,30,.45);--g-glow:rgba(140,170,255,.32);' +
        '--g-label:#f0302a;--g-text:#ffd84a;--g-sel:#fff;--g-line:#f4efe6;--g-check:#c9a47a;--g-btn1:#ffd36b;--g-btn2:#e98a1c;' +
        '--g-t1:#ffe39a;--g-t2:#f2a23a;--g-t3:#b8561a;--g-well:rgba(0,0,0,.28);' +
        'background:radial-gradient(60% 55% at 50% 72%,var(--g-glow),transparent 62%),' +
        'radial-gradient(40% 40% at 18% 40%,var(--g-side),transparent 70%),radial-gradient(40% 40% at 82% 40%,var(--g-side),transparent 70%),' +
        'linear-gradient(180deg,var(--g-bg1),var(--g-bg2) 40%,var(--g-bg3) 70%,var(--g-bg1));' +
        '--f:"Arial Narrow","Roboto Condensed","Helvetica Neue",Arial,sans-serif;' +
        'font-family:var(--f);font-stretch:condensed}' +
        '#jk-lib:not([open]){display:none}' +
        '#jk-lib::backdrop{background:#000}' +
        '#jk-lib[data-game="mots"]{--g-bg1:#0d0614;--g-bg2:#2a0f38;--g-bg3:#43195a;--g-side:rgba(150,70,190,.40);' +
        '--g-glow:rgba(220,200,255,.30);--g-label:#e43ce4;--g-text:#ea5cea;--g-line:#f6ecf8;--g-check:#c463c4;' +
        '--g-btn1:#ffe0ff;--g-btn2:#d98ad9;--g-t1:#ffe2ff;--g-t2:#e07ae6;--g-t3:#8a2fa6}' +
        '#jk-lib .stage{--u:calc(min(100vw,133.333vh)/640);position:relative;flex:none;width:calc(640*var(--u));' +
        'height:calc(480*var(--u));color:var(--g-text);user-select:none}' +
        '#jk-lib .abs{position:absolute;left:calc(var(--x)*var(--u));top:calc(var(--y)*var(--u));width:calc(var(--w)*var(--u));' +
        'height:calc(var(--h)*var(--u));box-sizing:border-box;margin:0}' +
        '#jk-lib .title{text-align:center;font:800 calc(44*var(--u))/1 var(--f);letter-spacing:.01em;' +
        'background:linear-gradient(180deg,var(--g-t1),var(--g-t2) 55%,var(--g-t3));-webkit-background-clip:text;' +
        'background-clip:text;color:transparent;filter:drop-shadow(0 calc(2*var(--u)) 0 rgba(0,0,0,.6))}' +
        '#jk-lib .lbl{color:var(--g-label);font:700 calc(15*var(--u))/1.15 var(--f);letter-spacing:.04em;text-shadow:0 1px 0 rgba(0,0,0,.8)}' +
        '#jk-lib .val{color:var(--g-text);font:700 calc(15*var(--u))/1.15 var(--f);letter-spacing:.04em;text-shadow:0 1px 0 rgba(0,0,0,.8);' +
        'white-space:nowrap;overflow:hidden;text-overflow:ellipsis}' +
        '#jk-lib .box{border:max(1px,calc(1.5*var(--u))) solid var(--g-line);color:var(--g-text);font:700 calc(14*var(--u))/1 var(--f);' +
        'letter-spacing:.04em;padding:0 calc(5*var(--u));display:flex;align-items:center;text-shadow:0 1px 0 rgba(0,0,0,.8);' +
        'background:rgba(0,0,0,.32);border-radius:0}' +
        '#jk-lib input.box{outline:none;caret-color:var(--g-sel);user-select:text}' +
        '#jk-lib input.box:focus{border-color:var(--g-sel)}' +
        '#jk-lib input.box::placeholder{color:var(--g-text);opacity:.45}' +
        '#jk-lib input.box:focus-visible,#jk-lib button:focus-visible,#jk-lib .rows:focus-visible,#jk-lib .panel:focus-visible' +
        '{outline:calc(2*var(--u)) solid var(--g-sel);outline-offset:calc(2*var(--u))}' +
        '#jk-lib input.box:focus-visible{outline:none}' +
        '#jk-lib .filters{display:flex;gap:calc(9*var(--u))}' +
        '#jk-lib .chk{display:flex;align-items:flex-start;gap:calc(5*var(--u));background:none;border:0;padding:0;text-align:left;' +
        'cursor:pointer;white-space:nowrap;font-size:calc(13*var(--u))}' +
        '#jk-lib .chk i{flex:none;width:calc(13*var(--u));height:calc(13*var(--u));margin-top:calc(1*var(--u));background:var(--g-check);' +
        'box-shadow:inset 0 0 0 calc(1*var(--u)) rgba(0,0,0,.4);position:relative}' +
        '#jk-lib .chk[aria-pressed="true"] i::before,#jk-lib .chk[aria-pressed="true"] i::after{content:"";position:absolute;left:45%;top:-10%;' +
        'width:14%;height:120%;background:#fff;transform:rotate(45deg)}' +
        '#jk-lib .chk[aria-pressed="true"] i::after{transform:rotate(-45deg)}' +
        '#jk-lib .list{border:max(1px,calc(1.5*var(--u))) solid var(--g-line);background:rgba(0,0,0,.32)}' +
        '#jk-lib .arrow{justify-content:center;padding:0;cursor:pointer}' +
        '#jk-lib .gbtn{display:flex;align-items:center;justify-content:center;background:none;border:0;cursor:pointer;padding:0;' +
        'font:800 calc(30*var(--u))/1 var(--f);letter-spacing:.02em;background-image:linear-gradient(180deg,var(--g-btn1),var(--g-btn2));' +
        '-webkit-background-clip:text;background-clip:text;color:transparent;filter:drop-shadow(0 calc(1.5*var(--u)) 0 rgba(0,0,0,.7))}' +
        '#jk-lib .gbtn:hover{filter:drop-shadow(0 calc(1.5*var(--u)) 0 rgba(0,0,0,.7)) brightness(1.25)}' +
        '#jk-lib .gbtn:disabled{opacity:.35;cursor:not-allowed;filter:none}' +
        '#jk-lib .small-btn{display:inline-flex;align-items:center;background:none;border:0;padding:0;cursor:pointer;' +
        'font:700 calc(15*var(--u))/1 var(--f);letter-spacing:.04em;color:var(--g-btn1);text-shadow:0 1px 0 rgba(0,0,0,.85)}' +
        '#jk-lib .small-btn:hover{color:var(--g-sel)}' +
        '#jk-lib .tabs{display:flex;gap:calc(18*var(--u))}' +
        '#jk-lib .tab[aria-selected="true"]{color:var(--g-sel);text-decoration:underline;text-decoration-color:var(--g-label);' +
        'text-decoration-thickness:calc(2*var(--u));text-underline-offset:calc(4*var(--u))}' +
        '#jk-lib .hint{text-align:center;color:var(--g-label);font:700 calc(11.5*var(--u))/1.2 var(--f);letter-spacing:.04em;' +
        'text-shadow:0 1px 0 rgba(0,0,0,.9),0 0 calc(4*var(--u)) rgba(0,0,0,.9)}' +
        '#jk-lib .rows{overflow-y:auto;padding:calc(3*var(--u)) calc(4*var(--u));list-style:none;scrollbar-width:thin;' +
        'scrollbar-color:var(--g-label) transparent}' +
        '#jk-lib .rows button{width:100%;display:grid;grid-template-columns:1fr auto;gap:calc(8*var(--u));align-items:baseline;' +
        'background:none;border:0;padding:0 calc(3*var(--u));cursor:pointer;text-align:left;color:var(--g-text);' +
        'font:700 calc(14*var(--u))/calc(19*var(--u)) var(--f);letter-spacing:.04em;text-shadow:0 1px 0 rgba(0,0,0,.8)}' +
        '#jk-lib .rows .t{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}' +
        '#jk-lib .rows .s{font-size:calc(11.5*var(--u));opacity:.8;font-variant-numeric:tabular-nums;white-space:nowrap}' +
        '#jk-lib .rows button:hover .t{text-decoration:underline;text-decoration-color:var(--g-label)}' +
        '#jk-lib .rows button[aria-selected="true"]{color:var(--g-sel);background:var(--g-well)}' +
        '#jk-lib .rows .mark{color:var(--g-sel);margin-left:calc(4*var(--u))}' +
        '#jk-lib .more{color:var(--g-label);font:700 calc(11.5*var(--u))/1.3 var(--f);letter-spacing:.04em;padding:calc(6*var(--u)) calc(3*var(--u));' +
        'white-space:normal}' +
        '#jk-lib[data-busy] .rows button:not([aria-selected="true"]){opacity:.4;cursor:not-allowed}' +
        '#jk-lib .panel{overflow-y:auto;padding:calc(6*var(--u));display:grid;grid-auto-rows:max-content;gap:calc(6*var(--u));' +
        'align-content:start;scrollbar-width:thin;scrollbar-color:var(--g-label) transparent;user-select:text}' +
        '#jk-lib .panel>*{min-width:0}' +
        '#jk-lib .shot{position:relative;width:100%;aspect-ratio:308/176;border:calc(1*var(--u)) solid var(--g-line);background:#000;overflow:hidden}' +
        '#jk-lib .shot img{width:100%;height:100%;object-fit:contain;display:block}' +
        '#jk-lib .shot .none{position:absolute;inset:0;display:grid;place-items:center;text-align:center;padding:calc(10*var(--u));' +
        'background:var(--g-well);white-space:normal}' +
        '#jk-lib .pager{position:absolute;right:calc(5*var(--u));bottom:calc(4*var(--u));display:flex;gap:calc(8*var(--u))}' +
        '#jk-lib .pager button{background:none;border:0;padding:0 calc(2*var(--u));cursor:pointer;font:800 calc(15*var(--u))/1 var(--f);' +
        'color:var(--g-btn1);text-shadow:0 1px 2px #000,0 0 calc(3*var(--u)) #000}' +
        '#jk-lib .pager button[aria-pressed="true"]{color:var(--g-sel);text-decoration:underline;text-decoration-color:var(--g-label)}' +
        '#jk-lib .p-title{color:var(--g-sel);font:800 calc(19*var(--u))/1.05 var(--f);letter-spacing:.03em;text-shadow:0 1px 0 rgba(0,0,0,.8);' +
        'overflow-wrap:anywhere}' +
        '#jk-lib .facts{display:grid;grid-template-columns:auto 1fr;gap:calc(1*var(--u)) calc(8*var(--u));margin:0}' +
        '#jk-lib .facts dt{color:var(--g-label);font:700 calc(13*var(--u))/1.25 var(--f);letter-spacing:.04em;text-shadow:0 1px 0 rgba(0,0,0,.8)}' +
        '#jk-lib .facts dd{margin:0;color:var(--g-text);font:700 calc(13*var(--u))/1.25 var(--f);letter-spacing:.04em;' +
        'text-shadow:0 1px 0 rgba(0,0,0,.8);overflow-wrap:anywhere}' +
        '#jk-lib .facts small{font-size:calc(10.5*var(--u));opacity:.8}' +
        '#jk-lib .desc{margin:0;color:var(--g-text);font:600 calc(12.5*var(--u))/1.3 var(--f);letter-spacing:.03em;' +
        'text-shadow:0 1px 0 rgba(0,0,0,.8);white-space:pre-line;overflow-wrap:anywhere}' +
        '#jk-lib .note{margin:0;color:var(--g-label);font:700 calc(11.5*var(--u))/1.3 var(--f);letter-spacing:.04em}' +
        '#jk-lib .note a{color:var(--g-btn1)}' +
        '#jk-lib .readme{margin:0;font:calc(8.5*var(--u))/1.4 Consolas,"Courier New",monospace;white-space:pre-wrap;overflow-wrap:anywhere;' +
        'color:var(--g-text);text-shadow:0 1px 0 rgba(0,0,0,.6)}' +
        '#jk-lib .rname{color:var(--g-label);font:700 calc(12*var(--u))/1.3 var(--f);letter-spacing:.04em}' +
        '#jk-lib .tree{margin:0;padding:0;list-style:none;font:700 calc(12*var(--u))/1.45 var(--f);letter-spacing:.03em;color:var(--g-text);' +
        'text-shadow:0 1px 0 rgba(0,0,0,.8)}' +
        '#jk-lib .tree li{display:grid;grid-template-columns:1fr auto;gap:calc(8*var(--u))}' +
        '#jk-lib .tree li>span:first-child{overflow-wrap:anywhere}' +
        '#jk-lib .tree .why{color:var(--g-label);font-size:calc(10.5*var(--u))}' +
        '#jk-lib .tree .sz{text-align:right;font-variant-numeric:tabular-nums}' +
        '#jk-lib .progress{display:none}' +
        '#jk-lib[data-busy] .progress,#jk-lib[data-error] .progress{display:block}' +
        '#jk-lib[data-busy] .foot,#jk-lib[data-error] .foot{display:none}' +
        '#jk-lib .bar{position:absolute;left:calc(170*var(--u));top:calc(406*var(--u));width:calc(300*var(--u));height:calc(9*var(--u));' +
        'border:calc(1*var(--u)) solid var(--g-line);box-sizing:border-box;background:rgba(0,0,0,.4)}' +
        '#jk-lib[data-error] .bar{display:none}' +
        '#jk-lib .bar i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--g-btn2),var(--g-btn1))}' +
        '#jk-lib .ptext{position:absolute;left:0;width:100%;top:calc(417*var(--u));text-align:center;color:var(--g-text);' +
        'font:700 calc(11.5*var(--u))/1 var(--f);letter-spacing:.04em;text-shadow:0 1px 0 rgba(0,0,0,.8)}' +
        '#jk-lib[data-error] .ptext{top:calc(410*var(--u));color:var(--g-sel)}' +
        '#jk-lib .ptext b{color:var(--g-sel)}' +
        '@media (prefers-reduced-motion:no-preference){#jk-lib .bar i{transition:width .2s linear}}';

    function pos(x, y, w, h) {
        return 'style="--x:' + x + ';--y:' + y + ';--w:' + w + ';--h:' + h + '"';
    }

    function make() {
        if (L.el)
            return true;
        if (typeof document === 'undefined' || !document.body)
            return false;
        var style = document.createElement('style');
        style.textContent = CSS;
        document.head.appendChild(style);
        var el = document.createElement('dialog');
        el.id = 'jk-lib';
        el.setAttribute('aria-labelledby', 'jk-lib-title');
        el.innerHTML =
            '<div class="stage">' +
            '<h2 class="abs title" id="jk-lib-title" ' + pos(20, 10, 600, 46) + '>Level Library</h2>' +
            '<p class="abs hint" data-id="sub" ' + pos(20, 56, 600, 14) + '></p>' +
            '<label class="abs lbl" for="jk-lib-q" ' + pos(20, 82, 60, 20) + '>Search:</label>' +
            '<input class="abs box" id="jk-lib-q" data-id="q" type="search" ' + pos(82, 79, 208, 22) +
            ' placeholder="title or author" autocomplete="off" spellcheck="false">' +
            '<div class="abs filters" ' + pos(20, 110, 285, 20) + '>' +
            '<button type="button" class="lbl chk" data-filter="mp"><i></i><span>Multiplayer</span></button>' +
            '<button type="button" class="lbl chk" data-filter="ctf"><i></i><span>CTF</span></button>' +
            '<button type="button" class="lbl chk" data-filter="sp"' +
            ' title="Single-player levels, which the Host screen lists with Experimental Co-op ticked under Advanced"><i></i><span>Co-op</span></button>' +
            '</div>' +
            '<span class="abs lbl" ' + pos(20, 138, 44, 20) + '>Sort:</span>' +
            '<button type="button" class="abs box arrow" data-id="sortPrev" ' + pos(66, 135, 24, 22) + ' aria-label="Previous sort">&lsaquo;</button>' +
            '<button type="button" class="abs box arrow" data-id="sortNext" ' + pos(90, 135, 24, 22) + ' aria-label="Next sort">&rsaquo;</button>' +
            '<span class="abs val" data-id="sortName" aria-live="polite" ' + pos(122, 138, 168, 20) + '></span>' +
            '<ul class="abs list rows" data-id="rows" role="listbox" aria-label="Levels" tabindex="0" ' + pos(20, 164, 270, 238) + '></ul>' +
            '<div class="abs tabs" role="tablist" aria-label="Level details" ' + pos(310, 79, 320, 22) + '>' +
            '<button type="button" class="small-btn tab" role="tab" data-tab="lib">Overview</button>' +
            '<button type="button" class="small-btn tab" role="tab" data-tab="readme">Readme</button>' +
            '<button type="button" class="small-btn tab" role="tab" data-tab="files">Files</button></div>' +
            '<div class="abs list panel" data-id="panel" role="tabpanel" tabindex="0" ' + pos(310, 104, 320, 298) + '></div>' +
            '<p class="abs hint foot" ' + pos(0, 410, 640, 16) + '>One download at a time. Levels are saved in this browser, never in your game folder.</p>' +
            '<div class="progress" aria-live="polite"><div class="bar"><i data-id="fill"></i></div><p class="ptext" data-id="ptext"></p></div>' +
            '<button type="button" class="abs gbtn" data-id="cancel" ' + pos(20, 430, 200, 40) + '>Cancel</button>' +
            '<button type="button" class="abs gbtn" data-id="use" ' + pos(420, 430, 200, 40) + '>Use Level</button>' +
            '</div>';
        document.body.appendChild(el);
        L.el = el;
        wire(el);
        return true;
    }

    function part(id) { return L.el.querySelector('[data-id="' + id + '"]'); }

    function wire(el) {
        // Keys and clicks here are the library's, never the game's: the game's
        // own listeners (SDL's, on the window and the document) and the page's
        // would otherwise take them too, and Enter would press Ok on the Host
        // screen once it's back.
        ['keydown', 'keyup', 'keypress', 'wheel', 'pointerdown', 'mousedown', 'touchstart', 'contextmenu'].forEach(function(type) {
            el.addEventListener(type, function(ev) { ev.stopPropagation(); });
        });
        el.addEventListener('cancel', function(ev) {
            ev.preventDefault();
            cancel();
        });
        el.addEventListener('keydown', onKey);
        // And keys that land outside it while it's open (on the page, when a
        // button that had the focus was turned off): the game's listeners are
        // on the window, after these.
        var guard = function(ev) {
            if (L.state !== STATE_OPEN || !el.open || el.contains(ev.target))
                return;
            ev.stopPropagation();
            if (ev.type === 'keydown')
                onKey(ev);
        };
        ['keydown', 'keyup', 'keypress'].forEach(function(type) {
            window.addEventListener(type, guard, true);
        });
        // A screenshot the site doesn't have after all.
        part('panel').addEventListener('error', function(ev) {
            var img = ev.target;
            if (img && img.tagName === 'IMG' && img.parentNode)
                img.outerHTML = '<div class="none val">No screenshot</div>';
        }, true);
        part('q').addEventListener('input', function(ev) {
            L.q = ev.target.value;
            renderRows(true);
        });
        el.addEventListener('click', function(ev) {
            var b = ev.target.closest('button');
            if (!b || !el.contains(b))
                return;
            if (b.dataset.filter) {
                L.filters[b.dataset.filter] = !L.filters[b.dataset.filter];
                renderRows(true);
            } else if (b.dataset.tab) {
                L.tab = b.dataset.tab;
                renderPanel();
            } else if (b.dataset.level) {
                pick(Number(b.dataset.level));
            } else if (b.dataset.shot) {
                L.shot = Number(b.dataset.shot);
                renderPanel(true);
            } else if (b.dataset.id === 'sortPrev' || b.dataset.id === 'sortNext') {
                L.sort = (L.sort + (b.dataset.id === 'sortNext' ? 1 : SORTS.length - 1)) % SORTS.length;
                renderRows(true);
            } else if (b.dataset.id === 'cancel') {
                cancel();
            } else if (b.dataset.id === 'use') {
                use();
            } else if (b.dataset.id === 'retry') {
                load();
            }
        });
    }

    function onKey(ev) {
        if (ev.key === 'Escape') {
            ev.preventDefault();
            cancel();
            return;
        }
        var inText = ev.target && ev.target.tagName === 'INPUT';
        if (ev.key === 'Enter' && (inText || ev.target === part('rows'))) {
            ev.preventDefault();
            use();
            return;
        }
        if ((ev.key === 'ArrowDown' || ev.key === 'ArrowUp' || ev.key === 'PageDown' || ev.key === 'PageUp') &&
            (inText || ev.target === part('rows') || (ev.target.closest && ev.target.closest('.rows')))) {
            ev.preventDefault();
            var list = visible();
            if (!list.length || L.job)
                return;
            var at = list.findIndex(function(l) { return l.id === L.sel; });
            var step = { ArrowDown: 1, ArrowUp: -1, PageDown: 10, PageUp: -10 }[ev.key];
            at = Math.max(0, Math.min(list.length - 1, (at < 0 ? 0 : at + step)));
            pick(list[at].id, true);
        }
    }

    function show() {
        var el = L.el;
        el.dataset.game = L.game;
        delete el.dataset.busy;
        delete el.dataset.error;
        var mots = L.game === 'mots';
        el.querySelector('[data-filter="ctf"] span').textContent = mots ? 'Kill the Fool' : 'CTF';
        if (!el.open) {
            if (el.showModal)
                el.showModal();
            else
                el.setAttribute('open', '');
        }
        part('q').value = L.q;
        part('q').focus();
    }

    function hide() {
        var el = L.el;
        if (el && el.open) {
            if (el.close)
                el.close();
            else
                el.removeAttribute('open');
        }
        if (Module.canvas && Module.canvas.focus)
            Module.canvas.focus();
    }

    async function load() {
        var game = L.game;
        part('sub').textContent = 'Getting the list...';
        part('rows').innerHTML = '';
        renderPanel();
        try {
            var data = await loadIndex(game);
            await loadSaved(game);
            if (game !== L.game || L.state !== STATE_OPEN)
                return;
            var n = data.levels.length, listed = data.listed || n;
            part('sub').textContent = n.toLocaleString('en-US') + (n === 1 ? ' level' : ' levels') + ' from Massassi.net' +
                (data.left ? ', with ' + data.left.toLocaleString('en-US') + ' more of its ' +
                 listed.toLocaleString('en-US') + ' still being copied' : '');
            if (L.sel == null || !byId(L.sel)) {
                var first = visible()[0];
                L.sel = first ? first.id : null;
            }
            renderRows(true);
            renderPanel();
        } catch (e) {
            if (game !== L.game)
                return;
            part('sub').textContent = '';
            part('rows').innerHTML = '<li class="more">The library can\'t be reached right now.' +
                ' <button type="button" class="small-btn" data-id="retry">Try again</button></li>';
            console.warn('OpenJKDF2: the Level Library list did not load: ' + e);
        }
    }

    function renderRows(scroll) {
        if (!L.el)
            return;
        var list = visible(), game = L.game;
        if (list.length && !list.some(function(l) { return l.id === L.sel; }) && !L.job) {
            L.sel = list[0].id;
            renderPanel();
        }
        var html = list.map(function(l) {
            var mark = savedName(l, game) ? '<span class="mark" title="Saved in this browser">&#10003;</span>' : '';
            var kind = l.k === 'ctf' ? 'CTF &middot; ' : l.k === 'kfy' ? 'KFY &middot; ' : l.k === 'sp' ? 'SP &middot; ' : '';
            return '<li><button type="button" role="option" tabindex="-1" data-level="' + l.id + '" aria-selected="' + (l.id === L.sel) + '">' +
                '<span class="t">' + esc(l.t) + mark + '</span>' +
                '<span class="s">' + kind + esc(year(l)) + ' &middot; ' + size(l.sz) + '</span></button></li>';
        }).join('');
        if (!list.length && levels().length)
            html = '<li class="more">No levels match.</li>';
        else if (!levels().length && L.index[game])
            html = '<li class="more">No levels are ready yet. The site is still copying them from Massassi.net.</li>';
        part('rows').innerHTML = html;
        part('sortName').textContent = SORTS[L.sort][1];
        Array.prototype.forEach.call(L.el.querySelectorAll('[data-filter]'), function(b) {
            b.setAttribute('aria-pressed', String(!!L.filters[b.dataset.filter]));
        });
        if (scroll)
            scrollToSel();
        renderUse();
    }

    function scrollToSel() {
        var b = part('rows').querySelector('[aria-selected="true"]');
        if (b && b.scrollIntoView)
            b.scrollIntoView({ block: 'nearest' });
    }

    function pick(id, scroll) {
        if (L.job || id === L.sel)
            return;
        L.sel = id;
        L.shot = 1;
        delete L.el.dataset.error;
        Array.prototype.forEach.call(part('rows').querySelectorAll('[data-level]'), function(b) {
            b.setAttribute('aria-selected', String(Number(b.dataset.level) === id));
        });
        if (scroll)
            scrollToSel();
        renderPanel();
    }

    // One level's description, readmes and files, the first time it's picked.
    function detail(l) {
        var d = L.details[l.id];
        if (d)
            return d.data;
        d = L.details[l.id] = { data: null };
        getJson('d/' + l.id + '.json').then(function(data) {
            d.data = data;
        }, function(e) {
            d.data = { failed: true };
            console.warn('OpenJKDF2: the Level Library has no details for ' + l.id + ': ' + e);
        }).then(function() {
            if (L.sel === l.id && L.state === STATE_OPEN)
                renderPanel(true);
        });
        return null;
    }

    function sourceText(l) {
        var src = l.src === 'r' ? 'from its readme' : l.src === 'f' ? 'the date of the level files in its zip' :
                  l.src === 'm' ? "Massassi's date" : '';
        var note = src;
        if (l.add && l.src !== 'm' && l.add.substring(0, 10) !== (l.rel || ''))
            note += (note ? '; ' : '') + 'Massassi says added ' + dateText(l.add, 'd') +
                    (l.add.substring(0, 10) === BULK_DATE ? ', the day it imported most older levels' : '');
        return note;
    }

    function renderPanel(keepScroll) {
        if (!L.el)
            return;
        Array.prototype.forEach.call(L.el.querySelectorAll('.tab'), function(b) {
            b.setAttribute('aria-selected', String(b.dataset.tab === L.tab));
        });
        var panel = part('panel');
        var l = L.sel != null ? byId(L.sel) : null;
        var top = panel.scrollTop;
        if (!l) {
            panel.innerHTML = '';
            renderUse();
            return;
        }
        var d = detail(l);
        var h = '';
        if (L.tab === 'lib') {
            h += '<div class="shot">';
            if (l.sh > 0) {
                var n = Math.min(L.shot, l.sh);
                h += '<img src="' + BASE + 's/' + l.id + '-' + n + '.jpg" alt="Screenshot ' + n + ' of ' + l.sh + ' of ' + esc(l.t) + '">';
                if (l.sh > 1) {
                    h += '<div class="pager">';
                    for (var i = 1; i <= l.sh; i++)
                        h += '<button type="button" data-shot="' + i + '" aria-pressed="' + (i === n) + '" aria-label="Screenshot ' + i + '">' + i + '</button>';
                    h += '</div>';
                }
            } else {
                h += '<div class="none val">No screenshots for this level</div>';
            }
            h += '</div>';
            h += '<div class="p-title">' + esc(l.t) + '</div>';
            var gobs = episodeGobs(l, L.game);
            var src = sourceText(l);
            h += '<dl class="facts"><dt>Author:</dt><dd>' + esc(l.a || 'unknown') + '</dd>' +
                 '<dt>Released:</dt><dd>' + esc(dateText(l.rel || l.add, l.rel ? l.rp : 'd')) +
                 (src ? ' <small>(' + esc(src) + ')</small>' : '') + '</dd>' +
                 '<dt>Download:</dt><dd>' + size(l.sz) + ' zip</dd>' +
                 '<dt>Type:</dt><dd>' + esc(KINDS[l.k] || l.k) + '</dd>';
            if (gobs.length) {
                var g = gobs[0];
                h += '<dt>Episode:</dt><dd>' + esc(g.ep) + (g.lv && g.lv.length ? ' <small>(' + g.lv.length + ' level' +
                     (g.lv.length === 1 ? '' : 's') + ')</small>' : '') + '</dd>';
            }
            if (l.r != null)
                h += '<dt>Rating:</dt><dd>' + esc(l.r) + ' of 10' + (l.v ? ' <small>(' + l.v + ' votes)</small>' : '') + '</dd>';
            if (l.dl)
                h += '<dt>Downloads:</dt><dd>' + l.dl.toLocaleString('en-US') + ' <small>(on Massassi)</small></dd>';
            h += '</dl>';
            if (!gobs.length)
                h += '<p class="note">This level has no episode GOB for ' + (L.game === 'mots' ? 'Mysteries of the Sith' : 'Jedi Knight') +
                     ', so the game can\'t list it.</p>';
            else if (l.k === 'sp')
                h += '<p class="note">A single-player level: the Host screen lists it with Experimental Co-op ticked under Advanced.</p>';
            if (savedName(l, L.game))
                h += '<p class="note">Saved in this browser as ' + esc(savedName(l, L.game)) + '.</p>';
            if (d && d.desc)
                h += '<p class="desc">' + esc(d.desc) + '</p>';
            else if (!d)
                h += '<p class="note">Getting the description...</p>';
            h += '<p class="note">From <a href="https://www.massassi.net/levels/files/' + l.id + '.shtml" target="_blank" rel="noopener">' +
                 'its page on Massassi.net</a>.</p>';
        } else if (L.tab === 'readme') {
            if (!d)
                h = '<p class="note">Getting the readme...</p>';
            else if (d.failed)
                h = '<p class="note">The readme can\'t be shown right now.</p>';
            else if (!d.texts || !d.texts.length)
                h = '<p class="note">This level\'s zip has no readme.</p>';
            else
                h = d.texts.map(function(t) {
                    return '<div class="rname">' + esc(t.n) + '</div><pre class="readme">' + esc(t.t) + '</pre>';
                }).join('');
        } else {
            if (!d)
                h = '<p class="note">Getting the file list...</p>';
            else if (d.failed || !d.files)
                h = '<p class="note">The file list can\'t be shown right now.</p>';
            else {
                var eps = {};
                for (var e of episodeGobs(l, L.game))
                    eps[((e.in ? e.in + '/' : '') + e.n).toLowerCase()] = e;
                h = '<ul class="tree">' + d.files.map(function(f) {
                    var g = eps[String(f.n).toLowerCase()];
                    return '<li><span>' + esc(f.n) + (g ? ' <span class="why">episode: ' + esc(g.ep) + '</span>' : '') +
                           '</span><span class="sz">' + size(f.b) + '</span></li>';
                }).join('') + '</ul>';
            }
        }
        panel.innerHTML = h;
        panel.scrollTop = keepScroll ? top : 0;
        renderUse();
    }

    function renderUse() {
        var l = L.sel != null ? byId(L.sel) : null;
        part('use').disabled = !!L.job || !l || !episodeGobs(l, L.game).length;
    }

    function progress(fraction, html) {
        part('fill').style.width = Math.round(Math.max(0, Math.min(1, fraction)) * 100) + '%';
        part('ptext').innerHTML = html;
    }

    function setBusy(on) {
        if (on) {
            L.el.dataset.busy = '';
            delete L.el.dataset.error;
        } else {
            delete L.el.dataset.busy;
        }
        Array.prototype.forEach.call(L.el.querySelectorAll('[data-filter],[data-id="sortPrev"],[data-id="sortNext"],[data-id="q"]'),
                                     function(b) { b.disabled = on; });
        renderUse();
        // Keys stay with the library: Cancel, never turned off, takes the
        // focus from what was.
        if (on || !L.el.contains(document.activeElement) || document.activeElement.disabled)
            part(on ? 'cancel' : 'q').focus();
    }

    function showError(text) {
        L.el.dataset.error = '';
        part('ptext').innerHTML = esc(text);
    }

    // ---- Getting a level

    function cancel() {
        if (L.job) {
            L.job.abort.abort();
            return;
        }
        close('');
    }

    async function use() {
        var l = L.sel != null ? byId(L.sel) : null;
        if (!l || L.job || !episodeGobs(l, L.game).length)
            return;
        var job = L.job = { l: l, game: L.game, abort: new AbortController() };
        setBusy(true);
        progress(0, '<b>Getting ' + esc(l.t) + '</b>');
        var name;
        try {
            name = await getLevel(job);
        } catch (e) {
            L.job = null;
            setBusy(false);
            if (job.abort.signal.aborted)
                return;
            console.warn('OpenJKDF2: the Level Library could not get ' + l.id + ': ' + (e && e.stack || e));
            showError(e && e.shown ? e.message : 'The level could not be got: ' + (e && e.message || e));
            return;
        }
        progress(1, '<b>Saved in this browser</b> &middot; adding it to Choose Episode');
        renderRows();
        await new Promise(function(resolve) { setTimeout(resolve, 500); });
        L.job = null;
        setBusy(false);
        close(name);
    }

    // A name for the GOB in episode/: its own, unless something else there
    // has it (an original episode, the player's own level by that name, or
    // another level from the library), then with the level's number after
    // it. write: false when the same GOB is there already under it.
    async function placeName(g, l, game, bytes) {
        var c = core(), fs = Module.FS, root = c.gameRoot(), ext = EXT[game];
        var base = g.n.replace(/^.*\//, '').replace(/\.[^.]*$/, '').replace(/[\/\\:*?"<>|\x00-\x1f]/g, '_')
                      .replace(/^\.+/, '').replace(/\.\.+/g, '.');
        var tag = '_' + l.id;
        var tries = [base.substring(0, 31), base.substring(0, 31 - tag.length) + tag,
                     base.substring(0, 30 - tag.length) + tag + 'b'];
        for (var name of tries) {
            if (!name || !c.validEpisodeName(name) || Module.jkMaps.lib.isRetailEpisode(name))
                continue;
            if (c.findPath(root, 'episode/' + name))
                continue;  // a folder, which the game lists as an episode by that name
            var path = c.findPath(root, 'episode/' + name + '.' + ext);
            if (!path)
                return { name: name, write: true };
            if (c.isFile(path) && fs.stat(path).size === bytes.length && await sha256(c.fileBytes(path)) === g.h)
                return { name: name, write: false };
        }
        throw fail('There is no free name for ' + g.n + ' in the Episode folder.');
    }

    async function getLevel(job) {
        var l = job.l, game = job.game, c = core(), root = c.gameRoot(), ext = EXT[game];
        var gobs = episodeGobs(l, game);
        var got = {};
        var saved = L.saved[game] || {};

        // GOBs saved in this browser before: from there, without the zip.
        for (var g of gobs) {
            if (!saved[g.h])
                continue;
            var have = null;
            try { have = await c.store.find(game, g.h); } catch (e) {}
            alive(job);
            if (!have)
                continue;
            var bytes = new Uint8Array(have.data);
            if (await sha256(bytes) !== g.h)
                continue;
            got[g.h] = await place(g, l, game, bytes, job);
        }

        var left = gobs.filter(function(g) { return !got[g.h]; });
        if (left.length) {
            var zip = await download(job);
            alive(job);
            progress(1, '<b>Opening the zip</b> &middot; ' + esc(l.z || l.id + '.zip'));
            var outer = zipEntries(zip);
            var inner = {};
            for (var g of left) {
                var list = outer, from = zip;
                if (g.in) {
                    if (!inner[g.in]) {
                        var ze = outer.find(function(z) { return z.name.toLowerCase() === g.in.toLowerCase(); });
                        if (!ze)
                            throw fail("The zip doesn't have " + g.in + ' in it.');
                        var ib = await zipData(zip, ze);
                        inner[g.in] = { bytes: ib, list: zipEntries(ib) };
                    }
                    list = inner[g.in].list;
                    from = inner[g.in].bytes;
                }
                var want = g.n.toLowerCase();
                var e = list.find(function(z) { return z.name.toLowerCase() === want && z.size === g.b; }) ||
                        list.find(function(z) { return /\.(gob|goo)$/i.test(z.name) && z.size === g.b; });
                if (!e)
                    throw fail("The zip doesn't have " + g.n + ' in it.');
                progress(1, '<b>Checking ' + esc(g.n) + '</b> &middot; episode &ldquo;' + esc(g.ep) + '&rdquo;');
                var data = await zipData(from, e);
                alive(job);
                if (await sha256(data) !== g.h)
                    throw fail(g.n + " isn't the one the library lists. Try again later.");
                got[g.h] = await place(g, l, game, data, job, true);
            }
        }

        // The one to pick on the Host screen: the first the Host screen lists.
        var types = HOST_TYPES[game];
        var first = gobs.find(function(g) { return (g.ty & types) && got[g.h]; }) || gobs.find(function(g) { return got[g.h]; });
        return got[first.h];
    }

    // Saves a GOB (when it's new here) and writes it into episode/.
    async function place(g, l, game, bytes, job, fresh) {
        var c = core(), root = c.gameRoot(), ext = EXT[game];
        var spot = await placeName(g, l, game, bytes);
        alive(job);
        if (fresh || (L.saved[game] || {})[g.h] !== spot.name) {
            try {
                await c.store.put(game, spot.name, ext, g.h, bytes, { lib: l.id });
            } catch (e) {
                console.warn('OpenJKDF2: could not save ' + g.n + ' in this browser: ' + e);
            }
        }
        alive(job);
        if (spot.write)
            c.install(root, spot.name, ext, bytes);
        (L.saved[game] = L.saved[game] || {})[g.h] = spot.name;
        console.log('OpenJKDF2: the Level Library installed ' + l.id + ' (' + l.t + ') as episode/' + spot.name + '.' + ext);
        return spot.name;
    }

    function alive(job) {
        if (job.abort.signal.aborted)
            throw new Error('cancelled');
    }

    async function download(job) {
        var l = job.l;
        var res = await fetch(BASE + 'z/' + l.id + '.zip?h=' + encodeURIComponent(l.h), { signal: job.abort.signal });
        if (!res.ok)
            throw fail(res.status === 404 ? 'The site no longer has this level. Try again later.' :
                       'The site answered ' + res.status + '. Try again in a moment.');
        var total = Number(res.headers.get('Content-Length')) || l.sz || 0;
        if (total > MAX_ZIP_BYTES)
            throw fail('This level is too big for the browser.');
        var reader = res.body.getReader(), parts = [], got = 0;
        for (;;) {
            var r = await reader.read();
            if (r.done)
                break;
            parts.push(r.value);
            got += r.value.length;
            if (got > MAX_ZIP_BYTES)
                throw fail('This level is too big for the browser.');
            progress(total ? got / total : 0, '<b>Getting ' + esc(l.t) + '</b> &middot; ' + size(got) + ' of ' + size(total));
        }
        var bytes = new Uint8Array(got), at = 0;
        for (var p of parts) {
            bytes.set(p, at);
            at += p.length;
        }
        if (await sha256(bytes) !== l.h)
            throw fail("The download didn't come through whole. Try again.");
        return bytes;
    }

    // ---- The engine's side

    function open(game) {
        if (L.state === STATE_OPEN)
            return true;
        if (!core() || !make())
            return false;
        L.game = game === 'mots' ? 'mots' : 'jk1';
        L.state = STATE_OPEN;
        L.result = '';
        show();
        load();
        return true;
    }

    function close(name) {
        L.result = name || '';
        L.state = STATE_CLOSED;
        hide();
    }

    Module.jkLibrary = {
        available: available,
        open: open,
        state: function() { return L.state; },
        take: function() {
            var name = L.result;
            L.result = '';
            return name;
        },
        // For tests.
        _L: L, _zipEntries: zipEntries, _dateText: dateText
    };

    // Asks the site whether it serves the library as the engine starts, so
    // the Host screen knows by the time it opens.
    Module.preRun = Module.preRun || [];
    if (typeof Module.preRun === 'function')
        Module.preRun = [Module.preRun];
    Module.preRun.push(function() {
        ask(isMots() ? 'mots' : 'jk1');
    });
})();
