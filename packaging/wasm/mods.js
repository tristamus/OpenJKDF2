// Full mods for the browser build: a folder of files the engine loads ahead
// of the game's own (`-path <folder>`, src/Main/jkRes.c), such as a saber
// mod. Bundled into openjkdf2.js after maps.js (cmake_modules/plat_wasm.cmake)
// and built on its transfer.
//
// The page (index.html) puts each mod folder it finds in the picked game
// folder under jkmods/ and starts the engine with the one the player chose.
// A host's lobby entry carries a short tail naming its mod: a name and a
// fingerprint, the SHA-256 of the mod packed into one GOB (pack()). The pack
// is built the same way everywhere, so the same files give the same
// fingerprint.
//
// A joiner whose mod isn't the host's gets the host's before the level loads:
// from a mod folder of its own with the same fingerprint, from one saved in an
// earlier game, or else from the host, sent the same way as a map (KIND_MOD
// frames). As with maps, original game files never cross the wire: every file
// in the pack that is a copy of a file in the retail GOBs, or of a whole
// retail GOB, is sent as a reference the joiner fills in from its own copy.
// Then the engine restarts into the mod in the same tab, and the player joins
// again with the game already selected (stdComm_WSRelay.c).
//
// Mods that came from a host are kept in IndexedDB and unpacked into
// netmods/<first 16 hex digits of the fingerprint> when needed, so they can
// be chosen next time from the folder prompt.
//
// The lobby tail, hex-encoded after the 240-byte jkMultiEntry:
//   "JKMD" <version u8> <name length u8> <fingerprint: 32 bytes> <name>

