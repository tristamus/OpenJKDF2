// The Mod Library for the browser build: the community mods the site keeps a
// copy of, in the Level Library's game menu (src/Gui/jkGUILibrary.c), which
// opens from Mod Library on the main menu (src/Gui/jkGUIMain.c). Bundled into
// openjkdf2.js with --pre-js after library.js (cmake_modules/plat_wasm.cmake),
// whose helpers it uses, so it always matches the engine it ships with.
//
// The site's Worker (src/levels.js in the jkdf2mp-bb-server repository)
// copies Massassi.net's two mod lists (JK and MotS) the way it copies levels,
// and the mods in jkdf2.net's archive that Massassi doesn't have (no page,
// screenshots, rating or downloads; their title from their readme or zip,
// and f the names of their files, to find them by), works out what each mod
// is, and serves
//
//   /levels/mods/all/jk1.json, /levels/mods/all/mots.json  the mods of both
//                         that are ready; one of jkdf2.net's has o: 'jkdf2.net'
//   /levels/mods/jk1.json, /levels/mods/mots.json  Massassi's alone (what this
//                         reads from a site without the other)
//   /levels/d/<id>.json   one mod's description, readmes, files, and its plan
//                         (detail.mod): which zip entries make up the mod's
//                         folder and where each goes, which are episodes,
//                         what's inside its GOBs
//   /levels/z/<id>.zip?h=<sha256>, /levels/s/<id>-<n>.jpg  as for levels
//
// This file gives the menu its text and numbers as library.js does, with the
// Mod Library's filters (Gameplay, Characters, Graphics, My mods, Campaigns),
// versions of one mod together (newest first, the older ones under it), and a
// Changes tab: which of the game's own files the mod replaces, by name.
//
// Get Mod gets the zip, checks it, puts the plan's files in a mod folder's
// order and packs them as mods.js packs a mod folder, so its fingerprint is
// the one the same folder would have, and saves the pack with mods.js
// (Module.jkMods.library): there it's a mod the start page can tick under
// Choose mods, and one a joiner can use when a host runs it. A mod's episode
// GOBs are saved and written into episode/ as the Level Library's are.
// Restart With It unpacks the pack and sets it as the engine's next mod; the
// menu returns its folder and the engine starts again with it, in this tab.
// Nothing goes to the player's game folder.

