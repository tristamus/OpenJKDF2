// The Level Library for the browser build: the community levels the site
// keeps a copy of, in a game menu (src/Gui/jkGUILibrary.c) that opens from
// Level Library... on the Host screen (src/Gui/jkGUINetHost.c). Bundled into
// openjkdf2.js with --pre-js (cmake_modules/plat_wasm.cmake), so it always
// matches the engine it ships with.
//
// The site's Worker (src/levels.js in the jkdf2mp-bb-server repository)
// copies Massassi.net's level lists, pages, zips and screenshots, slowly, and
// serves
//
//   /levels/jk1.json, /levels/mots.json  the levels that are ready, per game
//   /levels/d/<id>.json                  one level's description, readmes, files
//   /levels/z/<id>.zip?h=<sha256>        its zip
//   /levels/s/<id>-<n>.jpg               its screenshots, from 1
//
// This file is the menu's other half: it gets the list and the details, keeps
// the search, filters and sort, and gives the menu its text (text(), number(),
// what changed since it last asked). The menu draws everything in the game's
// own fonts and colors; the one thing it can't draw, a level's screenshot in
// full color, the page puts over the frame the menu leaves for it (shot()).
//
// Use Level gets the zip from the site, checks it and each GOB in it against
// the SHA-256s in the list, saves the GOBs in this browser (maps.js's store,
// where maps from hosts go too, so they're written into episode/ each time
// the game starts) and writes them into episode/ now. Nothing goes to the
// player's game folder. The menu closes when it's done, and the Host screen
// lists the episodes again with the new one picked. Players who join get the
// level from the host, as any other map.

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
        state: STATE_CLOSED, result: '', done: false, game: 'jk1',
        avail: {}, checkedAt: {},
        index: {}, details: {}, saved: {},
        rows: [], sel: null, q: '', filters: { mp: true, ctf: true, sp: false }, sort: 0, shot: 1,
        job: null, msg: '', error: '', loadFailed: false, changed: 0
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

    // ---- What the game's menu shows (src/Gui/jkGUILibrary.c asks for it
    // as text, every frame it has changed)

    // What changed since the menu last asked: CH_STATUS the line under the
    // title, CH_ROWS the list, CH_PANEL the level's details, CH_MSG the line
    // over the buttons, CH_USE whether Use Level can be pressed.
    var CH_STATUS = 1, CH_ROWS = 2, CH_PANEL = 4, CH_MSG = 8, CH_USE = 16, CH_ALL = 31;

    function touch(bits) { L.changed |= bits; }

    function statusText() {
        var game = L.game, have = L.index[game];
        if (L.loadFailed)
            return 'The Level Library can\'t be reached right now. Close it and try again in a moment.';
        if (!have)
            return 'Getting the list of levels...';
        var data = have.data, n = data.levels.length;
        return n.toLocaleString('en-US') + (n === 1 ? ' level' : ' levels') + ' from Massassi.net' +
            (data.left ? ' so far, ' + data.left.toLocaleString('en-US') + ' more coming' : '');
    }

    // Letters the game's fonts may not have, as the nearest ones they do
    // (accents dropped; anything else is drawn as the font's stand-in).
    function plain(s) {
        s = String(s == null ? '' : s);
        if (s.normalize)
            s = s.normalize('NFD').replace(/[\u0300-\u036f]/g, '');
        return s
            .replace(/[\u2018\u2019\u201a\u2032]/g, "'").replace(/[\u201c\u201d\u201e\u2033]/g, '"')
            .replace(/[\u2013\u2014\u2212]/g, '-').replace(/\u2026/g, '...').replace(/\u00a0/g, ' ')
            .replace(/\r\n?/g, '\n').replace(/\t/g, '    ').replace(/[\u0000-\u0008\u000b-\u001f\u007f]/g, '');
    }

    // A row: the title, a tab, then the kind, the year and the size. The
    // menu cuts the title to fit and puts the rest in its own column, which
    // has room for "CTF 2000 12 MB".
    function rowText(l) {
        var mark = savedName(l, L.game) ? '* ' : '';
        var kind = l.k === 'ctf' ? 'CTF ' : l.k === 'kfy' ? 'KFY ' : l.k === 'sp' ? 'SP ' : '';
        var mb = (l.sz || 0) / (1024 * 1024);
        return plain(mark + l.t).replace(/\n/g, ' ') + '\t' + kind + year(l) + ' ' +
            (mb >= 10 ? Math.round(mb) + ' MB' : size(l.sz));
    }

    function selected() { return L.sel != null ? byId(L.sel) : null; }

    // Lines for the Overview, Readme and Files tabs, '\n' between them. A
    // line with a tab has a label before it; the menu wraps what's after it.
    function overviewText(l, d) {
        var out = [];
        var fact = function(label, value) { out.push(label + ':\t' + plain(value).replace(/\n/g, ' ')); };
        var src = sourceText(l);
        fact('Author', l.a || 'unknown');
        fact('Released', dateText(l.rel || l.add, l.rel ? l.rp : 'd') + (src ? ' (' + src + ')' : ''));
        fact('Size', size(l.sz) + ' zip');
        fact('Type', KINDS[l.k] || l.k);
        var gobs = episodeGobs(l, L.game);
        if (gobs.length) {
            var g = gobs[0];
            fact('Episode', g.ep + (g.lv && g.lv.length ? ' (' + g.lv.length + ' level' + (g.lv.length === 1 ? '' : 's') + ')' : ''));
        }
        var dls = l.dl ? l.dl.toLocaleString('en-US') + ' downloads on Massassi' : '';
        if (l.r != null)
            fact('Rating', l.r + ' of 10' + (l.v ? ' (' + l.v + ' votes)' : '') + (dls ? ', ' + dls : ''));
        else if (dls)
            fact('Rating', 'none yet, ' + dls);
        var notes = [];
        if (!gobs.length)
            notes.push('This level has no episode GOB for ' + (L.game === 'mots' ? 'Mysteries of the Sith' : 'Jedi Knight') +
                       ', so the game can\'t list it.');
        else if (l.k === 'sp')
            notes.push('A single-player level: the Host screen lists it with Experimental Co-op ticked under Advanced.');
        var saved = savedName(l, L.game);
        if (saved)
            notes.push('Saved in this browser as ' + saved + '.');
        if (notes.length)
            out.push('', plain(notes.join(' ')));
        out.push('');
        if (d && d.desc)
            out.push(plain(d.desc));
        else if (!d)
            out.push('Getting the description...');
        else if (!d.failed)
            out.push('No description.');
        return out.join('\n');
    }

    function readmeText(d) {
        if (!d)
            return 'Getting the readme...';
        if (d.failed)
            return 'The readme can\'t be shown right now.';
        if (!d.texts || !d.texts.length)
            return 'This level\'s zip has no readme.';
        return plain(d.texts.map(function(t) { return '--- ' + t.n + ' ---\n\n' + String(t.t || '').replace(/\s+$/, ''); }).join('\n\n'));
    }

    // A size, a tab, then the file's name.
    function filesText(l, d) {
        if (!d)
            return 'Getting the file list...';
        if (d.failed || !d.files)
            return 'The file list can\'t be shown right now.';
        var eps = {};
        for (var e of episodeGobs(l, L.game))
            eps[((e.in ? e.in + '/' : '') + e.n).toLowerCase()] = e;
        return d.files.map(function(f) {
            var g = eps[String(f.n).toLowerCase()];
            return size(f.b) + '\t' + plain(f.n + (g ? '  (episode: ' + g.ep + ')' : '')).replace(/\n/g, ' ');
        }).join('\n');
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
            if (L.sel === l.id)
                touch(CH_PANEL);
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

    function canUse() {
        var l = selected();
        return !L.job && !!l && episodeGobs(l, L.game).length > 0;
    }

    function setQuery(q, mask, sort) {
        var f = { mp: !!(mask & 1), ctf: !!(mask & 2), sp: !!(mask & 4) };
        sort = Math.max(0, Math.min(SORTS.length - 1, sort | 0));
        q = String(q || '');
        if (q === L.q && sort === L.sort && f.mp === L.filters.mp && f.ctf === L.filters.ctf && f.sp === L.filters.sp)
            return;
        L.q = q;
        L.filters = f;
        L.sort = sort;
        refreshRows();
    }

    function refreshRows() {
        L.rows = visible();
        if (!L.job && L.rows.length && !L.rows.some(function(l) { return l.id === L.sel; }))
            pickId(L.rows[0].id);
        touch(CH_ROWS);
    }

    function pickId(id) {
        if (L.job || id === L.sel)
            return;
        L.sel = id;
        L.shot = 1;
        L.error = '';
        var l = selected();
        if (l)
            detail(l);
        touch(CH_PANEL | CH_USE | CH_MSG);
    }

    async function load() {
        var game = L.game;
        L.loadFailed = false;
        touch(CH_ALL);
        try {
            await loadIndex(game);
            await loadSaved(game);
            if (game !== L.game || L.state !== STATE_OPEN)
                return;
            if (L.sel != null && !byId(L.sel))
                L.sel = null;
            refreshRows();
            if (L.sel == null && L.rows.length)
                pickId(L.rows[0].id);
            else if (selected())
                detail(selected());
        } catch (e) {
            if (game !== L.game)
                return;
            L.loadFailed = true;
            console.warn('OpenJKDF2: the Level Library list did not load: ' + e);
        }
        touch(CH_ALL);
    }

    // The line over the buttons: what's being got, or what went wrong.
    function progress(fraction, text) {
        L.msg = plain(text) + (fraction != null ? ' (' + Math.round(Math.max(0, Math.min(1, fraction)) * 100) + '%)' : '');
        touch(CH_MSG);
    }

    function messageText() {
        if (L.job || L.msg)
            return L.msg;
        if (L.error)
            return L.error;
        return 'Levels you use are saved in this browser, not in your game folder.';
    }

    // ---- The screenshot: a picture the page puts over the menu's frame for
    // it (the menu draws in the game's 256 colors), where the game draws its
    // 640x480 menus: the canvas's full height, 4:3, in the middle
    // (std3D_DrawMenu).

    var shot = { on: false, x: 0, y: 0, w: 0, h: 0, img: null, src: '', polled: 0, raf: 0 };

    function shotImg() {
        if (shot.img || typeof document === 'undefined' || !document.body)
            return shot.img;
        var img = document.createElement('img');
        img.id = 'jk-lib-shot';
        img.alt = '';
        img.style.cssText = 'position:fixed;z-index:1;pointer-events:none;object-fit:contain;background:#000;display:none;';
        img.addEventListener('error', function() { img.style.visibility = 'hidden'; });
        img.addEventListener('load', function() { img.style.visibility = ''; });
        document.body.appendChild(img);
        shot.img = img;
        return img;
    }

    function shotSrc() {
        var l = selected();
        if (!l || !(l.sh > 0) || L.state !== STATE_OPEN)
            return '';
        return BASE + 's/' + l.id + '-' + Math.min(Math.max(1, L.shot), l.sh) + '.jpg';
    }

    function shotLayout() {
        shot.raf = 0;
        var img = shot.img;
        if (!img)
            return;
        var c = Module.canvas;
        var want = shot.on && shotSrc() && c && c.getBoundingClientRect && Date.now() - shot.polled < 500;
        if (!want) {
            img.style.display = 'none';
            return;
        }
        var src = shotSrc();
        if (src !== shot.src) {
            shot.src = src;
            img.style.visibility = 'hidden';
            img.src = src;
        }
        var r = c.getBoundingClientRect();
        var scale = r.height / 480, left = r.left + (r.width - r.height * 4 / 3) / 2;
        img.style.left = (left + shot.x * scale) + 'px';
        img.style.top = (r.top + shot.y * scale) + 'px';
        img.style.width = (shot.w * scale) + 'px';
        img.style.height = (shot.h * scale) + 'px';
        img.style.display = 'block';
        shot.raf = requestAnimationFrame(shotLayout);
    }

    function setShot(on, x, y, w, h) {
        shot.on = !!on;
        shot.x = x; shot.y = y; shot.w = w; shot.h = h;
        shot.polled = Date.now();
        if (!shotImg())
            return;
        if (!shot.raf && typeof requestAnimationFrame === 'function')
            shot.raf = requestAnimationFrame(shotLayout);
    }

    // ---- Getting a level

    function use() {
        var l = selected();
        if (!l || L.job || !episodeGobs(l, L.game).length || L.done)
            return false;
        run(l);
        return true;
    }

    async function run(l) {
        var job = L.job = { l: l, game: L.game, abort: new AbortController() };
        L.error = '';
        progress(0, 'Getting ' + l.t);
        touch(CH_USE);
        var name;
        try {
            name = await getLevel(job);
        } catch (e) {
            L.job = null;
            L.msg = '';
            touch(CH_MSG | CH_USE);
            if (job.abort.signal.aborted)
                return;
            console.warn('OpenJKDF2: the Level Library could not get ' + l.id + ': ' + (e && e.stack || e));
            L.error = plain(e && e.shown ? e.message : 'The level could not be got: ' + (e && e.message || e));
            return;
        }
        progress(null, 'Saved in this browser. Adding it to Choose Episode...');
        touch(CH_ROWS | CH_PANEL);
        await new Promise(function(resolve) { setTimeout(resolve, 400); });
        L.job = null;
        L.result = name;
        L.done = true;
        touch(CH_MSG | CH_USE);
    }

    // Cancel: stops a download if one is going (true), else nothing (false),
    // and the menu closes.
    function cancel() {
        if (!L.job)
            return false;
        L.job.abort.abort();
        return true;
    }

    function openPage() {
        var l = selected();
        if (!l || typeof window === 'undefined' || !window.open)
            return false;
        try {
            window.open('https://www.massassi.net/levels/files/' + l.id + '.shtml', '_blank', 'noopener');
        } catch (e) {
            return false;
        }
        return true;
    }

    function textOf(what, arg) {
        var l = selected();
        switch (what) {
        case 0: return statusText();
        case 1: return L.rows[arg] ? rowText(L.rows[arg]) : '';
        case 2: return SORTS[L.sort][1];
        case 3: return l ? plain(l.t) : '';
        case 4: return l ? overviewText(l, detail(l)) : '';
        case 5: return l ? readmeText(detail(l)) : '';
        case 6: return l ? filesText(l, detail(l)) : '';
        case 7: return messageText();
        }
        return '';
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
            progress(null, 'Opening ' + (l.z || l.id + '.zip'));
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
                progress(null, 'Checking ' + g.n + ', episode "' + g.ep + '"');
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
            progress(total ? got / total : 0, 'Getting ' + l.t + ': ' + size(got) + ' of ' + size(total));
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

    function begin(game) {
        if (!core())
            return false;
        L.game = game === 'mots' ? 'mots' : 'jk1';
        L.state = STATE_OPEN;
        L.result = '';
        L.done = false;
        L.msg = '';
        L.error = '';
        L.rows = [];
        L.changed = CH_ALL;
        load();
        return true;
    }

    function end() {
        if (L.job)
            L.job.abort.abort();
        L.job = null;
        L.state = STATE_CLOSED;
        shot.on = false;
        if (shot.img)
            shot.img.style.display = 'none';
        if (Module.canvas && Module.canvas.focus)
            Module.canvas.focus();
    }

    function number(what) {
        switch (what) {
        case 0: { var c = L.changed; L.changed = 0; return c; }
        case 1: return L.rows.length;
        case 2: {
            for (var i = 0; i < L.rows.length; i++) {
                if (L.rows[i].id === L.sel)
                    return i;
            }
            return -1;
        }
        case 3: return canUse() ? 1 : 0;
        case 4: return L.job ? 1 : 0;
        case 5: return L.done ? 1 : 0;
        case 6: { var l = selected(); return l && l.sh > 0 ? l.sh : 0; }
        }
        return 0;
    }

    Module.jkLibrary = {
        available: available,
        begin: begin,
        end: end,
        // Text for the menu (textOf's numbers), and numbers (number's).
        text: textOf,
        number: number,
        query: setQuery,
        select: function(i) {
            var l = L.rows[i | 0];
            if (l)
                pickId(l.id);
        },
        use: use,
        cancel: cancel,
        page: openPage,
        shot: setShot,
        nextShot: function() {
            var l = selected();
            if (l && l.sh > 1) {
                L.shot = L.shot % l.sh + 1;
                if (shot.raf === 0 && typeof requestAnimationFrame === 'function')
                    shot.raf = requestAnimationFrame(shotLayout);
            }
        },
        // The episode the library installed, '' if none.
        take: function() {
            var name = L.result;
            L.result = '';
            return name;
        },
        // For tests.
        _L: L, _zipEntries: zipEntries, _dateText: dateText, _shot: shot,
        _ch: { STATUS: CH_STATUS, ROWS: CH_ROWS, PANEL: CH_PANEL, MSG: CH_MSG, USE: CH_USE }
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