(function() {
    var maps = Module.jkMaps;
    if (!maps || !maps.core)
        return;
    var core = maps.core;
    var lib = maps.lib;
    var KIND_MOD = core.KIND_MOD;

    var TAIL_MAGIC = [0x4A, 0x4B, 0x4D, 0x44]; // "JKMD"
    var TAIL_VERSION = 1;
    var MAX_NAME = 63;
    // What can be sent, like a map; bigger mods are still fingerprinted, so a
    // joiner with the same folder can use theirs.
    var MAX_MOD_BYTES = 100 * 1024 * 1024;
    var MAX_MOD_FILES = 10000;
    var MAX_PACK_BYTES = 1024 * 1024 * 1024;
    var MAX_PACK_FILES = 50000;
    var MAX_STORED_MOD_BYTES = 500 * 1024 * 1024;
    var MAX_PATH = 127; // a GOB entry name is char[128]
    var MAX_DEPTH = 8;
    var GOB_ENTRY = 136;
    // A pack of MAX_MOD_BYTES of files, with its header and table.
    var MAX_SEND_BYTES = MAX_MOD_BYTES + 16 + MAX_MOD_FILES * GOB_ENTRY;

    var store = core.makeStore('jkdf2mp-mods', MAX_STORED_MOD_BYTES);

    function FS() { return Module.FS; }

    function isMots() {
        return (Module['arguments'] || []).indexOf('-motsCompat') >= 0 || /\/mots$/.test(core.gameRoot());
    }

    function gameExt() { return isMots() ? 'goo' : 'gob'; }
    function gameName() { return isMots() ? 'mots' : 'jk1'; }

    function gameTitle(ext) {
        return ext === 'goo' ? 'Mysteries of the Sith' : 'Jedi Knight';
    }

    function isDir(path) {
        try { return FS().isDir(FS().stat(path).mode); } catch (e) { return false; }
    }

    // ---- The pack: every file of the mod in one GOB, in a fixed order.

    // Files that differ between copies of the same mod without mattering to
    // the game, so they'd only make two copies look different.
    function packable(rel) {
        var parts = rel.split('/');
        for (var p of parts) {
            if (p[0] === '.' || p.toLowerCase() === '__macosx')
                return false;
        }
        var base = parts[parts.length - 1].toLowerCase();
        return base !== 'thumbs.db' && base !== 'desktop.ini' &&
               !/\.(exe|dll|ico|ini|txt|url|pdf|htm|html|zip|rar|7z|bak|tmp)$/.test(base);
    }

    // A path a pack may hold: relative, at most MAX_DEPTH folders deep, and
    // nothing Windows wouldn't allow in a file name.
    function validRel(rel) {
        if (typeof rel !== 'string' || !rel.length || rel.length > MAX_PATH || !/^[\x20-\x7e]+$/.test(rel))
            return false;
        var parts = rel.split('/');
        if (parts.length > MAX_DEPTH + 1)
            return false;
        for (var p of parts) {
            if (!p || p === '.' || p === '..' || /[\\:*?"<>|]/.test(p) || /[ .]$/.test(p))
                return false;
        }
        return true;
    }

    // The mod's files under dir, sorted, as { rel, path, size }.
    function listFiles(dir) {
        var fs = FS();
        var out = [];
        (function walk(path, rel, depth) {
            var names = fs.readdir(path);
            for (var n of names) {
                if (n === '.' || n === '..')
                    continue;
                var p = path + '/' + n;
                var r = rel ? rel + '/' + n : n;
                var st = fs.stat(p);
                if (fs.isDir(st.mode)) {
                    if (depth < MAX_DEPTH && packable(r))
                        walk(p, r, depth + 1);
                } else if (fs.isFile(st.mode) && packable(r)) {
                    if (validRel(r))
                        out.push({ rel: r, path: p, size: st.size });
                    else
                        console.warn('OpenJKDF2: left out of the mod (name not shareable): ' + r);
                }
            }
        })(dir, '', 0);
        out.sort(function(a, b) { return a.rel < b.rel ? -1 : a.rel > b.rel ? 1 : 0; });
        return out;
    }

    function fail(reason) {
        return core.fail(reason);
    }

    // Packs the files into a GOB: "GOB ", version 20, the table's offset, the
    // files back to back, then the table (Win95/stdGob.h).
    function packFiles(files, readFile) {
        if (files.length > MAX_PACK_FILES)
            throw fail('too-many-files');
        var dataBytes = 0;
        for (var f of files)
            dataBytes += f.size;
        if (dataBytes > MAX_PACK_BYTES)
            throw fail('too-big');
        var tableOffs = 12 + dataBytes;
        var total = tableOffs + 4 + files.length * GOB_ENTRY;
        var out = new Uint8Array(total);
        var v = new DataView(out.buffer);
        out.set([0x47, 0x4F, 0x42, 0x20], 0);
        v.setUint32(4, 20, true);
        v.setUint32(8, tableOffs, true);
        v.setUint32(tableOffs, files.length, true);
        var at = 12;
        var entries = [];
        files.forEach(function(f, i) {
            var bytes = readFile(f);
            if (bytes.length !== f.size)
                throw fail('changed');
            out.set(bytes, at);
            var e = tableOffs + 4 + i * GOB_ENTRY;
            v.setUint32(e, at, true);
            v.setUint32(e + 4, f.size, true);
            for (var j = 0; j < f.rel.length; j++)
                out[e + 8 + j] = f.rel.charCodeAt(j);
            entries.push({ offset: at, size: f.size, name: f.rel });
            at += f.size;
        });
        return { bytes: out, entries: entries };
    }

    function pack(dir) {
        return packFiles(listFiles(dir), function(f) { return FS().readFile(f.path); });
    }

    // What may be sent as references: each file, and each file inside a GOB
    // the mod carries.
    function shareRanges(p) {
        var ranges = [];
        for (var e of p.entries) {
            ranges.push(e);
            if (!/\.go[bo]$/i.test(e.name))
                continue;
            var inner = p.bytes.subarray(e.offset, e.offset + e.size);
            var dir = lib.parseGob(function(o, n) { return inner.subarray(o, o + n); }, inner.length);
            if (!dir)
                continue;
            for (var ie of dir.entries)
                ranges.push({ offset: e.offset + ie.offset, size: ie.size });
        }
        return ranges;
    }

    // Writes a pack's files into dir, after checking every name in it.
    function unpack(bytes, dir) {
        var gob = lib.parseGob(function(o, n) { return bytes.subarray(o, o + n); }, bytes.length);
        if (!gob)
            throw fail('corrupt');
        if (gob.entries.length > MAX_MOD_FILES)
            throw fail('too-many-files');
        var seen = {};
        for (var e of gob.entries) {
            var lower = e.name.toLowerCase();
            if (!validRel(e.name) || seen[lower])
                throw fail('bad-files');
            seen[lower] = true;
        }
        // A file and a folder can't share a name.
        for (var e of gob.entries) {
            var parts = e.name.toLowerCase().split('/');
            for (var i = 1; i < parts.length; i++) {
                if (seen[parts.slice(0, i).join('/')])
                    throw fail('bad-files');
            }
        }
        var fs = FS();
        try {
            fs.mkdirTree(dir);
            for (var e of gob.entries) {
                var path = dir + '/' + e.name;
                fs.mkdirTree(path.substring(0, path.lastIndexOf('/')));
                fs.writeFile(path, bytes.slice(e.offset, e.offset + e.size));
            }
        } catch (err) {
            throw fail('install');
        }
        return gob.entries.length;
    }

    // ---- The lobby tail.

    function cleanName(name) {
        name = String(name || '').replace(/[^\x20-\x7e]/g, '?').trim();
        return (name || 'mod').substring(0, MAX_NAME);
    }

    function encodeTail(fp, name) {
        name = cleanName(name);
        var b = new Uint8Array(4 + 2 + 32 + name.length);
        b.set(TAIL_MAGIC, 0);
        b[4] = TAIL_VERSION;
        b[5] = name.length;
        for (var i = 0; i < 32; i++)
            b[6 + i] = parseInt(fp.substr(i * 2, 2), 16);
        for (var j = 0; j < name.length; j++)
            b[38 + j] = name.charCodeAt(j);
        return lib.toHex(b);
    }

    // null when the host runs no mod, { fp, name } for its mod, or
    // { bad: true } for a tail this page can't read.
    function parseTail(hex) {
        hex = String(hex || '');
        if (!hex)
            return null;
        if (hex.length % 2 || !/^[0-9a-fA-F]+$/.test(hex) || hex.length < 2 * 38)
            return { bad: true };
        var b = new Uint8Array(hex.length / 2);
        for (var i = 0; i < b.length; i++)
            b[i] = parseInt(hex.substr(i * 2, 2), 16);
        if (b[0] !== TAIL_MAGIC[0] || b[1] !== TAIL_MAGIC[1] || b[2] !== TAIL_MAGIC[2] ||
            b[3] !== TAIL_MAGIC[3] || b[4] !== TAIL_VERSION || b[5] > MAX_NAME || b.length !== 38 + b[5])
            return { bad: true };
        var name = '';
        for (var j = 0; j < b[5]; j++)
            name += String.fromCharCode(b[38 + j]);
        return { fp: lib.toHex(b.subarray(6, 38)), name: cleanName(name) };
    }

    // ---- Which mod this tab runs, and the mods it could switch to.

    var current = null;   // { fp, name, path } of the running mod, or null
    var target = null;    // where the next restart goes, the same shape
    var fingerprints = {}; // path -> Promise<fp>
    var unpacked = {};    // netmods folder name -> fp, this session

    // Mod folders the page found and loaded: [{ name, path }], path relative
    // to the game folder.
    function folders() {
        return Array.isArray(Module.jkModFolders) ? Module.jkModFolders : [];
    }

    function fingerprint(path) {
        if (!fingerprints[path]) {
            fingerprints[path] = (async function() {
                return lib.sha256(pack(core.gameRoot() + '/' + path).bytes);
            })();
            fingerprints[path].catch(function() { delete fingerprints[path]; });
        }
        return fingerprints[path];
    }

    function netPath(fp) {
        return 'netmods/' + fp.substring(0, 16);
    }

    function nameOfPath(path) {
        for (var f of folders()) {
            if (f.path === path)
                return f.name;
        }
        var parts = path.split('/');
        return parts[parts.length - 1];
    }

    // Saved from an earlier game: unpacked into netmods/, after checking it's
    // still what was saved.
    async function fromStore(root, fp, name) {
        var saved = null;
        try { saved = await store.find(gameName(), fp); } catch (e) {}
        if (!saved)
            return null;
        var bytes = new Uint8Array(saved.data);
        if (await lib.sha256(bytes) !== fp)
            return null;
        var path = netPath(fp);
        unpack(bytes, root + '/' + path);
        unpacked[path] = fp;
        return { fp: fp, name: cleanName(name || saved.meta.name), path: path };
    }

    // ---- Host.

    function tail() {
        return current && current.fp ? encodeTail(current.fp, current.name) : '';
    }

    function host() {
        var c = current;
        if (!c || !c.fp) {
            core.withdraw(KIND_MOD);
            return;
        }
        core.offer(KIND_MOD, async function() {
            var root = core.gameRoot();
            var ext = gameExt();
            var info = { status: 'error', name: c.name, ext: ext };
            var p;
            try {
                p = pack(root + '/' + c.path);
            } catch (e) {
                info.status = e.reason || 'error';
                return { info: info };
            }
            var sha = await lib.sha256(p.bytes);
            if (sha !== c.fp) {
                info.status = 'changed';
                return { info: info };
            }
            if (p.entries.length > MAX_MOD_FILES) {
                info.status = 'too-many-files';
                return { info: info };
            }
            if (p.entries.reduce(function(n, e) { return n + e.size; }, 0) > MAX_MOD_BYTES) {
                info.status = 'too-big';
                return { info: info };
            }
            var plan = await lib.planShareRanges(p.bytes, shareRanges(p), core.retailSources(root, ext));
            if (!plan.ok) {
                info.status = plan.reason;
                return { info: info };
            }
            console.log('OpenJKDF2: ready to send mod ' + c.name + ' to joiners (' + p.entries.length + ' files, ' +
                        plan.refs + ' parts left out as original game files, ' + plan.stream.length + ' of ' +
                        p.bytes.length + ' bytes to send)');
            return {
                info: { status: 'ok', name: c.name, ext: ext, size: p.bytes.length, sha256: sha,
                        streamSize: plan.stream.length, refsBytes: plan.refsBytes },
                stream: plan.stream
            };
        });
    }

    function unhost() {
        core.withdraw(KIND_MOD);
    }

    // ---- Joiner.

    var MESSAGES = {
        'none': "The host's mod couldn't be read on their side.",
        'error': "The host couldn't get its mod ready to send.",
        'too-big': 'This mod is over 100 MB, too big to send. To join, put your own copy of it in your game folder.',
        'too-many-files': 'This mod has too many files to send.',
        'retail-only': "The host's mod is made only of original game files, so it isn't sent.",
        'corrupt': "The mod didn't arrive intact, so it wasn't installed.",
        'mismatch': "The mod didn't arrive intact, so it wasn't installed.",
        'install': "The mod couldn't be set up in this tab.",
        'bad-files': "The host's mod has file names that can't be saved here, so it wasn't installed.",
        'changed': "The host's mod files changed after their game started. They need to reload the page.",
        'other-mod': 'The host is running a different mod now.',
        'unreadable': "This game's mod details couldn't be read. One of you may need to reload the page."
    };

    function messageFor(reason) {
        var ext = gameExt();
        if (reason === 'other-game')
            return 'This game is for ' + gameTitle(ext === 'goo' ? 'gob' : 'goo') +
                   ', and the game folder you picked is ' + gameTitle(ext) + '.';
        if (reason === 'missing-retail')
            return 'This mod uses original ' + gameTitle(ext) +
                   " files that weren't found in the game folder you picked.";
        if (reason in MESSAGES)
            return MESSAGES[reason];
        if (/^(no-answer|stalled|host-left|lobby-lost|cancelled)$/.test(reason))
            return undefined; // worded the same as for maps
        return "The host's mod couldn't be sent (" + reason + ').';
    }

    function fetchMod(tailHex) {
        var want = parseTail(tailHex);
        core.startJob(KIND_MOD, {
            want: want,
            label: want && !want.bad ? want.name : 'Unknown mod',
            ext: gameExt(),
            messageFor: messageFor
        }, runJob);
    }

    function cancel() {
        core.abort('cancelled', KIND_MOD);
    }

    async function runJob(j) {
        var want = j.want;
        if (want && want.bad)
            throw fail('unreadable');
        var have = current && current.fp ? current.fp : null;
        if ((want ? want.fp : null) === have)
            return core.STATE_READY;

        if (!want) {
            target = { fp: null, name: null, path: '' };
            core.ui.notice('Restarting without your mod', current.name,
                           "The host isn't using a mod, so the game restarts without yours. When the main " +
                           'menu is back, join from Multiplayer again: this game will already be selected.');
            return core.STATE_RESTART;
        }

        var found = await findLocal(j, want);
        if (!found) {
            var info = await askHost(j, want);
            found = await findBySize(j, want, info) || await download(j, want, info);
        }
        if (!found.downloaded)
            console.log('OpenJKDF2: using mod ' + want.name + ' from ' + found.path);
        core.check(j);
        target = found;
        core.ui.notice("Restarting with the host's mod", want.name,
                       'The game restarts to load it. When the main menu is back, join from Multiplayer ' +
                       'again: this game will already be selected.');
        return core.STATE_RESTART;
    }

    async function matchFolder(j, want, f) {
        var fp = null;
        try { fp = await fingerprint(f.path); } catch (e) {}
        core.check(j);
        return fp === want.fp ? { fp: fp, name: f.name, path: f.path } : null;
    }

    // The host's mod among this player's own, without asking the host:
    // unpacked earlier this session, a mod folder of the same name with the
    // same files, or saved from an earlier game.
    async function findLocal(j, want) {
        var path = netPath(want.fp);
        if (unpacked[path] === want.fp && isDir(core.gameRoot() + '/' + path))
            return { fp: want.fp, name: want.name, path: path };

        var lower = want.name.toLowerCase();
        for (var f of folders()) {
            if (f.name.toLowerCase() === lower) {
                var found = await matchFolder(j, want, f);
                if (found)
                    return found;
            }
        }

        var saved = await fromStore(core.gameRoot(), want.fp, want.name);
        core.check(j);
        return saved;
    }

    // What the host says about its mod. Its size picks out which other mod
    // folders could hold the same files.
    async function askHost(j, want) {
        j.checking = setTimeout(function() {
            if (!j.done)
                core.ui.show(j.label, "Checking the host's mod...", cancel, KIND_MOD);
        }, core.CHECKING_UI_MS);
        j.timers.push(j.checking);
        var info = await core.askInfo(j);
        if (info.status === 'ok') {
            if (info.ext !== gameExt())
                throw fail('other-game');
            core.checkInfo(info, MAX_SEND_BYTES);
            if (info.sha256 !== want.fp)
                throw fail('other-mod');
        }
        return info;
    }

    // A mod folder under another name with the same files. Packs of the same
    // files are the same size, which listing a folder tells without reading
    // it. When the host can't send its mod there's no size to go by, so
    // every folder is tried.
    async function findBySize(j, want, info) {
        var lower = want.name.toLowerCase();
        for (var f of folders()) {
            if (f.name.toLowerCase() === lower)
                continue;
            if (info.status === 'ok') {
                var size = 0;
                try {
                    var files = listFiles(core.gameRoot() + '/' + f.path);
                    size = 16 + files.length * GOB_ENTRY;
                    for (var file of files)
                        size += file.size;
                } catch (e) {}
                if (size !== info.size)
                    continue;
            }
            var found = await matchFolder(j, want, f);
            if (found)
                return found;
        }
        if (info.status !== 'ok')
            throw fail(info.status);
        return null;
    }

    async function download(j, want, info) {
        var root = core.gameRoot();
        var ext = gameExt();
        clearTimeout(j.checking);
        core.ui.show(j.label, 'Connecting to the host...', cancel, KIND_MOD);
        await core.waitForDirect(j);
        var stream = await core.download(j, info, core.directUp() ? core.CHUNK_DIRECT : core.CHUNK_RELAY);

        var result = await lib.rebuild(info, stream, core.retailSources(root, ext));
        if (!result.ok)
            throw fail(result.reason);
        core.check(j);
        var path = netPath(want.fp);
        var count = unpack(result.bytes, root + '/' + path);
        unpacked[path] = want.fp;
        console.log('OpenJKDF2: installed mod ' + want.name + ' from the host (' + count + ' files)');
        store.put(gameName(), want.name, ext, want.fp, result.bytes).catch(function(e) {
            console.warn('OpenJKDF2: could not save mod ' + want.name + ' for next time: ' + e);
        });
        return { fp: want.fp, name: want.name, path: path, downloaded: true };
    }

    // Called by the engine as it restarts into the target: the path to give
    // it, which is now the running mod.
    function restart() {
        var t = target;
        target = null;
        current = t && t.fp ? { fp: t.fp, name: t.name, path: t.path } : null;
        return t ? t.path : '';
    }

    // One line for the join screen about the selected game's mod.
    function describe(tailHex) {
        var want = parseTail(tailHex);
        var have = current && current.fp ? current : null;
        // The page names several mods "A + B + C", ending in "+ N more" when
        // that's too long. Shortened, that's the first and how many more.
        function several(n) { return n.split(' + ').length > 1; }
        function short(n) {
            if (n.length <= 24)
                return n;
            var parts = n.split(' + ');
            if (parts.length < 2)
                return n.substring(0, 23) + '...';
            var more = parts.length - 1;
            var m = /^(\d+) more$/.exec(parts[parts.length - 1]);
            if (m)
                more += Number(m[1]) - 1;
            var rest = ' + ' + more + ' more';
            var first = parts[0];
            return (first.length > 24 - rest.length ? first.substring(0, 21 - rest.length) + '...' : first) + rest;
        }
        if (want && want.bad)
            return 'Mod: unknown';
        if (want && have && want.fp === have.fp)
            return (several(want.name) ? 'Mods: ' : 'Mod: ') + short(want.name);
        if (want && several(want.name))
            return 'Mods: ' + short(want.name) + ' (the game restarts to load them)';
        if (want)
            return 'Mod: ' + short(want.name) + ' (the game restarts to load it)';
        if (have)
            return 'No mod (the game restarts without ' + short(have.name) + ')';
        return '';
    }

    Module.jkMods = {
        lib: { pack: pack, packFiles: packFiles, unpack: unpack, shareRanges: shareRanges, listFiles: listFiles,
               encodeTail: encodeTail, parseTail: parseTail, validRel: validRel, packable: packable },
        tail: tail,
        host: host,
        unhost: unhost,
        fetch: fetchMod,
        abort: function(reason) { core.abort(reason, KIND_MOD); },
        state: function() { return core.state(KIND_MOD); },
        restart: restart,
        describe: describe,
        current: function() { return current; }
    };

    // Works out which mod the engine starts with, from -path: a mod folder the
    // page loaded, or a mod saved from an earlier game, which is unpacked
    // first. Its fingerprint is needed before anyone can host or join.
    function pathArg() {
        var args = Module['arguments'] || [];
        for (var i = 0; i < args.length - 1; i++) {
            if (/^[-\/]path$/i.test(args[i]))
                return args[i + 1];
        }
        return null;
    }

    Module.preRun = Module.preRun || [];
    if (typeof Module.preRun === 'function')
        Module.preRun = [Module.preRun];
    Module.preRun.push(function() {
        var path = pathArg();
        if (!path)
            return;
        path = path.replace(/\\/g, '/').replace(/^\.\//, '').replace(/\/+$/, '');
        Module.addRunDependency('jk-mods');
        (async function() {
            var root = isMots() ? '/mots' : '/jk1';
            var m = /^netmods\/([0-9a-f]{16})$/.exec(path);
            if (m && !isDir(root + '/' + path)) {
                var metas = [];
                try { metas = await store.list(gameName()); } catch (e) {}
                metas.sort(function(a, b) { return b.used - a.used; });
                for (var meta of metas) {
                    if (meta.sha256.substring(0, 16) === m[1]) {
                        current = await fromStore(root, meta.sha256, meta.name);
                        break;
                    }
                }
                if (!current)
                    console.warn('OpenJKDF2: the saved mod ' + path + " couldn't be loaded from this browser's storage");
            } else if (isDir(root + '/' + path)) {
                var fp = await lib.sha256(pack(root + '/' + path).bytes);
                fingerprints[path] = Promise.resolve(fp);
                current = { fp: fp, name: cleanName(nameOfPath(path)), path: path };
            } else {
                console.warn('OpenJKDF2: no mod folder at ' + path);
            }
            if (current)
                console.log('OpenJKDF2: running mod ' + current.name + ' (' + current.fp.substring(0, 16) + ')');
        })().catch(function(e) {
            current = null;
            console.warn('OpenJKDF2: could not read the mod in ' + path + ': ' + e);
        }).finally(function() {
            Module.removeRunDependency('jk-mods');
        });
    });
})();