(function() {
    var STATE_OPEN = 0, STATE_CLOSED = 1;
    var BASE = '/levels/';
    var RECHECK_MS = 60000;
    var BULK_DATE = '2000-06-02';
    var EXT = { jk1: 'gob', mots: 'goo' };
    var KINDS = { gameplay: 'Gameplay', characters: 'Characters', graphics: 'Graphics', campaign: 'Campaign',
                  other: 'Other' };
    // The kind after a row's title, as the Level Library has CTF and SP.
    var TAGS = { characters: 'Skin', graphics: 'GFX', campaign: 'SP' };
    var SORTS = [['dl', 'Most downloaded'], ['name', 'Name'], ['new', 'Newest first'], ['old', 'Oldest first'],
                 ['rating', 'Top rated'], ['size', 'Smallest first']];
    // What changed since the menu last asked (jkGUILibrary.c's LIB_CH_*).
    var CH_STATUS = 1, CH_ROWS = 2, CH_PANEL = 4, CH_MSG = 8, CH_USE = 16, CH_PV = 32, CH_ALL = 127;

    var M = {
        state: STATE_CLOSED, result: '', done: false, game: 'jk1',
        avail: {}, checkedAt: {},
        index: {}, details: {}, saved: {}, savedEps: {}, retail: {},
        rows: [], sel: null, q: '',
        filters: { gameplay: true, characters: true, graphics: true, mine: false, campaigns: false }, sort: 0,
        job: null, ready: null, restarting: false, msg: '', error: '', loadFailed: false, changed: 0
    };

    function U() { return Module.jkLibrary && Module.jkLibrary.util; }
    function core() { return Module.jkMaps && Module.jkMaps.core; }
    function mods() { return Module.jkMods && Module.jkMods.library ? Module.jkMods : null; }

    function isMots() {
        return (Module['arguments'] || []).indexOf('-motsCompat') >= 0;
    }

    // ---- Whether the site serves the library, asked as the engine starts and
    // again now and then while it doesn't.

    function ask(game) {
        if (typeof fetch !== 'function' || typeof location === 'undefined' || !/^https?:$/.test(location.protocol))
            return;
        var now = Date.now();
        if (M.checkedAt[game] && now - M.checkedAt[game] < RECHECK_MS)
            return;
        M.checkedAt[game] = now;
        fetch(BASE + 'mods/' + game + '.json', { method: 'HEAD' }).then(function(res) {
            M.avail[game] = res.ok && /json/.test(res.headers.get('Content-Type') || '');
        }, function() {});
    }

    function available() {
        var game = isMots() ? 'mots' : 'jk1';
        if (!M.avail[game])
            ask(game);
        return !!M.avail[game] && !!core() && !!U() && !!mods() && typeof document !== 'undefined';
    }

    // ---- Small helpers

    function plain(s) { return U().plain(s); }
    function size(b) { return U().size(b); }
    function dateText(iso, p) { return U().dateText(iso, p); }
    function fail(text) { return U().fail(text); }
    function count(n, one, many) { return n.toLocaleString('en-US') + ' ' + (n === 1 ? one : many || one + 's'); }

    function sortDate(l) { return l.sd || l.rel || l.add || ''; }
    function year(l) { return (l.rel || l.add || '').substring(0, 4) || '?'; }

    function newestFirst(a, b) {
        return sortDate(b).localeCompare(sortDate(a)) || b.id - a.id;
    }

    async function getJson(path, signal) {
        var res = await fetch(BASE + path, { signal: signal });
        if (!res.ok)
            throw Object.assign(new Error('HTTP ' + res.status), { status: res.status });
        return res.json();
    }

    // ---- The list, from the site

    // Both sources' mods, or Massassi's alone from a site that doesn't have
    // that list (yet).
    async function loadIndex(game) {
        var have = M.index[game];
        if (have && Date.now() - have.at < 5 * 60000)
            return have.data;
        var data = null;
        try {
            data = await getJson('mods/all/' + game + '.json');
        } catch (e) {}
        if (!data || !Array.isArray(data.mods))
            data = await getJson('mods/' + game + '.json');
        if (!data || !Array.isArray(data.mods))
            throw new Error('not a mod list');
        M.index[game] = { at: Date.now(), data: data };
        return data;
    }

    // The mods saved in this browser by the library, by their id: the pack's
    // record (mods.js's store), and the episodes of mods that are only
    // episodes (maps.js's store), by the id too.
    async function loadSaved(game) {
        var saved = {}, eps = {};
        var metas = [];
        try { metas = await mods().library.list(game); } catch (e) {}
        metas.sort(function(a, b) { return a.used - b.used; });
        for (var m of metas)
            saved[String(m.lib)] = m;
        var epMetas = [];
        try { epMetas = await core().store.list(game); } catch (e) {}
        for (var e of epMetas) {
            if (e.lib != null)
                (eps[String(e.lib)] = eps[String(e.lib)] || []).push(e.name);
        }
        M.saved[game] = saved;
        M.savedEps[game] = eps;
    }

    function all() {
        var data = M.index[M.game] && M.index[M.game].data;
        return data ? data.mods : [];
    }

    function byId(id) {
        var list = all();
        for (var i = 0; i < list.length; i++) {
            if (list[i].id === id)
                return list[i];
        }
        return null;
    }

    function savedOf(l) {
        return l ? (M.saved[M.game] || {})[String(l.id)] || null : null;
    }

    function savedEpsOf(l) {
        return l ? (M.savedEps[M.game] || {})[String(l.id)] || null : null;
    }

    function isSaved(l) {
        return !!(savedOf(l) || (!(l.n > 0) && savedEpsOf(l)));
    }

    function running(l) {
        var s = savedOf(l), cur = mods() && mods().current();
        return !!(s && cur && cur.fp === s.sha256);
    }

    // The versions of l's mod in the library, newest first.
    function versionsOf(l) {
        return all().filter(function(x) { return x.gk === l.gk; }).sort(newestFirst);
    }

    function filterKey(l) {
        return l.k === 'campaign' ? 'campaigns' : l.k === 'characters' || l.k === 'graphics' ? l.k : 'gameplay';
    }

    // The rows: the mods that pass the filters, each mod's versions together,
    // the newest first and the older ones under it ({ l, older }). The sort
    // orders the mods by their newest version.
    function visible() {
        var q = M.q.trim().toLowerCase(), f = M.filters;
        var list = all().filter(function(l) {
            if (!f[filterKey(l)])
                return false;
            if (f.mine && !isSaved(l))
                return false;
            return !q || searchText(l).indexOf(q) >= 0;
        });
        var groups = new Map();
        for (var l of list) {
            var key = l.gk || String(l.id);
            if (!groups.has(key))
                groups.set(key, []);
            groups.get(key).push(l);
        }
        var heads = [];
        for (var g of groups.values()) {
            g.sort(newestFirst);
            heads.push(g);
        }
        var name = function(x, y) { return String(x.t).localeCompare(String(y.t), 'en', { sensitivity: 'base' }); };
        var by = {
            name: name,
            'new': function(x, y) { return sortDate(y).localeCompare(sortDate(x)) || name(x, y); },
            old: function(x, y) { return sortDate(x).localeCompare(sortDate(y)) || name(x, y); },
            dl: function(x, y) { return (y.dl || 0) - (x.dl || 0) || name(x, y); },
            rating: function(x, y) { return (y.r || 0) - (x.r || 0) || (y.dl || 0) - (x.dl || 0); },
            size: function(x, y) { return (x.sz || 0) - (y.sz || 0) || name(x, y); }
        }[SORTS[M.sort][0]];
        heads.sort(function(a, b) { return by(a[0], b[0]); });
        var rows = [];
        for (var h of heads) {
            h.forEach(function(l, i) { rows.push({ l: l, older: i > 0 }); });
        }
        return rows;
    }

    // What the search looks in: the title, the author, the zip's name, and
    // for one of jkdf2.net's the names of its models, scripts and sounds (a
    // pack of skins named only by its zip is found by its characters).
    function searchText(l) {
        return ((l.t || '') + ' ' + (l.a || '') + ' ' + (l.z || '') + ' ' + (l.f || []).join(' ')).toLowerCase();
    }

    // A mod from jkdf2.net's archive: no page there to open, no screenshots.
    function fromJkdf2(l) { return !!l && l.o === 'jkdf2.net'; }

    function touch(bits) { M.changed |= bits; }

    function selected() { return M.sel != null ? byId(M.sel) : null; }

    function statusText() {
        var have = M.index[M.game];
        if (M.loadFailed)
            return 'The Mod Library can\'t be reached right now. Go back and try again in a moment.';
        if (!have)
            return 'Getting the list of mods...';
        var data = have.data, n = data.mods.length;
        if (M.filters.mine && !M.rows.length)
            return M.q.trim() ? 'None of the mods saved in this browser match.' : 'No mods saved in this browser yet.';
        var both = Array.isArray(data.sources) && data.sources.length > 1;
        if (!n && data.left)
            return 'The site is still copying the mods from Massassi.net' + (both ? ' and jkdf2.net' : '') + ': ' +
                count(data.left, 'mod') + ' to go.';
        var other = data.from && data.from['jkdf2.net'] > 0;
        return count(n, 'mod') + ' from Massassi.net' + (other ? ' and jkdf2.net' : '') +
            (data.left ? ' so far, ' + data.left.toLocaleString('en-US') + ' more coming' : '');
    }

    function rowText(r) {
        var l = r.l;
        var mark = (r.older ? '  ' : '') + (isSaved(l) ? '* ' : '');
        var tag = TAGS[l.k] || '';
        var mb = (l.sz || 0) / (1024 * 1024);
        return plain(mark + l.t).replace(/[\n\t]/g, ' ') + '\t' + year(l) + ' ' +
            (mb >= 10 ? Math.round(mb) + ' MB' : size(l.sz)) + (tag ? '\t' + tag : '');
    }

    function sourceText(l) {
        var src = l.src === 'r' ? 'from its readme' : l.src === 'f' ? 'the date of the files in its zip' :
                  l.src === 'm' ? "Massassi's date" : '';
        var note = src;
        if (l.add && l.src !== 'm' && l.add.substring(0, 10) !== (l.rel || ''))
            note += (note ? '; ' : '') + 'Massassi says added ' + dateText(l.add, 'd') +
                    (l.add.substring(0, 10) === BULK_DATE ? ', the day it imported most older mods' : '');
        return note;
    }

    function overviewText(l, d) {
        var out = [];
        var fact = function(label, value) { out.push(label + ':\t' + plain(value).replace(/\n/g, ' ')); };
        var src = sourceText(l);
        fact('Author', l.a || 'unknown');
        fact('Released', dateText(l.rel || l.add, l.rel ? l.rp : 'd') + (src ? ' (' + src + ')' : ''));
        fact('Kind', l.gp ? 'Graphics pack' : KINDS[l.k] || l.k);
        fact('From', fromJkdf2(l) ? 'jkdf2.net (Massassi doesn\'t have it)' : 'Massassi.net');
        if (fromJkdf2(l))
            fact('Credit', U().DF2_CREDIT || 'jkdf2.net\'s archive kept it; copied with its owner\'s permission.');
        var what = [];
        if (l.n > 0)
            what.push(count(l.n, 'file') + ' for the game');
        if (l.ep)
            what.push(count(l.ep, 'episode'));
        fact('Size', size(l.sz) + ' zip' + (what.length ? ', ' + what.join(' and ') : ''));
        var versions = versionsOf(l);
        if (versions.length > 1) {
            var at = versions.indexOf(l);
            if (at === 0)
                fact('Versions', 'this is the newest of ' + versions.length + '; the older ones are under it in the list');
            else
                fact('Versions', 'an older one (' + (at + 1) + ' of ' + versions.length + '); the newest is ' +
                     versions[0].t + ', ' + dateText(versions[0].rel || versions[0].add, versions[0].rel ? versions[0].rp : 'd'));
        }
        if (l.f && l.f.length)
            fact('Includes', l.f.join(', '));
        var dls = l.dl ? l.dl.toLocaleString('en-US') + ' downloads on Massassi' : '';
        if (l.r != null)
            fact('Rating', l.r + ' of 10' + (l.v ? ' (' + l.v + ' votes)' : '') + (dls ? ', ' + dls : ''));
        else if (dls)
            fact('Rating', 'none yet, ' + dls);

        var notes = [];
        if (running(l))
            notes.push('The game is running with this mod now.');
        else if (savedOf(l))
            notes.push('Saved in this browser: Restart With It starts the game with it, and Choose mods on the start page lists it.');
        else if (savedEpsOf(l) && !(l.n > 0))
            notes.push('Saved in this browser: its episodes are on the Host screen.');
        if (l.gp)
            notes.push('A graphics pack: only you see it, whatever mod a game runs, and players with and without it play together.');
        else if (l.k === 'campaign')
            notes.push('A single-player campaign: the Host screen lists its episodes with Experimental Co-op ticked under Advanced.');
        else if (l.n > 0)
            notes.push('When you host with it, players who join get it from you.');
        if (l.x)
            notes.push('Its zip has ' + count(l.x, 'program') + ', which the browser can\'t run, so it\'s left out.');
        if (l.ty)
            notes.push('Its files were loose in its zip: the library put each in the game\'s folder for its kind.');
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
            return 'This mod\'s zip has no readme.';
        return plain(d.texts.map(function(t) { return '--- ' + t.n + ' ---\n\n' + U().readmeBody(t); }).join('\n\n'));
    }

    function filesText(d) {
        if (!d)
            return 'Getting the file list...';
        if (d.failed || !d.files)
            return 'The file list can\'t be shown right now.';
        var credit = U().df2Credit ? U().df2Credit(d) : null;
        return (credit ? credit + '\n\n' : '') +
            d.files.map(function(f) { return size(f.b) + '\t' + plain(f.n).replace(/\n/g, ' '); }).join('\n');
    }

    // ---- What it changes: the mod's files by name against the game's own
    // (its retail GOBs, maps.js's retailSources). A file the game has by the
    // same name is replaced while the mod runs.

    function norm(n) { return String(n).replace(/\\/g, '/').replace(/^\/+/, '').toLowerCase(); }
    function base(n) { var p = norm(n).split('/'); return p[p.length - 1]; }

    function retailNames(game) {
        if (M.retail[game])
            return M.retail[game];
        var names = null;
        try {
            var c = core();
            var sources = c.retailSources(c.gameRoot(), EXT[game]);
            if (sources.length) {
                names = new Set();
                for (var s of sources) {
                    for (var e of s.entries) {
                        if (e.name)
                            names.add(norm(e.name));
                    }
                }
            }
        } catch (e) {
            console.warn('OpenJKDF2: the Mod Library could not read the game\'s own files: ' + e);
        }
        M.retail[game] = names;
        return names;
    }

    var WHAT = [[/\.3do$/, 'model', 'models'], [/\.mat$/, 'texture', 'textures'], [/\.cog$/, 'script', 'scripts'],
                [/\.wav$/, 'sound', 'sounds'], [/\.key$/, 'animation', 'animations'], [/\.jkl$/, 'level', 'levels']];

    function whatOf(names) {
        var by = WHAT.map(function() { return 0; }), other = 0;
        for (var n of names) {
            var i = WHAT.findIndex(function(w) { return w[0].test(n); });
            if (i >= 0)
                by[i]++;
            else
                other++;
        }
        var parts = [];
        WHAT.forEach(function(w, i) {
            if (by[i])
                parts.push(count(by[i], w[1], w[2]));
        });
        if (other)
            parts.push(count(other, 'other file', 'other files'));
        return parts.join(', ');
    }

    function changesText(l, d) {
        if (!d)
            return 'Getting the mod\'s files...';
        if (d.failed || !d.mod || !d.mod.plan)
            return 'What this mod changes can\'t be shown right now.';
        var plan = d.mod.plan, inside = d.mod.inside || {};
        var names = [], unread = [];
        for (var pair of plan.files || []) {
            var entry = pair[0], rel = pair[1];
            if (/\.go[bo]$/i.test(rel)) {
                if (inside[entry]) {
                    for (var f of inside[entry])
                        names.push(norm(f[0]));
                } else {
                    unread.push(rel);
                }
            } else {
                names.push(norm(rel));
            }
        }
        var out = ['What Get Mod adds to the game, by file name. A file with the name of one of the game\'s own ' +
                   'takes its place while the mod runs.', ''];
        var retail = retailNames(M.game);
        if (l.gp)
            out.push('Kind:\tA graphics pack: the game loads it over its own files, and over any mod\'s.');
        if (!names.length && !(plan.episodes || []).length)
            out.push('Files:\tNone the game uses.');
        if (names.length && retail) {
            var replaced = names.filter(function(n) { return retail.has(n); });
            var added = names.filter(function(n) { return !retail.has(n); });
            if (replaced.length) {
                out.push('Replaces:\t' + count(replaced.length, 'of the game\'s own files', 'of the game\'s own files') +
                         ': ' + whatOf(replaced));
                var folders = new Map();
                for (var n of replaced) {
                    // By folder, so 3do/key's animations aren't with 3do's models.
                    var dir = n.lastIndexOf('/') > 0 ? n.substring(0, n.lastIndexOf('/')) : '(top)';
                    if (!folders.has(dir))
                        folders.set(dir, []);
                    folders.get(dir).push(base(n));
                }
                var list = Array.from(folders.entries()).sort(function(a, b) { return b[1].length - a[1].length; });
                for (var fd of list) {
                    var shown = fd[1].slice(0, 8);
                    // No tab: a folder's name can be longer than the labels' column.
                    out.push('  ' + plain(fd[0]) + ': ' + plain(shown.join(', ')) +
                             (fd[1].length > shown.length ? ', and ' + (fd[1].length - shown.length) + ' more' : ''));
                }
            } else {
                out.push('Replaces:\tNone of the game\'s own files.');
            }
            if (added.length)
                out.push('Adds:\t' + count(added.length, 'new file') + ': ' + whatOf(added));
        } else if (names.length) {
            out.push('Files:\t' + count(names.length, 'file') + ': ' + whatOf(names));
            out.push('', 'The game\'s own files can\'t be read here, so this can\'t say which of them it replaces.');
        }
        if (unread.length)
            out.push('GOBs:\t' + plain(unread.join(', ')) + ' (what\'s inside isn\'t listed)');
        if ((plan.episodes || []).length) {
            var eps = (d.mod.episodes || []).map(function(e) { return e.ep || e.n; }).filter(Boolean);
            out.push('Episodes:\t' + (eps.length ? plain(eps.join(', ')) : count(plan.episodes.length, 'episode')) +
                     ' (on the Host screen once you have the mod)');
        }
        var left = d.mod.left || {};
        if ((left.programs || []).length)
            out.push('Left out:\t' + plain(left.programs.map(base).join(', ')) + ': programs, which the browser can\'t run.');
        if (left.other)
            out.push(((left.programs || []).length ? '\t' : 'Left out:\t') +
                     count(left.other, 'file') + ' the game doesn\'t read from a mod' +
                     ((left.others || []).length ? ': ' + plain(left.others.slice(0, 8).map(base).join(', ')) +
                      (left.other > 8 ? ', and more' : '') : ''));
        return out.join('\n');
    }

    // One mod's details, the first time it's picked. waitDetail resolves when
    // they're in.
    function detail(l) {
        var d = M.details[l.id];
        if (d)
            return d.data;
        d = M.details[l.id] = { data: null };
        d.promise = getJson('d/' + l.id + '.json').then(function(data) {
            d.data = data;
        }, function(e) {
            d.data = { failed: true };
            console.warn('OpenJKDF2: the Mod Library has no details for ' + l.id + ': ' + e);
        }).then(function() {
            if (M.sel === l.id)
                touch(CH_PANEL);
            return d.data;
        });
        return null;
    }

    async function waitDetail(l) {
        var d = M.details[l.id];
        if (d && d.data && d.data.failed) {
            delete M.details[l.id];
            d = null;
        }
        if (!d) {
            detail(l);
            d = M.details[l.id];
        }
        return d.data || await d.promise;
    }

    // ---- The Use button: Get Mod, then Restart With It.

    function canUse() {
        var l = selected();
        if (M.job || !l || M.done)
            return false;
        if (savedOf(l))
            return !running(l);
        if (!(l.n > 0))
            return l.ep > 0 && !savedEpsOf(l);
        return true;
    }

    function useLabel() {
        var l = selected();
        return l && savedOf(l) ? 'Restart With It' : 'Get Mod';
    }

    function useHint() {
        var l = selected();
        if (!l)
            return '';
        if (savedOf(l))
            return l.gp ? 'Starts again with this graphics pack and the mod you run now.'
                        : 'Starts the game again with only this mod running.';
        return 'Saves the mod in this browser. Nothing goes to your game folder.';
    }

    function messageText() {
        if (M.job || M.msg)
            return M.msg;
        // Nothing otherwise: a hint of two lines, centered on the line under
        // this one, reaches up into it. Get Mod's hint says where mods go.
        return M.error || '';
    }

    function progress(fraction, text) {
        M.msg = plain(text) + (fraction != null ? ' (' + Math.round(Math.max(0, Math.min(1, fraction)) * 100) + '%)' : '');
        touch(CH_MSG);
    }

    function alive(job) {
        if (job.abort.signal.aborted)
            throw new Error('cancelled');
    }

    function use() {
        var l = selected();
        if (!l || !canUse())
            return false;
        if (savedOf(l))
            restartWith(l);
        else
            getMod(l);
        return true;
    }

    async function runJob(l, text, work) {
        var job = M.job = { l: l, game: M.game, abort: new AbortController() };
        M.error = '';
        M.msg = '';
        progress(null, text);
        touch(CH_USE);
        try {
            await work(job);
        } catch (e) {
            M.job = null;
            M.msg = '';
            touch(CH_MSG | CH_USE);
            if (job.abort.signal.aborted)
                return false;
            console.warn('OpenJKDF2: the Mod Library could not get ' + l.id + ': ' + (e && e.stack || e));
            M.error = plain(e && e.shown ? e.message : 'Something went wrong: ' + (e && e.message || e));
            return false;
        }
        M.job = null;
        touch(CH_MSG | CH_USE);
        return true;
    }

    function entryIn(list, name) {
        var want = String(name).toLowerCase();
        return list.find(function(z) { return z.name === name; }) ||
               list.find(function(z) { return z.name.toLowerCase() === want; });
    }

    // Whether a place in the mod's folder is one a mod folder's pack has:
    // mods.js leaves out (and so doesn't fingerprint) what packable() and
    // validRel() don't take, folders included.
    function packablePath(rel) {
        var lib = Module.jkMods.lib;
        if (!lib.validRel(rel))
            return false;
        var parts = rel.split('/');
        for (var i = 1; i <= parts.length; i++) {
            if (!lib.packable(parts.slice(0, i).join('/')))
                return false;
        }
        return true;
    }

    async function getMod(l) {
        var got = null;
        var ok = await runJob(l, 'Getting ' + l.t, async function(job) {
            got = await fetchMod(job);
        });
        if (!ok)
            return;
        M.ready = got.fp ? { id: l.id, fp: got.fp, bytes: got.bytes } : null;
        if (got.fp)
            M.msg = 'Saved in this browser. Restart With It plays it now.';
        else
            M.msg = 'Saved in this browser: ' + (got.eps.length === 1 ? 'its episode is' : 'its episodes are') + ' on the Host screen.';
        touch(CH_ROWS | CH_PANEL | CH_MSG | CH_USE | CH_STATUS);
    }

    async function fetchMod(job) {
        var l = job.l, game = job.game, u = U(), lib = Module.jkMods.lib;
        var d = await waitDetail(l);
        alive(job);
        if (!d || d.failed || !d.mod || !d.mod.plan)
            throw fail('This mod\'s details can\'t be got right now. Try again in a moment.');
        var plan = d.mod.plan;
        job.progress = function(got, total) {
            progress(total ? got / total : 0, 'Getting ' + l.t + ': ' + size(got) + ' of ' + size(total));
        };
        var zip = await u.download(job);
        alive(job);
        progress(null, 'Opening ' + (l.z || l.id + '.zip'));
        var list = u.zipEntries(zip), from = zip;
        if (plan.inner) {
            var ze = entryIn(list, plan.inner);
            if (!ze)
                throw fail('The zip doesn\'t have ' + plan.inner + ' in it.');
            from = await u.zipData(zip, ze);
            list = u.zipEntries(from);
        }

        // The mod's folder, in a mod folder's order (mods.js's listFiles).
        var files = [];
        for (var pair of plan.files || []) {
            var entry = pair[0], rel = pair[1];
            if (!packablePath(rel)) {
                console.warn('OpenJKDF2: the Mod Library leaves out ' + entry + ' of ' + l.id);
                continue;
            }
            var e = entryIn(list, entry);
            if (!e)
                throw fail('The zip doesn\'t have ' + entry + ' in it.');
            progress(files.length / plan.files.length, 'Unpacking ' + l.t);
            var bytes = await u.zipData(from, e);
            alive(job);
            files.push({ rel: rel, size: bytes.length, bytes: bytes });
        }
        files.sort(function(a, b) { return a.rel < b.rel ? -1 : a.rel > b.rel ? 1 : 0; });

        var out = { fp: null, bytes: null, eps: [] };
        if (files.length) {
            progress(null, 'Saving ' + l.t + ' in this browser');
            var p = lib.packFiles(files, function(f) { return f.bytes; });
            files = null;
            var fp = await u.sha256(p.bytes);
            alive(job);
            await mods().library.save(game, l.t, EXT[game], fp, p.bytes,
                                      { lib: l.id, title: l.t, author: l.a || '', kind: l.k, gp: l.gp || undefined });
            out.fp = fp;
            out.bytes = p.bytes;
            console.log('OpenJKDF2: the Mod Library saved ' + l.id + ' (' + l.t + '), ' + fp.substring(0, 16));
        }

        // Its episodes, as the Level Library writes a level's.
        for (var epEntry of plan.episodes || []) {
            var ee = entryIn(list, epEntry);
            if (!ee)
                throw fail('The zip doesn\'t have ' + epEntry + ' in it.');
            progress(null, 'Checking ' + epEntry);
            var data = await u.zipData(from, ee);
            alive(job);
            var sha = await u.sha256(data);
            var g = (l.g || []).find(function(x) { return String(x.n).toLowerCase() === epEntry.toLowerCase(); });
            if (g && g.h && g.h !== sha)
                throw fail(epEntry + ' isn\'t the one the library lists. Try again later.');
            out.eps.push(await u.placeEpisode(g || { n: epEntry, h: sha }, l, game, data, job));
        }
        await loadSaved(game);
        return out;
    }

    // A mod restarted with starts ticked in the start page's Choose mods
    // next time too (index.html's LIBRARY_ON).
    function playWith(game, id) {
        try {
            var key = 'jkdf2mp-library-mods-on-' + game;
            var ids = JSON.parse(localStorage.getItem(key) || '[]');
            if (!Array.isArray(ids))
                ids = [];
            ids = ids.map(String);
            if (ids.indexOf(String(id)) < 0)
                ids.push(String(id));
            localStorage.setItem(key, JSON.stringify(ids));
        } catch (e) {}
    }

    async function restartWith(l) {
        var s = savedOf(l), path = null;
        var ok = await runJob(l, 'Getting the game ready to start with ' + l.t, async function(job) {
            var have = M.ready && M.ready.id === l.id && M.ready.fp === s.sha256 ? M.ready.bytes : null;
            var p = await mods().library.prepare(s.sha256, s.title || l.t, have, !!s.gp);
            alive(job);
            path = p;
        });
        if (!ok)
            return;
        M.ready = null;
        M.result = path;
        M.restarting = true;
        playWith(M.game, l.id);
        if (s.gp) {
            var packs = Array.isArray(Module.jkGraphicsPacks) ? Module.jkGraphicsPacks : (Module.jkGraphicsPacks = []);
            if (packs.indexOf(s.title || l.t) < 0)
                packs.push(s.title || l.t);
        }
        M.done = true;
        progress(null, 'Starting the game again with ' + l.t + '...');
        touch(CH_USE);
    }

    function cancel() {
        if (!M.job)
            return false;
        M.job.abort.abort();
        return true;
    }

    function openPage() {
        var l = selected();
        if (!l || fromJkdf2(l) || typeof window === 'undefined' || !window.open)
            return false;
        try {
            window.open('https://www.massassi.net/levels/files/' + l.id + '.shtml', '_blank', 'noopener');
        } catch (e) {
            return false;
        }
        return true;
    }

    // ---- The screenshots: the first over the menu's picture frame, the
    // second over the frame where the Level Library has its 3D view (the
    // menu draws in the game's 256 colors), where the game draws its 640x480
    // menus: the canvas's full height, 4:3, in the middle (std3D_DrawMenu).

    function makeShot(n) {
        var s = { n: n, on: false, x: 0, y: 0, w: 0, h: 0, img: null, src: '', polled: 0, raf: 0 };

        function img() {
            if (s.img || typeof document === 'undefined' || !document.body)
                return s.img;
            var el = document.createElement('img');
            el.id = 'jk-mod-shot-' + n;
            el.alt = '';
            el.style.cssText = 'position:fixed;z-index:1;pointer-events:none;object-fit:contain;background:#000;display:none;';
            el.addEventListener('error', function() { el.style.visibility = 'hidden'; });
            el.addEventListener('load', function() { el.style.visibility = ''; });
            document.body.appendChild(el);
            s.img = el;
            return el;
        }

        function src() {
            var l = selected();
            if (!l || !(l.sh >= n) || M.state !== STATE_OPEN)
                return '';
            return BASE + 's/' + l.id + '-' + n + '.jpg';
        }

        function layout() {
            s.raf = 0;
            var el = s.img;
            if (!el)
                return;
            var c = Module.canvas;
            var want = s.on && src() && c && c.getBoundingClientRect && Date.now() - s.polled < 500;
            if (!want) {
                el.style.display = 'none';
                return;
            }
            var url = src();
            if (url !== s.src) {
                s.src = url;
                el.style.visibility = 'hidden';
                el.src = url;
            }
            var r = c.getBoundingClientRect();
            var scale = r.height / 480, left = r.left + (r.width - r.height * 4 / 3) / 2;
            el.style.left = (left + s.x * scale) + 'px';
            el.style.top = (r.top + s.y * scale) + 'px';
            el.style.width = (s.w * scale) + 'px';
            el.style.height = (s.h * scale) + 'px';
            el.style.display = 'block';
            s.raf = requestAnimationFrame(layout);
        }

        s.set = function(on, x, y, w, h) {
            s.on = !!on;
            s.x = x; s.y = y; s.w = w; s.h = h;
            s.polled = Date.now();
            if (!img())
                return;
            if (!s.raf && typeof requestAnimationFrame === 'function')
                s.raf = requestAnimationFrame(layout);
        };
        s.hide = function() {
            s.on = false;
            if (s.img)
                s.img.style.display = 'none';
        };
        return s;
    }

    var shots = [makeShot(1), makeShot(2)];

    // ---- The rows and what's picked

    function setQuery(q, mask, sort) {
        var f = { gameplay: !!(mask & 1), characters: !!(mask & 2), graphics: !!(mask & 4), mine: !!(mask & 8),
                  campaigns: !!(mask & 16) };
        sort = Math.max(0, Math.min(SORTS.length - 1, sort | 0));
        q = String(q || '');
        var same = q === M.q && sort === M.sort;
        for (var k in f)
            same = same && f[k] === M.filters[k];
        if (same)
            return;
        var mineChanged = f.mine !== M.filters.mine;
        M.q = q;
        M.filters = f;
        M.sort = sort;
        refreshRows();
        if (mineChanged)
            touch(CH_STATUS);
    }

    function refreshRows() {
        var wasEmpty = !M.rows.length;
        M.rows = visible();
        if (M.filters.mine && wasEmpty !== !M.rows.length)
            touch(CH_STATUS);
        if (!M.job && M.rows.length && !M.rows.some(function(r) { return r.l.id === M.sel; }))
            pickId(M.rows[0].l.id);
        if (!M.job && !M.rows.length && M.sel != null) {
            M.sel = null;
            M.error = '';
            touch(CH_PANEL | CH_USE | CH_MSG | CH_PV);
        }
        touch(CH_ROWS);
    }

    function pickId(id) {
        if (M.job || id === M.sel)
            return;
        M.sel = id;
        M.error = '';
        M.msg = '';
        var l = selected();
        if (l)
            detail(l);
        touch(CH_PANEL | CH_USE | CH_MSG | CH_PV);
    }

    function emptyTitle() {
        return M.filters.mine && !M.rows.length && M.index[M.game] ? 'My mods' : '';
    }

    function emptyText() {
        if (!emptyTitle())
            return '';
        if (M.q.trim())
            return 'None of the mods saved in this browser match the search.';
        return 'No mods saved in this browser yet.\n\nA mod you get here with Get Mod is saved in this browser and shows here.';
    }

    async function load() {
        var game = M.game;
        M.loadFailed = false;
        touch(CH_ALL);
        try {
            await loadIndex(game);
            await loadSaved(game);
            if (game !== M.game || M.state !== STATE_OPEN)
                return;
            if (M.sel != null && !byId(M.sel))
                M.sel = null;
            refreshRows();
            if (M.sel == null && M.rows.length)
                pickId(M.rows[0].l.id);
            else if (selected())
                detail(selected());
        } catch (e) {
            if (game !== M.game)
                return;
            M.loadFailed = true;
            console.warn('OpenJKDF2: the Mod Library list did not load: ' + e);
        }
        touch(CH_ALL);
    }

    function textOf(what, arg) {
        var l = selected();
        switch (what) {
        case 0: return statusText();
        case 1: return M.rows[arg] ? rowText(M.rows[arg]) : '';
        case 2: return SORTS[M.sort][1];
        case 3: return l ? plain(l.t) : emptyTitle();
        case 4: return l ? overviewText(l, detail(l)) : emptyText();
        case 5: return l ? readmeText(detail(l)) : '';
        case 6: return l ? filesText(detail(l)) : '';
        case 7: return messageText();
        case 14: return l ? changesText(l, detail(l)) : '';
        case 15: return useLabel();
        case 16: return useHint();
        }
        return '';
    }

    function number(what) {
        switch (what) {
        case 0: {
            var c = M.changed;
            M.changed = 0;
            return c;
        }
        case 1: return M.rows.length;
        case 2: {
            for (var i = 0; i < M.rows.length; i++) {
                if (M.rows[i].l.id === M.sel)
                    return i;
            }
            return -1;
        }
        case 3: return canUse() ? 1 : 0;
        case 4: return M.job ? 1 : 0;
        case 5: return M.done ? 1 : 0;
        case 6: { var l = selected(); return l && l.sh > 0 ? Math.min(l.sh, 2) : 0; }
        case 7: return 0;
        // The picked mod has no page to open (Massassi Page): one of jkdf2.net's.
        case 8: return fromJkdf2(selected()) ? 1 : 0;
        }
        return 0;
    }

    // ---- The engine's side

    function begin(game) {
        if (!core() || !U() || !mods())
            return false;
        M.game = game === 'mots' ? 'mots' : 'jk1';
        M.state = STATE_OPEN;
        M.result = '';
        M.restarting = false;
        M.done = false;
        M.msg = '';
        M.error = '';
        M.rows = [];
        M.changed = CH_ALL;
        load();
        return true;
    }

    function end() {
        if (M.job)
            M.job.abort.abort();
        M.job = null;
        M.ready = null;
        M.state = STATE_CLOSED;
        shots.forEach(function(s) { s.hide(); });
        if (Module.canvas && Module.canvas.focus)
            Module.canvas.focus();
    }

    Module.jkModLibrary = {
        available: available,
        begin: begin,
        end: end,
        text: textOf,
        number: number,
        query: setQuery,
        select: function(i) {
            var r = M.rows[i | 0];
            if (r)
                pickId(r.l.id);
        },
        use: use,
        cancel: cancel,
        page: openPage,
        shot: function(on, x, y, w, h) { shots[0].set(on, x, y, w, h); },
        preview: function(on, x, y, w, h) { shots[1].set(on, x, y, w, h); },
        view: function() {},
        viewCommand: function() {},
        nextShot: function() {},
        // The folder Restart With It starts the engine in ('' for none). The
        // engine starts again as the menu takes it, so mods.js's next mod is
        // its running one from now on (restart(), as for a host's mod).
        take: function() {
            var path = M.result;
            M.result = '';
            if (M.restarting) {
                M.restarting = false;
                if (mods())
                    path = mods().restart();
            }
            return path;
        },
        // For tests.
        _M: M, _visible: visible, _searchText: searchText, _changesText: changesText, _packablePath: packablePath,
        _ch: { STATUS: CH_STATUS, ROWS: CH_ROWS, PANEL: CH_PANEL, MSG: CH_MSG, USE: CH_USE, PV: CH_PV }
    };

    Module.preRun = Module.preRun || [];
    if (typeof Module.preRun === 'function')
        Module.preRun = [Module.preRun];
    Module.preRun.push(function() {
        ask(isMots() ? 'mots' : 'jk1');
    });
})();
