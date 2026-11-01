// Automatic map downloads for the browser build. Bundled into openjkdf2.js
// with --pre-js (cmake_modules/plat_wasm.cmake), so it always matches the
// engine it ships with.
//
// When a player joins a game whose episode GOB they don't have, or have a
// different version of, their tab gets it from the host's tab: over a second
// WebRTC data channel when the direct link is up, else through the lobby as
// binary frames it forwards like game packets (stdComm_WSRelay.c). Nothing is
// stored on or served from the lobby.
//
// Original game files never cross the wire. The original episodes are never
// offered, and any file inside a custom GOB that is byte-for-byte identical
// to a file in the host's retail GOBs, or to a whole retail GOB, is replaced
// by a reference (offset, size, SHA-256). The joiner fills those parts in
// from its own retail GOBs and checks the rebuilt GOB against the host's
// SHA-256 before installing it. A GOB made only of retail files (a renamed
// original episode) isn't sent. A joiner whose game has other versions of
// some of those files, such as from a fan pack, leaves them out of the map
// and plays it with its own (ownVersions).
//
// Downloaded maps are kept in IndexedDB and written into episode/ before the
// engine starts, so they're there next time and can be hosted.
//
// The same transfer also carries the host's mod (mods.js): KIND_MOD frames
// ask for and send it, KIND_MAP frames the map.
//
// Every frame starts like a relay game packet, with idFrom and idTo (u32),
// then MAGIC, a type byte, a version byte and a kind byte, padded to 16 bytes:
//   joiner -> host   INFO_REQ
//                    CHUNK_REQ  <id:8> <chunkSize u32> <first u32> <count u32>
//                    BLOCK_REQ  <id:8> <first u32> <count u32> <partSize u32>
//                               <accept u8> <pad:3>
//   host -> joiner   INFO       <JSON: status, name, ext, size, sha256,
//                                streamSize, refsBytes, z, streamSha256>
//                    CHUNK      <id:8> <index u32> <data>
//                    BLOCK      <id:8> <block u32> <zlen u32> <at u32>
//                               <codec u8> <pad:3> <data>
// <id> is the first 8 bytes of the file's SHA-256, so chunks of another file
// are never mixed in. The stream the chunks cut up is the reference table (u32
// count, then u32 offset, u32 size and a 32-byte SHA-256 per reference),
// followed by the file's bytes outside the references, in order; a map's
// host with references says the stream's SHA-256 as streamSha256. Game packets
// start with a DSS message id below DSS_MAX (66), so their byte 8 is never
// MAGIC's first byte ('J', 74). Pages from before mods leave the kind byte at
// 0 and ignore it, so they only ever answer for their map.
//
// A host that says z (BLOCK_BYTES) in its INFO also sends the stream in
// blocks of that many bytes, each squeezed with deflate where the joiner can
// unsqueeze it (accept 1) and that makes it smaller (codec 1, else 0, as
// is), and cut into parts of partSize: zlen is the block's size as sent and
// at where the part goes in it. Mods are mostly text and uncompressed
// textures and sounds: SBX-JK Continuum's 305 MiB GOB goes as 82 MB. The host
// squeezes blocks as they're asked for, in a worker, and keeps them for the
// next joiner. The joiner asks for a few blocks at a time on each way to the
// host it has (lanes): the map channel, the extra links the network code
// opens for a transfer (stdComm_WSRelay.c, net.mapBulk), or the relay, and
// puts each block in place as it comes, so the file is only in memory once.
// A browser's data channel carries only so much at a time however fast the
// connection (about 2 MB/s at a 50 ms round trip), and each link has its own,
// so four go about four times as fast. Pages from before blocks only ask for
// chunks, which the host still sends, and a host from before them never says
// z, so the joiner asks it for chunks.

(function() {
    var MAGIC = [0x4A, 0x4B, 0x4D, 0x54]; // "JKMT"
    var VERSION = 1;
    var HEADER = 16;
    var T_INFO_REQ = 1, T_INFO = 2, T_CHUNK_REQ = 3, T_CHUNK = 4, T_BLOCK_REQ = 5, T_BLOCK = 6;
    var KIND_MAP = 0, KIND_MOD = 1;

    // What can be sent: as much as a browser tab can comfortably hold twice
    // over, once as the file and once while checking it.
    var MAX_MAP_BYTES = 1024 * 1024 * 1024;
    var MAX_STORED_BYTES = 2048 * 1024 * 1024;
    var MAX_REFS = 65536;
    var REF_BYTES = 40;
    // The stream is copied whole to be hashed for streamSha256.
    var MAX_STREAM_HASH_BYTES = 256 * 1024 * 1024;
    // The lobby closes sockets that send more than 64 KiB in one message
    // (jkdf2mp-bb-server cloudflare/src/lobby.js), and data channel messages
    // are best kept small.
    var CHUNK_DIRECT = 16 * 1024;
    var CHUNK_RELAY = 48 * 1024;
    var WINDOW_BYTES = 1024 * 1024;
    var MAX_CHUNKS_PER_REQ = 128;
    // Blocks: every browser takes data channel messages of 64 KiB and more.
    var BLOCK_BYTES = 1024 * 1024;
    var PART_DIRECT = 64 * 1024;
    var PART_RELAY = 48 * 1024;
    var BLOCK_HEADER = 24;
    var MAX_BLOCKS_PER_REQ = 4;
    // Blocks asked for and not yet in, on each way to the host: through the
    // lobby, at most twice what the chunks had out at once, and a quarter of
    // that for most mods.
    var BLOCKS_DIRECT = 3;
    var BLOCKS_RELAY = 2;
    // Blocks squeezed ahead of the joiner's asking.
    var BLOCKS_AHEAD = 6;
    var CODEC_STORED = 0, CODEC_DEFLATE = 1;
    var DIRECT_WAIT_MS = 3000;
    var INFO_RESEND_MS = 5000;
    var INFO_TIMEOUT_MS = 20000;
    var CHUNK_RESEND_MS = 5000;
    var STALL_MS = 15000;
    var CHECKING_UI_MS = 600;

    // STATE_RESTART: the engine has to restart to use what was fetched (a mod).
    var STATE_BUSY = 0, STATE_READY = 1, STATE_FAILED = 2, STATE_RESTART = 3;

    var GOB_ENTRY = 136; // u32 offset, u32 size, char[128] name (Win95/stdGob.h)

    // The original episodes, by name, and the retail GOBs whose files are
    // never sent (src/Main/InstallHelper.c).
    var RETAIL_EPISODES = ['jk1', 'jk1ctf', 'jk1mp', 'jk1demo', 'jk1mpdem', 'jk1mpdemo',
                           'jkm', 'jkm_kfy', 'jkm_mp', 'jkm_saber'];
    var RETAIL_FILES = {
        gob: ['episode/JK1.gob', 'episode/JK1CTF.gob', 'episode/JK1MP.gob',
              'episode/jk1demo.gob', 'episode/jk1mpdem.gob', 'episode/jk1mpdemo.gob',
              'resource/Res1hi.gob', 'resource/Res1low.gob', 'resource/Res2.gob',
              'resource/res1demo.gob'],
        goo: ['episode/JKM.goo', 'episode/JKM_KFY.goo', 'episode/JKM_MP.goo', 'episode/JKM_SABER.goo',
              'resource/Jkmres.goo', 'resource/JKMsndLO.goo']
    };

    function isRetailEpisode(name) {
        return RETAIL_EPISODES.indexOf(String(name).toLowerCase()) >= 0;
    }

    function gameOfEpisode(name) {
        return /^jkm/i.test(name) ? 'Mysteries of the Sith' : 'Jedi Knight';
    }

    // Episode names come from the other player, and become a file name here:
    // at most 31 characters (jkMultiEntry), nothing Windows wouldn't allow in
    // a file name, and no way out of episode/.
    function validEpisodeName(name) {
        return typeof name === 'string' && /^[^\/\\:*?"<>|\x00-\x1f.][^\/\\:*?"<>|\x00-\x1f]{0,30}$/.test(name) &&
               name.indexOf('..') < 0;
    }

    function validExt(ext) {
        return ext === 'gob' || ext === 'goo';
    }

    function toHex(buf) {
        var b = new Uint8Array(buf);
        var s = '';
        for (var i = 0; i < b.length; i++)
            s += (b[i] < 16 ? '0' : '') + b[i].toString(16);
        return s;
    }

    function fromHex(hex, out, at) {
        for (var i = 0; i < 32; i++)
            out[at + i] = parseInt(hex.substr(i * 2, 2), 16);
    }

    async function sha256(bytes) {
        return toHex(await crypto.subtle.digest('SHA-256', bytes));
    }

    // Bytes made of pieces back to back, such as a mod's pack (its header,
    // each file as the file system holds it, its table) or the stream sent
    // for a file, so neither is copied whole to be sent.
    function pieces(parts) {
        parts = parts.filter(function(p) { return p.length > 0; });
        var starts = [];
        var length = 0;
        for (var p of parts) {
            starts.push(length);
            length += p.length;
        }
        function find(offset) {
            var lo = 0, hi = parts.length - 1;
            while (lo < hi) {
                var mid = (lo + hi + 1) >> 1;
                if (starts[mid] <= offset)
                    lo = mid;
                else
                    hi = mid - 1;
            }
            return lo;
        }
        // The views that make up n bytes from offset.
        function views(offset, n) {
            var out = [];
            if (n <= 0)
                return out;
            for (var i = find(offset), at = offset - starts[i]; n > 0; i++, at = 0) {
                var take = Math.min(parts[i].length - at, n);
                out.push(parts[i].subarray(at, at + take));
                n -= take;
            }
            return out;
        }
        // n bytes from offset: a view where they're in one piece, else a copy.
        function read(offset, n) {
            var v = views(offset, n);
            if (v.length === 1)
                return v[0];
            var out = new Uint8Array(n);
            var at = 0;
            for (var x of v) {
                out.set(x, at);
                at += x.length;
            }
            return out;
        }
        return {
            length: length,
            views: views,
            read: read,
            bytes: function() { return read(0, length); }
        };
    }

    // ---- GOB tools. They read through read(offset, length) -> Uint8Array, so
    // the same code works on a GOB in memory and on one in the file system.

    function memReader(bytes) {
        return function(offset, length) { return bytes.subarray(offset, offset + length); };
    }

    // The file list of a GOB, or null if it isn't one.
    function parseGob(read, fileSize) {
        if (fileSize < 16)
            return null;
        var head = read(0, 12);
        if (head[0] !== 0x47 || head[1] !== 0x4F || head[2] !== 0x42 || head[3] !== 0x20) // "GOB "
            return null;
        var hv = new DataView(head.buffer, head.byteOffset, 12);
        if (hv.getUint32(4, true) !== 20)
            return null;
        var tableOffs = hv.getUint32(8, true);
        if (tableOffs + 4 > fileSize)
            return null;
        var countBytes = read(tableOffs, 4);
        var count = new DataView(countBytes.buffer, countBytes.byteOffset, 4).getUint32(0, true);
        if (tableOffs + 4 + count * GOB_ENTRY > fileSize)
            return null;
        var table = read(tableOffs + 4, count * GOB_ENTRY);
        var tv = new DataView(table.buffer, table.byteOffset, table.length);
        var entries = [];
        for (var i = 0; i < count; i++) {
            var at = i * GOB_ENTRY;
            var offset = tv.getUint32(at, true);
            var size = tv.getInt32(at + 4, true);
            if (size < 0 || offset + size > fileSize)
                return null;
            var name = '';
            for (var j = 0; j < 128 && table[at + 8 + j]; j++)
                name += String.fromCharCode(table[at + 8 + j]);
            entries.push({ offset: offset, size: size, name: name });
        }
        return { entries: entries };
    }

    // Finds files by content among a set of GOBs. sources is a list of
    // { read, entries }; hashes are worked out per size, only when a file of
    // that size is looked up.
    function contentIndex(sources) {
        var bySize = new Map();
        sources.forEach(function(src) {
            src.entries.forEach(function(e) {
                if (e.size <= 0)
                    return;
                var list = bySize.get(e.size);
                if (!list)
                    bySize.set(e.size, list = []);
                list.push({ src: src, entry: e });
            });
        });
        var hashed = new Map(); // size -> Promise<Map(sha -> candidate)>

        function hashesOfSize(size) {
            var p = hashed.get(size);
            if (!p) {
                p = (async function() {
                    var m = new Map();
                    for (var c of bySize.get(size) || []) {
                        var h = await sha256(c.src.read(c.entry.offset, c.entry.size));
                        if (!m.has(h))
                            m.set(h, c);
                    }
                    return m;
                })();
                hashed.set(size, p);
            }
            return p;
        }

        return {
            hasSize: function(size) { return bySize.has(size); },
            find: async function(size, sha) {
                if (!bySize.has(size))
                    return null;
                return (await hashesOfSize(size)).get(sha) || null;
            }
        };
    }

    // Splits a custom GOB into what may be sent: the references to retail
    // files and the bytes outside them.
    async function planShare(gob, retailSources) {
        var dir = parseGob(memReader(gob), gob.length);
        if (!dir)
            return { ok: false, reason: 'not-gob' };
        return planShareRanges(gob, dir.entries, retailSources);
    }

    // The same for any bytes, in one piece or in pieces(): each range
    // ({ offset, size }) that is a copy of a file in the retail GOBs becomes
    // a reference. The stream is in pieces too, views of the bytes given.
    async function planShareRanges(gob, ranges, retailSources) {
        var src = gob.views ? gob : pieces([gob]);
        var index = contentIndex(retailSources);
        var refs = [];
        var custom = 0;
        for (var e of ranges) {
            if (e.size <= 0)
                continue;
            var hit = null;
            var sha = null;
            if (index.hasSize(e.size)) {
                sha = await sha256(src.read(e.offset, e.size));
                hit = await index.find(e.size, sha);
            }
            if (hit)
                refs.push({ offset: e.offset, size: e.size, sha: sha });
            else
                custom++;
        }
        if (!custom)
            return { ok: false, reason: 'retail-only' };

        // A reference inside another (a file of a nested GOB that is itself a
        // copy, or a second entry for the same bytes) adds nothing.
        refs.sort(function(a, b) { return a.offset - b.offset || b.size - a.size; });
        var kept = [];
        var end = 0;
        for (var r of refs) {
            if (kept.length && r.offset + r.size <= end)
                continue;
            kept.push(r);
            end = Math.max(end, r.offset + r.size);
        }
        refs = kept;
        if (refs.length > MAX_REFS)
            return { ok: false, reason: 'too-many-files' };

        var table = refTable(refs);
        var refsBytes = table.length;

        // Entries can share or overlap bytes; the parts that are copies of
        // retail files are the union of the references.
        var parts = [table];
        var pos = 0;
        function inline(from, to) {
            for (var v of src.views(from, to - from))
                parts.push(v);
        }
        for (var r of refs) {
            if (r.offset > pos)
                inline(pos, r.offset);
            pos = Math.max(pos, r.offset + r.size);
        }
        if (pos < src.length)
            inline(pos, src.length);
        return { ok: true, stream: pieces(parts), refsBytes: refsBytes, refs: refs.length };
    }

    // The reference table a stream starts with.
    function refTable(refs) {
        var table = new Uint8Array(4 + refs.length * REF_BYTES);
        var tv = new DataView(table.buffer);
        tv.setUint32(0, refs.length, true);
        refs.forEach(function(r, i) {
            var at = 4 + i * REF_BYTES;
            tv.setUint32(at, r.offset, true);
            tv.setUint32(at + 4, r.size, true);
            fromHex(r.sha, table, at + 8);
        });
        return table;
    }

    // The reference table at the start of a stream, or null if it doesn't
    // fit what the host said.
    function parseRefs(head, info) {
        var sv = new DataView(head.buffer, head.byteOffset, head.length);
        var count = sv.getUint32(0, true);
        if (count > MAX_REFS || 4 + count * REF_BYTES !== info.refsBytes || head.length < info.refsBytes)
            return null;
        var refs = [];
        for (var i = 0; i < count; i++) {
            var at = 4 + i * REF_BYTES;
            var r = { offset: sv.getUint32(at, true), size: sv.getUint32(at + 4, true),
                      sha: toHex(head.subarray(at + 8, at + 40)) };
            if (!r.size || r.offset + r.size > info.size || (refs.length && r.offset < refs[refs.length - 1].offset))
                return null;
            refs.push(r);
        }
        return refs;
    }

    // Copies the references' bytes in from the joiner's own retail GOBs, then
    // checks the whole against the host's SHA-256. With own set (a map), a
    // reference the joiner has no exact copy of can be left to its own
    // version of the file instead (ownVersions).
    async function fillRefs(out, refs, info, retailSources, own) {
        var index = contentIndex(retailSources);
        var missing = [];
        for (var r of refs) {
            var hit = await index.find(r.size, r.sha);
            if (hit)
                out.set(hit.src.read(hit.entry.offset, hit.entry.size), r.offset);
            else
                missing.push(r);
        }
        if (missing.length)
            return own ? ownVersions(out, refs, missing, info, retailSources) : { ok: false, reason: 'missing-retail' };
        if (await sha256(out) !== info.sha256)
            return { ok: false, reason: 'mismatch' };
        return { ok: true, bytes: out };
    }

    // GOB entry names as the engine looks them up (stdGob_FileOpen).
    function gobName(name) {
        return name.toLowerCase().replace(/\//g, '\\');
    }

    // A fan pack such as MotS Remastered replaces some of the game's own
    // files (the JKMsndLO.goo its 3.0 installer puts in resource/ has other
    // versions of 1,002 of the original 1,222 sounds), so a map that bundles
    // a copy of an original file can't be rebuilt byte for byte from that
    // player's GOBs. It doesn't need to be: each such file is left out of the
    // map, and the engine opens the player's own version of it from the
    // game's resource GOBs instead, as it does in the original levels. Only
    // files the joiner has under the same name in a GOB the engine always
    // loads (resource/) are left out, and only whole entries; anything else
    // is still missing-retail. The rest of the GOB is checked against the
    // SHA-256 of the stream the host sent (streamSha256: the reference table
    // and the bytes outside it), since the whole file's can't match. Nothing
    // more is asked of the host, and no original file is sent.
    async function ownVersions(out, refs, missing, info, retailSources) {
        var hashed = typeof info.streamSha256 === 'string' && /^[0-9a-f]{64}$/.test(info.streamSha256);
        if (hashed && await sha256(sentStream(out, refs).bytes()) !== info.streamSha256)
            return { ok: false, reason: 'mismatch' };
        var mine = {}, episodes = {};
        retailSources.forEach(function(src) {
            src.entries.forEach(function(e) {
                if (e.name && !src.spare)
                    (src.always ? mine : episodes)[gobName(e.name)] = true;
            });
        });
        var dir = parseGob(memReader(out), out.length);
        if (!dir)
            return { ok: false, reason: hashed ? 'not-gob' : 'missing-retail' };
        var keep = [], lacking = [];
        for (var e of dir.entries) {
            var hits = missing.filter(function(m) {
                return e.size > 0 && e.offset < m.offset + m.size && m.offset < e.offset + e.size;
            });
            if (!hits.length) {
                keep.push(e);
                continue;
            }
            var inside = hits.length === 1 && hits[0].offset <= e.offset &&
                         e.offset + e.size <= hits[0].offset + hits[0].size;
            if (!inside || !mine[gobName(e.name)])
                lacking.push(e.name + (!inside ? ' (part of a file)' : episodes[gobName(e.name)] ? ' (only in an episode GOB)' : ''));
        }
        // Which ones, for whoever reads the console.
        if (lacking.length) {
            console.log('OpenJKDF2: original files in this map that this game folder has no copy of: ' +
                        lacking.slice(0, 20).join(', ') + (lacking.length > 20 ? ' and ' + (lacking.length - 20) + ' more' : ''));
            return { ok: false, reason: 'missing-retail' };
        }
        // A host from before this can't vouch for what it sent.
        if (!hashed)
            return { ok: false, reason: 'retail-changed' };
        return { ok: true, bytes: makeGob(out, keep), own: dir.entries.length - keep.length };
    }

    // What the host sent for a GOB: its reference table, then its bytes
    // outside the references, in order, as planShareRanges cut it.
    function sentStream(out, refs) {
        var parts = [refTable(refs)];
        var pos = 0;
        for (var r of refs) {
            if (r.offset > pos)
                parts.push(out.subarray(pos, r.offset));
            pos = Math.max(pos, r.offset + r.size);
        }
        if (pos < out.length)
            parts.push(out.subarray(pos));
        return pieces(parts);
    }

    // A GOB of the given entries of another (bytes): its table first, as the
    // game's own GOBs have it, then each entry's bytes, once for entries
    // that share them.
    function makeGob(bytes, entries) {
        var tableAt = 12;
        var at = tableAt + 4 + entries.length * GOB_ENTRY;
        var placed = new Map(); // "offset/size" -> where it goes
        var spans = [];
        var where = entries.map(function(e) {
            var k = e.offset + '/' + e.size;
            if (!placed.has(k)) {
                placed.set(k, at);
                spans.push({ from: e.offset, to: at, size: e.size });
                at += e.size;
            }
            return placed.get(k);
        });
        var gob = new Uint8Array(at);
        var v = new DataView(gob.buffer);
        gob.set([0x47, 0x4F, 0x42, 0x20]); // "GOB "
        v.setUint32(4, 20, true);
        v.setUint32(8, tableAt, true);
        v.setUint32(tableAt, entries.length, true);
        entries.forEach(function(e, i) {
            var t = tableAt + 4 + i * GOB_ENTRY;
            v.setUint32(t, where[i], true);
            v.setUint32(t + 4, e.size, true);
            for (var j = 0; j < e.name.length && j < 128; j++)
                gob[t + 8 + j] = e.name.charCodeAt(j) & 0xFF;
        });
        for (var s of spans)
            gob.set(bytes.subarray(s.from, s.from + s.size), s.to);
        return gob;
    }

    // Puts a GOB back together from a stream and the joiner's own retail
    // GOBs, and checks it against the host's SHA-256.
    async function rebuild(info, stream, retailSources, own) {
        if (stream.length !== info.streamSize || info.refsBytes < 4 || info.refsBytes > stream.length)
            return { ok: false, reason: 'corrupt' };
        var refs = parseRefs(stream, info);
        if (!refs)
            return { ok: false, reason: 'corrupt' };

        var out = new Uint8Array(info.size);
        var src = info.refsBytes;
        var pos = 0;
        for (var r of refs) {
            if (r.offset > pos) {
                var n = r.offset - pos;
                if (src + n > stream.length)
                    return { ok: false, reason: 'corrupt' };
                out.set(stream.subarray(src, src + n), pos);
                src += n;
            }
            pos = Math.max(pos, r.offset + r.size);
        }
        if (stream.length - src !== info.size - pos)
            return { ok: false, reason: 'corrupt' };
        out.set(stream.subarray(src), pos);
        return fillRefs(out, refs, info, retailSources, own);
    }

    // The same as the stream comes in, a piece at a time in any order (put),
    // each straight into place, so the GOB is only in memory once. Pieces
    // before the reference table is whole wait for it. finish() resolves as
    // rebuild() does, once every byte has been put.
    function rebuilder(info, retailSources, own) {
        var head = new Uint8Array(info.refsBytes);
        var headLeft = info.refsBytes;
        var early = [];
        var refs = null, gaps = null, out = null;
        var placed = 0;

        // Where each run of the stream after the table goes in the GOB: the
        // bytes between references, in order.
        function plan() {
            refs = parseRefs(head, info);
            if (!refs)
                throw fail('corrupt');
            gaps = [];
            var s = info.refsBytes, pos = 0;
            for (var r of refs) {
                if (r.offset > pos) {
                    gaps.push({ s: s, p: pos, n: r.offset - pos });
                    s += r.offset - pos;
                }
                pos = Math.max(pos, r.offset + r.size);
            }
            if (pos < info.size) {
                gaps.push({ s: s, p: pos, n: info.size - pos });
                s += info.size - pos;
            }
            if (s !== info.streamSize)
                throw fail('corrupt');
            try {
                out = new Uint8Array(info.size);
            } catch (e) {
                throw fail('memory');
            }
        }

        function scatter(s, bytes) {
            var end = s + bytes.length;
            var lo = 0, hi = gaps.length;
            while (lo < hi) {
                var mid = (lo + hi) >> 1;
                if (gaps[mid].s + gaps[mid].n <= s)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            for (var i = lo; i < gaps.length && gaps[i].s < end; i++) {
                var g = gaps[i];
                var a = Math.max(s, g.s), b = Math.min(end, g.s + g.n);
                out.set(bytes.subarray(a - s, b - s), g.p + a - g.s);
                placed += b - a;
            }
        }

        return {
            // The stream's bytes from offset s, each given once.
            put: function(s, bytes) {
                if (gaps) {
                    scatter(s, bytes);
                    return;
                }
                if (s < info.refsBytes) {
                    var n = Math.min(bytes.length, info.refsBytes - s);
                    head.set(bytes.subarray(0, n), s);
                    headLeft -= n;
                }
                early.push({ s: s, bytes: bytes });
                if (headLeft > 0)
                    return;
                plan();
                var waiting = early;
                early = null;
                for (var w of waiting)
                    scatter(w.s, w.bytes);
            },
            finish: function() {
                if (!gaps || placed !== info.streamSize - info.refsBytes)
                    return Promise.resolve({ ok: false, reason: 'corrupt' });
                return fillRefs(out, refs, info, retailSources, own);
            }
        };
    }

    var lib = { parseGob: parseGob, planShare: planShare, planShareRanges: planShareRanges,
                rebuild: rebuild, rebuilder: rebuilder, pieces: pieces, sha256: sha256, toHex: toHex,
                isRetailEpisode: isRetailEpisode, validEpisodeName: validEpisodeName };

    // ---- The file system side.

    function FS() { return Module.FS; }

    // MEMFS is case-sensitive, Windows game folders aren't: find "a/b.gob"
    // under root whatever its case.
    function findPath(root, rel) {
        var fs = FS();
        var path = root;
        var parts = rel.split('/');
        for (var i = 0; i < parts.length; i++) {
            var names;
            try { names = fs.readdir(path); } catch (e) { return null; }
            var want = parts[i].toLowerCase();
            var found = null;
            for (var n of names) {
                if (n.toLowerCase() === want) { found = n; break; }
            }
            if (!found)
                return null;
            path = path + '/' + found;
        }
        return path;
    }

    function isFile(path) {
        try { return FS().isFile(FS().stat(path).mode); } catch (e) { return false; }
    }

    // A file's bytes as the file system holds them: MEMFS keeps each file in
    // one array, so this is a view of it rather than a copy where it can be.
    function fileBytes(path) {
        var fs = FS();
        try {
            var node = fs.lookupPath(path).node;
            if (node.contents instanceof Uint8Array && typeof node.usedBytes === 'number')
                return node.contents.subarray(0, node.usedBytes);
        } catch (e) {}
        return fs.readFile(path);
    }

    function fileReader(path) {
        var fs = FS();
        return function(offset, length) {
            var buf = new Uint8Array(length);
            var stream = fs.open(path, 'r');
            try { fs.read(stream, buf, 0, length, offset); } finally { fs.close(stream); }
            return buf;
        };
    }

    // The retail GOBs this install has, with their file lists. always: one
    // in resource/, which the engine loads whatever the level. spare: a copy
    // of one in a folder in resource/, which the engine never loads, kept
    // when a fan pack replaced it (the MotS Remastered pack keeps the
    // original JKMsndLO.goo in resource/Backup Sounds/; the page reads it,
    // see isSpareRetail in index.html). Its files are found by content only.
    function retailSources(root, ext) {
        var out = [];
        var add = function(path, flags) {
            var read = fileReader(path);
            var size = FS().stat(path).size;
            var dir = parseGob(read, size);
            // The whole GOB too, for a mod that carries a copy of one.
            if (dir)
                out.push(Object.assign({ read: read, entries: dir.entries.concat([{ offset: 0, size: size, name: '' }]) },
                                       flags));
        };
        for (var rel of RETAIL_FILES[ext] || []) {
            var path = findPath(root, rel);
            if (path && isFile(path))
                add(path, { always: /^resource\//.test(rel) });
        }
        for (var path of spareRetail(root, ext))
            add(path, { always: false, spare: true });
        return out;
    }

    function spareRetail(root, ext) {
        var fs = FS();
        var resource = findPath(root, 'resource');
        var names = (RETAIL_FILES[ext] || []).filter(function(rel) { return /^resource\//.test(rel); })
                                             .map(function(rel) { return rel.substring(9).toLowerCase(); });
        var out = [];
        if (!resource)
            return out;
        for (var sub of fs.readdir(resource)) {
            var dir = resource + '/' + sub;
            if (sub === '.' || sub === '..' || isFile(dir))
                continue;
            var list;
            try { list = fs.readdir(dir); } catch (e) { continue; }
            for (var n of list) {
                if (names.indexOf(n.toLowerCase()) >= 0 && isFile(dir + '/' + n))
                    out.push(dir + '/' + n);
            }
        }
        return out;
    }

    function gameRoot() {
        return FS().cwd().replace(/\/+$/, '');
    }

    // Into the episode folder the engine already reads, whatever its case: a
    // second one spelled differently would hide one of them (see index.html).
    function install(root, name, ext, bytes) {
        var fs = FS();
        var dir = findPath(root, 'episode');
        if (!dir) {
            dir = root + '/episode';
            fs.mkdirTree(dir);
        }
        var old = findPath(root, 'episode/' + name + '.' + ext);
        if (old)
            fs.unlink(old);
        fs.writeFile(dir + '/' + name + '.' + ext, bytes, { canOwn: true });
    }

    // ---- Downloads kept in IndexedDB, one database per kind: "meta" holds
    // small records to list and evict by, "data" the files themselves, under
    // the same key. A file goes in as a Blob: Chromium refuses a record
    // whose own bytes are over 127 MiB (133169152), so a mod like SBX-JK
    // Continuum (305 MiB) wasn't kept, but it keeps a Blob's bytes beside the
    // record. Files saved before as bytes still read.

    function makeStore(dbName, maxBytes) {
        var dbPromise = null;

        function open() {
            if (dbPromise)
                return dbPromise;
            dbPromise = new Promise(function(resolve) {
                try {
                    var req = indexedDB.open(dbName, 1);
                    req.onupgradeneeded = function() {
                        req.result.createObjectStore('meta', { keyPath: 'key' });
                        req.result.createObjectStore('data');
                    };
                    req.onsuccess = function() { resolve(req.result); };
                    req.onerror = function() { resolve(null); };
                    req.onblocked = function() { resolve(null); };
                } catch (e) {
                    resolve(null);
                }
            });
            return dbPromise;
        }

        function request(req) {
            return new Promise(function(resolve, reject) {
                req.onsuccess = function() { resolve(req.result); };
                req.onerror = function() { reject(req.error); };
            });
        }

        function done(tx) {
            return new Promise(function(resolve, reject) {
                tx.oncomplete = function() { resolve(); };
                tx.onerror = tx.onabort = function() { reject(tx.error); };
            });
        }

        function key(game, name, sha) {
            return game + '/' + name.toLowerCase() + '/' + sha;
        }

        // A file's bytes as saved: an ArrayBuffer from a Blob, or the bytes.
        function bytesOf(data) {
            return typeof Blob !== 'undefined' && data instanceof Blob ? data.arrayBuffer() : data;
        }

        async function list(game) {
            var db = await open();
            if (!db)
                return [];
            var all = await request(db.transaction('meta').objectStore('meta').getAll());
            return all.filter(function(m) { return !game || m.game === game; });
        }

        // The record under k and its data, marked as just used.
        async function take(k) {
            var db = await open();
            if (!db)
                return null;
            var tx = db.transaction(['meta', 'data'], 'readwrite');
            var meta = await request(tx.objectStore('meta').get(k));
            var data = meta ? await request(tx.objectStore('data').get(k)) : null;
            if (meta && data) {
                meta.used = Date.now();
                tx.objectStore('meta').put(meta);
            }
            await done(tx);
            return meta && data ? { meta: meta, data: await bytesOf(data) } : null;
        }

        function get(game, name, sha) {
            return take(key(game, name, sha));
        }

        // By content alone, whatever name it was saved under.
        async function find(game, sha) {
            var metas = await list(game);
            for (var m of metas) {
                if (m.sha256 === sha)
                    return take(m.key);
            }
            return null;
        }

        async function read(k) {
            var db = await open();
            if (!db)
                return null;
            var data = await request(db.transaction('data').objectStore('data').get(k));
            return data ? bytesOf(data) : data;
        }

        // extra: more fields for the record, such as own (runJob).
        async function put(game, name, ext, sha, bytes, extra) {
            var db = await open();
            if (!db)
                return;
            var k = key(game, name, sha);
            var tx = db.transaction(['meta', 'data'], 'readwrite');
            tx.objectStore('data').put(new Blob([bytes]), k);
            var meta = { key: k, game: game, name: name, ext: ext, sha256: sha, size: bytes.length, used: Date.now() };
            for (var f in extra || {})
                meta[f] = extra[f];
            tx.objectStore('meta').put(meta);
            await done(tx);

            // Keep the store under its cap, dropping the least recently used.
            var metas = await list(null);
            metas.sort(function(a, b) { return b.used - a.used; });
            var total = 0;
            var drop = [];
            for (var m of metas) {
                total += m.size;
                if (total > maxBytes && m.key !== k)
                    drop.push(m.key);
            }
            if (drop.length) {
                tx = db.transaction(['meta', 'data'], 'readwrite');
                for (var d of drop) {
                    tx.objectStore('meta').delete(d);
                    tx.objectStore('data').delete(d);
                }
                await done(tx);
            }
        }

        return { list: list, get: get, find: find, read: read, put: put };
    }

    var store = makeStore('jkdf2mp-maps', MAX_STORED_BYTES);

    // Writes the maps stored for this game into episode/, the most recently
    // used version of each, unless the picked folder has one.
    async function installStoredMaps(root, game) {
        var metas = await store.list(game);
        metas.sort(function(a, b) { return b.used - a.used; });
        var seen = {};
        var names = [];
        for (var m of metas) {
            var lower = m.name.toLowerCase();
            if (seen[lower])
                continue;
            seen[lower] = true;
            if (!validEpisodeName(m.name) || !validExt(m.ext) || findPath(root, 'episode/' + m.name + '.' + m.ext))
                continue;
            var data = await store.read(m.key);
            if (!data)
                continue;
            install(root, m.name, m.ext, new Uint8Array(data));
            names.push(m.name);
        }
        return names;
    }

    // ---- What the joiner sees: a small panel over the game while a map
    // comes in, or with the reason it couldn't.

    var ui = (function() {
        var el = null, title, name, bar, detail, button;
        var onCancel = null;

        function make() {
            if (el || typeof document === 'undefined' || !document.body)
                return !!el;
            var style = document.createElement('style');
            style.textContent =
                '#jk-maps{position:fixed;left:50%;bottom:24px;transform:translateX(-50%);z-index:20;' +
                'width:min(420px,calc(100vw - 32px));box-sizing:border-box;padding:12px 14px;' +
                'background:var(--teal-dark,#003333);color:var(--ink,#fff);border:2px solid var(--gold,#ffcc66);' +
                'font:14px/1.35 Arial,Helvetica,sans-serif;box-shadow:0 4px 16px rgba(0,0,0,.5)}' +
                '#jk-maps p{margin:0 0 6px}' +
                '#jk-maps .jk-maps-title{font:bold 15px Verdana,Geneva,sans-serif;color:var(--gold,#ffcc66)}' +
                '#jk-maps .jk-maps-name{font:bold 14px "Courier New",monospace;word-break:break-all}' +
                '#jk-maps progress{display:block;width:100%;height:12px;margin:2px 0 6px;accent-color:var(--gold,#ffcc66)}' +
                '#jk-maps .jk-maps-detail{color:var(--muted,#99cccc)}' +
                '#jk-maps.jk-maps-error .jk-maps-detail{color:var(--bad,#ffb199)}' +
                '#jk-maps button{font:bold 14px Verdana,Geneva,sans-serif;padding:3px 14px;cursor:pointer;' +
                'border:2px outset #ffe7a8;background:linear-gradient(#ffe7a8,var(--gold,#ffcc66) 55%,#e8a93a);color:#1a1a00}' +
                '#jk-maps button:active{border-style:inset}' +
                '#jk-maps button:focus-visible{outline:2px solid var(--gold,#ffcc66);outline-offset:2px}';
            document.head.appendChild(style);
            el = document.createElement('div');
            el.id = 'jk-maps';
            el.setAttribute('role', 'status');
            el.setAttribute('aria-live', 'polite');
            el.hidden = true;
            title = document.createElement('p');
            title.className = 'jk-maps-title';
            name = document.createElement('p');
            name.className = 'jk-maps-name';
            bar = document.createElement('progress');
            bar.max = 1;
            detail = document.createElement('p');
            detail.className = 'jk-maps-detail';
            button = document.createElement('button');
            button.type = 'button';
            button.addEventListener('click', function() {
                if (onCancel)
                    onCancel();
                else
                    hide();
            });
            el.append(title, name, bar, detail, button);
            document.body.appendChild(el);
            return true;
        }

        function mb(n) {
            return (n / 1048576).toFixed(n < 10485760 ? 1 : 0);
        }

        function what(kind) {
            return kind === KIND_MOD ? 'mod' : 'map';
        }

        function show(mapName, text, cancel, kind) {
            if (!make())
                return;
            el.className = '';
            el.hidden = false;
            title.textContent = "Getting the host's " + what(kind);
            name.textContent = mapName;
            bar.hidden = false;
            bar.removeAttribute('value');
            detail.textContent = text;
            button.textContent = 'Cancel';
            onCancel = cancel;
        }

        // rate: bytes a second, shown once there is one.
        function progress(done, total, via, rate) {
            if (!el || el.hidden)
                return;
            bar.value = total ? done / total : 0;
            detail.textContent = mb(done) + ' of ' + mb(total) + ' MB, ' +
                (rate ? (rate / 1048576).toFixed(1) + ' MB/s, ' : '') +
                (via === 'relay' ? 'through the lobby' : 'straight from the host');
        }

        function error(mapName, text, kind) {
            if (!make())
                return;
            el.className = 'jk-maps-error';
            el.hidden = false;
            title.textContent = "Couldn't get the " + what(kind);
            name.textContent = mapName;
            bar.hidden = true;
            detail.textContent = text;
            button.textContent = 'OK';
            onCancel = null;
            button.focus();
        }

        // Something the player should read that isn't a failure, such as the
        // game restarting with the host's mod.
        function notice(heading, itemName, text) {
            if (!make())
                return;
            el.className = '';
            el.hidden = false;
            title.textContent = heading;
            name.textContent = itemName;
            bar.hidden = true;
            detail.textContent = text;
            button.textContent = 'OK';
            onCancel = null;
        }

        function hide() {
            if (el)
                el.hidden = true;
            onCancel = null;
        }

        return { show: show, progress: progress, error: error, notice: notice, hide: hide };
    })();

    var MESSAGES = {
        'none': "The host's map isn't a single map file on their side, so it can't be sent.",
        'too-big': 'This map is over 1 GB, more than the browser can take in.',
        'memory': "This tab doesn't have the memory for it. Close other tabs, reload this page and try again.",
        'not-gob': "The host's map file isn't a GOB the game can read.",
        'retail-only': "The host's map is made only of original game files, so it isn't sent.",
        'too-many-files': 'This map has too many files to send.',
        'no-answer': "The host didn't answer. They may need to reload the page.",
        'stalled': 'The download stopped. The connection to the host may have dropped.',
        'host-left': 'The host left the game.',
        'lobby-lost': 'The connection to the lobby dropped.',
        'corrupt': "The map didn't arrive intact, so it wasn't installed.",
        'mismatch': "The map didn't arrive intact, so it wasn't installed.",
        'install': "The map couldn't be installed in this tab.",
        'bad-name': "The host's map name isn't one that can be saved.",
        'retail': "The host is running an original level, which isn't sent.",
        'other-map': 'The host is running a different map now.',
        'error': "The host couldn't get its map ready to send.",
        'cancelled': null
    };

    function messageFor(reason, ctx) {
        if (ctx.messageFor) {
            var own = ctx.messageFor(reason);
            if (own !== undefined)
                return own;
        }
        if (reason === 'retail-missing')
            return 'This game runs an original ' + gameOfEpisode(ctx.name) +
                   " level that isn't in the game folder you picked.";
        if (reason === 'other-game')
            return 'This game is for ' + (ctx.ext === 'goo' ? 'Jedi Knight' : 'Mysteries of the Sith') +
                   ', and the game folder you picked is ' + (ctx.ext === 'goo' ? 'Mysteries of the Sith.' : 'Jedi Knight.');
        if (reason === 'missing-retail')
            return 'This map uses original ' + (ctx.ext === 'goo' ? 'Mysteries of the Sith' : 'Jedi Knight') +
                   " files that weren't found in the game folder you picked.";
        if (reason === 'retail-changed')
            return 'Your game folder has other versions of original ' +
                   (ctx.ext === 'goo' ? 'Mysteries of the Sith files this map uses (the MotS Remastered pack ' +
                                        'changes some). ' : 'Jedi Knight files this map uses. ') +
                   'Ask the host to reload the page, then join again.';
        if (reason in MESSAGES)
            return MESSAGES[reason];
        return "The host's map couldn't be sent (" + reason + ').';
    }

    // ---- Frames.

    function frame(type, idTo, bodyLen, kind) {
        var buf = new ArrayBuffer(HEADER + bodyLen);
        var b = new Uint8Array(buf);
        var v = new DataView(buf);
        v.setUint32(0, idTo === 1 ? 0 : 1, true); // idFrom: the relay stamps a client's own
        v.setUint32(4, idTo, true);
        b.set(MAGIC, 8);
        b[12] = type;
        b[13] = VERSION;
        b[14] = kind || KIND_MAP;
        return { buf: buf, bytes: b, view: v };
    }

    function isFrame(data) {
        if (!(data instanceof ArrayBuffer) || data.byteLength < HEADER)
            return false;
        var b = new Uint8Array(data, 8, 4);
        return b[0] === MAGIC[0] && b[1] === MAGIC[1] && b[2] === MAGIC[2] && b[3] === MAGIC[3];
    }

    // The connection the frames go over: the lobby's (stdComm_WSRelay.c), or
    // while that one is picked the Imperial Network Protocol's
    // (stdComm_Imperial.c), which has the same mapSend and mapDirect.
    function link() {
        var imp = Module.jkImperial;
        return imp && imp.active ? imp : Module.jkNet;
    }

    function send(id, f, via) {
        var net = link();
        return net && net.mapSend ? net.mapSend(id, f.buf, via) : null;
    }

    function idOf(sha) {
        var out = new Uint8Array(8);
        for (var i = 0; i < 8; i++)
            out[i] = parseInt(sha.substr(i * 2, 2), 16);
        return out;
    }

    function sameId(a, b) {
        for (var i = 0; i < 8; i++)
            if (a[i] !== b[i]) return false;
        return true;
    }

    // ---- Host: gets each thing it offers (its map, its mod) ready once when
    // it starts hosting, then answers whoever asks.

    var offers = {}; // kind -> { info, stream, id, waiting, blocks }

    // prepare() resolves to { info, stream }: the INFO joiners get, and with
    // status 'ok' the stream their chunks and blocks are cut from (bytes, or
    // pieces()).
    function offer(kind, prepare) {
        var h = offers[kind] = { info: null, stream: null, id: null, waiting: [], blocks: new Map() };
        (async function() {
            var r;
            try {
                r = await prepare();
            } catch (e) {
                console.warn('OpenJKDF2: could not get the ' + (kind === KIND_MOD ? 'mod' : 'map') +
                             ' ready for joiners: ' + e);
                r = { info: { status: 'error' } };
            }
            if (offers[kind] !== h)
                return;
            h.info = r.info;
            if (r.info.status === 'ok' && r.stream) {
                h.stream = r.stream.views ? r.stream : pieces([r.stream]);
                h.id = idOf(r.info.sha256);
                h.info.z = BLOCK_BYTES;
            }
            for (var w of h.waiting)
                sendInfo(kind, w.id, w.via);
            h.waiting = [];
        })();
    }

    function withdraw(kind) {
        delete offers[kind];
    }

    function host(name, ext) {
        offer(KIND_MAP, async function() {
            var info = { status: 'none', name: name, ext: ext };
            var stream = null;
            try {
                var root = gameRoot();
                var path = validEpisodeName(name) && validExt(ext) ? findPath(root, 'episode/' + name + '.' + ext) : null;
                if (isRetailEpisode(name)) {
                    info.status = 'retail';
                } else if (path && isFile(path)) {
                    if (FS().stat(path).size > MAX_MAP_BYTES) {
                        info.status = 'too-big';
                    } else {
                        var gob = fileBytes(path);
                        var plan = await planShare(gob, retailSources(root, ext));
                        if (plan.ok) {
                            info = { status: 'ok', name: name, ext: ext, size: gob.length,
                                     sha256: await sha256(gob), streamSize: plan.stream.length,
                                     refsBytes: plan.refsBytes };
                            // For a joiner who has other versions of the
                            // original files left out (ownVersions).
                            if (plan.refs && plan.stream.length <= MAX_STREAM_HASH_BYTES)
                                info.streamSha256 = await sha256(plan.stream.bytes());
                            stream = plan.stream;
                            console.log('OpenJKDF2: ready to send map ' + name + '.' + ext + ' to joiners (' +
                                        plan.refs + ' original files left out, ' + plan.stream.length + ' of ' +
                                        gob.length + ' bytes to send)');
                        } else {
                            info.status = plan.reason;
                        }
                    }
                }
            } catch (e) {
                console.warn('OpenJKDF2: could not prepare map ' + name + ' for joiners: ' + e);
                info = { status: 'error', name: name, ext: ext };
            }
            return { info: info, stream: stream };
        });
    }

    function unhost() {
        withdraw(KIND_MAP);
    }

    function sendInfo(kind, id, via) {
        var json = new TextEncoder().encode(JSON.stringify(offers[kind].info));
        var f = frame(T_INFO, id, json.length, kind);
        f.bytes.set(json, HEADER);
        send(id, f, via);
    }

    // Chunks and blocks are asked for by the file's id, whichever kind it is.
    function offerFor(want) {
        for (var key in offers) {
            if (offers[key].stream && sameId(want, offers[key].id))
                return Number(key);
        }
        return null;
    }

    // ---- Squeezing blocks with deflate: in workers, since a 1 MB block takes
    // about 30 ms and a host in a match would miss frames, or here where a
    // worker can't be made.

    var squeezer = (function() {
        var SOURCE = 'onmessage=function(e){var d=e.data;' +
            'new Response(new Blob([d.bytes]).stream().pipeThrough(new CompressionStream("deflate-raw"))).arrayBuffer()' +
            '.then(function(z){postMessage({n:d.n,z:z},[z]);},function(){postMessage({n:d.n,z:null});});};';
        var workers = null;
        var jobs = {};
        var seq = 0;

        function can() {
            return typeof CompressionStream !== 'undefined';
        }

        function here(bytes) {
            return new Response(new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate-raw')))
                .arrayBuffer().then(function(z) { return new Uint8Array(z); }, function() { return null; });
        }

        // A worker that fails gives up on them all: what it had resolves to
        // null (sent as is), and the rest is squeezed here.
        function broke() {
            for (var w of workers)
                w.terminate();
            workers = [];
            for (var n in jobs)
                jobs[n].resolve(null);
            jobs = {};
        }

        function start() {
            if (workers)
                return;
            workers = [];
            try {
                if (typeof Worker === 'undefined' || typeof URL === 'undefined' || !URL.createObjectURL)
                    return;
                var url = URL.createObjectURL(new Blob([SOURCE], { type: 'text/javascript' }));
                var cores = (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 2;
                for (var i = 0; i < Math.max(1, Math.min(4, cores - 1)); i++) {
                    var w = new Worker(url);
                    w.busy = 0;
                    w.onmessage = function(ev) {
                        var job = jobs[ev.data.n];
                        if (!job)
                            return;
                        delete jobs[ev.data.n];
                        job.worker.busy--;
                        job.resolve(ev.data.z ? new Uint8Array(ev.data.z) : null);
                    };
                    w.onerror = broke;
                    workers.push(w);
                }
            } catch (e) {
                workers = [];
            }
        }

        // bytes is the worker's to keep: a copy of its own.
        function squeeze(bytes) {
            start();
            if (!workers.length)
                return here(bytes);
            return new Promise(function(resolve) {
                var w = workers.reduce(function(a, b) { return b.busy < a.busy ? b : a; });
                var n = ++seq;
                jobs[n] = { resolve: resolve, worker: w };
                w.busy++;
                w.postMessage({ n: n, bytes: bytes }, [bytes.buffer]);
            });
        }

        return { can: can, squeeze: squeeze };
    })();

    // Block b of what o offers, as sent to a joiner that can unsqueeze what
    // accept says: { codec, bytes }. Squeezed blocks are kept for the next
    // joiner, and so is knowing that one doesn't get smaller.
    function block(o, b, accept) {
        var at = b * BLOCK_BYTES;
        var n = Math.min(BLOCK_BYTES, o.stream.length - at);
        if (!(accept & CODEC_DEFLATE) || !squeezer.can())
            return Promise.resolve({ codec: CODEC_STORED, bytes: o.stream.read(at, n) });
        var p = o.blocks.get(b);
        if (!p) {
            p = squeezer.squeeze(o.stream.read(at, n).slice()).then(function(z) {
                return z && z.length < n ? z : null;
            });
            o.blocks.set(b, p);
        }
        return p.then(function(z) {
            return z ? { codec: CODEC_DEFLATE, bytes: z } : { codec: CODEC_STORED, bytes: o.stream.read(at, n) };
        });
    }

    function sendBlock(k, o, b, to, via, part, accept) {
        block(o, b, accept).then(function(r) {
            if (offers[k] !== o)
                return;
            var z = r.bytes;
            for (var at = 0; at < z.length; at += part) {
                var n = Math.min(part, z.length - at);
                var f = frame(T_BLOCK, to, BLOCK_HEADER + n, k);
                f.bytes.set(o.id, HEADER);
                f.view.setUint32(HEADER + 8, b, true);
                f.view.setUint32(HEADER + 12, z.length, true);
                f.view.setUint32(HEADER + 16, at, true);
                f.bytes[HEADER + 20] = r.codec;
                f.bytes.set(z.subarray(at, at + n), HEADER + BLOCK_HEADER);
                if (!send(to, f, via))
                    return;
            }
        }, function(e) {
            console.warn('OpenJKDF2: could not send block ' + b + ': ' + e);
        });
    }

    function onHostFrame(from, type, data, via, kind) {
        if (from < 2)
            return;
        if (type === T_INFO_REQ) {
            var h = offers[kind];
            if (!h)
                return;
            if (h.info)
                sendInfo(kind, from, via);
            else if (h.waiting.length < 64)
                h.waiting.push({ id: from, via: via });
        } else if (type === T_CHUNK_REQ && data.byteLength >= HEADER + 20) {
            var k = offerFor(new Uint8Array(data, HEADER, 8));
            if (k === null)
                return;
            var o = offers[k];
            var v = new DataView(data);
            var size = v.getUint32(HEADER + 8, true);
            var first = v.getUint32(HEADER + 12, true);
            var count = Math.min(v.getUint32(HEADER + 16, true), MAX_CHUNKS_PER_REQ);
            if (size !== CHUNK_DIRECT && size !== CHUNK_RELAY)
                return;
            var stream = o.stream;
            for (var i = first; i < first + count && i * size < stream.length; i++) {
                var part = stream.read(i * size, Math.min(size, stream.length - i * size));
                var f = frame(T_CHUNK, from, 12 + part.length, k);
                f.bytes.set(o.id, HEADER);
                f.view.setUint32(HEADER + 8, i, true);
                f.bytes.set(part, HEADER + 12);
                if (!send(from, f, via))
                    break;
            }
        } else if (type === T_BLOCK_REQ && data.byteLength >= HEADER + 24) {
            var k = offerFor(new Uint8Array(data, HEADER, 8));
            if (k === null)
                return;
            var o = offers[k];
            var v = new DataView(data);
            var first = v.getUint32(HEADER + 8, true);
            var count = Math.min(v.getUint32(HEADER + 12, true), MAX_BLOCKS_PER_REQ);
            var part = v.getUint32(HEADER + 16, true);
            var accept = new Uint8Array(data)[HEADER + 20];
            if (part !== PART_DIRECT && part !== PART_RELAY)
                return;
            // The lobby closes a socket that sends it more than 64 KiB at once.
            if (via === 'relay')
                part = PART_RELAY;
            var nb = Math.ceil(o.stream.length / BLOCK_BYTES);
            var b = first;
            for (; b < first + count && b < nb; b++)
                sendBlock(k, o, b, from, via, part, accept);
            // The next ones, squeezed while these go.
            for (var ahead = b; ahead < b + BLOCKS_AHEAD && ahead < nb && (accept & CODEC_DEFLATE); ahead++)
                block(o, ahead, accept);
        }
    }

    // ---- Joiner: one job per kind at a time, a map or a mod.

    var jobs = {};   // kind -> job
    var states = {}; // kind -> STATE_*

    // fields: label (shown on the panel) and whatever run needs. run(j) does
    // the work and resolves to the state to end in (STATE_READY if it returns
    // nothing), or throws fail(reason). j.messageFor(reason) can word a
    // failure its own way.
    function startJob(kind, fields, run) {
        var j = { kind: kind, done: false, onInfo: null, onChunk: null, timers: [] };
        for (var f in fields)
            j[f] = fields[f];
        jobs[kind] = j;
        states[kind] = STATE_BUSY;
        run(j).then(function(state) {
            finish(j, null, state);
        }, function(e) {
            finish(j, e && e.reason ? e.reason : 'error');
            if (!(e && e.reason))
                console.warn('OpenJKDF2: ' + (kind === KIND_MOD ? 'mod' : 'map') + ' download failed: ' + e);
        });
        return j;
    }

    function fetchMap(name, ext) {
        startJob(KIND_MAP, { name: name, ext: ext, label: name + '.' + ext }, runJob);
    }

    function fail(reason) {
        var e = new Error(reason);
        e.reason = reason;
        return e;
    }

    function finish(j, reason, state) {
        if (j.done)
            return;
        j.done = true;
        j.timers.forEach(clearTimeout);
        if (j.cancel)
            j.cancel(fail(reason || 'done'));
        if (jobs[j.kind] !== j)
            return;
        if (!reason) {
            // A restart leaves its notice up, to be read after the restart.
            if (state !== STATE_RESTART)
                ui.hide();
            states[j.kind] = state || STATE_READY;
            return;
        }
        var text = messageFor(reason, j);
        if (text)
            ui.error(j.label, text, j.kind);
        else
            ui.hide();
        states[j.kind] = STATE_FAILED;
        console.log('OpenJKDF2: ' + (j.kind === KIND_MOD ? 'mod ' : 'map ') + j.label + ' not installed: ' + reason);
    }

    function abort(reason, kind) {
        var j = jobs[kind || KIND_MAP];
        if (j && !j.done)
            finish(j, reason || 'cancelled');
    }

    function state(kind) {
        return kind in states ? states[kind] : STATE_READY;
    }

    function sleep(j, ms) {
        return new Promise(function(resolve, reject) {
            if (j.done)
                return reject(fail('cancelled'));
            j.cancel = reject;
            j.timers.push(setTimeout(resolve, ms));
        });
    }

    function check(j) {
        if (j.done)
            throw fail('cancelled');
    }

    function directUp() {
        var net = link();
        return !!(net && net.mapDirect && net.mapDirect(1));
    }

    // No direct link is coming when WebRTC is off in this browser.
    function directPossible() {
        var net = link();
        return !(net && net.rtcEnabled && !net.rtcEnabled());
    }

    // Gives a direct link that is still starting up a moment to come up.
    async function waitForDirect(j) {
        for (var waited = 0; waited < DIRECT_WAIT_MS && directPossible() && !directUp(); waited += 100)
            await sleep(j, 100);
    }

    function askInfo(j) {
        return new Promise(function(resolve, reject) {
            var started = Date.now();
            j.cancel = reject;
            j.onInfo = function(info) {
                j.onInfo = null;
                resolve(info);
            };
            function ask() {
                if (j.done || !j.onInfo)
                    return;
                if (Date.now() - started > INFO_TIMEOUT_MS)
                    return reject(fail('no-answer'));
                send(1, frame(T_INFO_REQ, 1, 0, j.kind), directUp() ? 'direct' : 'relay');
                j.timers.push(setTimeout(ask, INFO_RESEND_MS));
            }
            ask();
        });
    }

    // Checks what a host says about the file it offers before anything is
    // asked for: sizes and the hash have to make sense.
    function checkInfo(info, maxBytes) {
        if (!(info.size > 0 && info.size <= maxBytes))
            throw fail('too-big');
        if (!(info.refsBytes >= 4 && info.streamSize >= info.refsBytes &&
              info.streamSize <= info.size + 4 + MAX_REFS * REF_BYTES) || !/^[0-9a-f]{64}$/.test(info.sha256))
            throw fail('corrupt');
    }

    async function runJob(j) {
        if (!validEpisodeName(j.name) || !validExt(j.ext))
            throw fail('bad-name');
        var root = gameRoot();
        var local = findPath(root, 'episode/' + j.name + '.' + j.ext);
        if (isRetailEpisode(j.name)) {
            if (local)
                return;
            throw fail('retail-missing');
        }

        var checking = setTimeout(function() {
            if (!j.done)
                ui.show(j.label, "Checking the host's map...", function() { abort('cancelled'); }, KIND_MAP);
        }, CHECKING_UI_MS);
        j.timers.push(checking);
        var info;
        try {
            info = await askInfo(j);
        } catch (e) {
            // A host on an older build of the page never answers. Players who
            // have the map could join it before, so let them.
            if (e.reason === 'no-answer' && local && isFile(local)) {
                console.log("OpenJKDF2: the host didn't answer about its map, using this player's own " + j.label);
                return;
            }
            throw e;
        }
        if (info.status !== 'ok')
            throw fail(info.status);
        if (String(info.name).toLowerCase() !== j.name.toLowerCase())
            throw fail('other-map');
        if (info.ext !== j.ext)
            throw fail('other-game');
        checkInfo(info, MAX_MAP_BYTES);

        if (local && isFile(local) && FS().stat(local).size === info.size &&
            await sha256(fileBytes(local)) === info.sha256)
            return;
        check(j);

        var game = j.ext === 'goo' ? 'mots' : 'jk1';
        var stored = null;
        try { stored = await store.get(game, j.name, info.sha256); } catch (e) {}
        check(j);
        // A map saved with the player's own versions of original files is
        // checked against what was saved (own), not the host's file.
        if (stored && await sha256(new Uint8Array(stored.data)) === (stored.meta.own || info.sha256)) {
            check(j);
            install(root, j.name, j.ext, new Uint8Array(stored.data));
            console.log('OpenJKDF2: installed map ' + j.label + ' saved from an earlier game');
            return;
        }

        clearTimeout(checking);
        ui.show(j.label, 'Connecting to the host...', function() { abort('cancelled'); }, KIND_MAP);
        await waitForDirect(j);
        var bytes = await receive(j, info, retailSources(root, j.ext), true);
        try {
            install(root, j.name, j.ext, bytes);
        } catch (e) {
            throw fail('install');
        }
        console.log('OpenJKDF2: installed map ' + j.label + ' from the host' +
                    (j.own ? ", with this player's own versions of " + j.own + ' original files it bundles' : ''));
        (async function() {
            var extra = j.own ? { own: await sha256(bytes) } : null;
            await store.put(game, j.name, j.ext, info.sha256, bytes, extra);
        })().catch(function(e) {
            console.warn('OpenJKDF2: could not save map ' + j.label + ' for next time: ' + e);
        });
    }

    // Gets the host's file, in blocks from a host that sends them (it says z)
    // or else in chunks, put back together and checked. With own (a map),
    // original files the joiner only has other versions of are left out of
    // it, and j.own says how many.
    async function receive(j, info, retailSources, own) {
        var result;
        if (info.z === BLOCK_BYTES) {
            var rb = rebuilder(info, retailSources, own);
            bulk(true);
            try {
                await downloadBlocks(j, info, rb.put);
            } finally {
                bulk(false);
            }
            result = await rb.finish();
        } else {
            var stream = await download(j, info, directUp() ? CHUNK_DIRECT : CHUNK_RELAY);
            result = await rebuild(info, stream, retailSources, own);
        }
        if (!result.ok)
            throw fail(result.reason);
        check(j);
        j.own = result.own || 0;
        return result.bytes;
    }

    // The ways to the host a block can be asked for on now: the map channel
    // and the extra links for transfers (net.mapLanes), or the relay.
    function lanes() {
        var net = link();
        if (net && net.mapLanes) {
            var l = net.mapLanes(1);
            if (l.length)
                return l;
        } else if (directUp()) {
            return ['direct'];
        }
        return ['relay'];
    }

    // Asks the network code for the extra links to the host while a transfer
    // runs, and lets them go after it.
    function bulk(on) {
        var net = link();
        if (net && net.mapBulk)
            net.mapBulk(1, on);
    }

    // Unsqueezes a block, which has to come out exactly size bytes.
    async function inflate(z, size) {
        var ds = new DecompressionStream('deflate-raw');
        var w = ds.writable.getWriter();
        w.write(z).catch(function() {});
        w.close().catch(function() {});
        var r = ds.readable.getReader();
        var out = new Uint8Array(size);
        var n = 0;
        for (;;) {
            var x;
            try {
                x = await r.read();
            } catch (e) {
                throw fail('corrupt');
            }
            if (x.done)
                break;
            if (n + x.value.length > size) {
                r.cancel().catch(function() {});
                throw fail('corrupt');
            }
            out.set(x.value, n);
            n += x.value.length;
        }
        if (n !== size)
            throw fail('corrupt');
        return out;
    }

    // Bytes a second over the last few seconds.
    function rateMeter() {
        var marks = [];
        return function(bytes) {
            var now = Date.now();
            marks.push({ t: now, bytes: bytes });
            while (marks.length > 2 && now - marks[1].t > 3000)
                marks.shift();
            var span = now - marks[0].t;
            return span >= 500 ? (bytes - marks[0].bytes) * 1000 / span : 0;
        };
    }

    // Parts of a block come in whole units of 16 KiB, which both part sizes
    // are, so a block asked for again on another way fills in what's missing.
    var UNIT = 16 * 1024;

    // Pulls the stream in blocks, a few at a time on each way to the host,
    // and hands each to put(offset, bytes) once it's in and unsqueezed. A
    // block that stops coming (its link dropped) is asked for again.
    function downloadBlocks(j, info, put) {
        return new Promise(function(resolve, reject) {
            var total = info.streamSize;
            var nb = Math.ceil(total / BLOCK_BYTES);
            var id = idOf(info.sha256);
            var accept = typeof DecompressionStream !== 'undefined' ? CODEC_DEFLATE : CODEC_STORED;
            var next = 0;
            var redo = [];
            var have = new Uint8Array(nb);
            var asked = new Map(); // block -> { lane, at }
            var parts = new Map(); // block -> { zlen, codec, buf, units, left }
            var done = 0, bytes = 0;
            var lastProgress = Date.now();
            var meter = rateMeter();
            var via = 'direct';

            j.cancel = reject;

            function rawSize(b) {
                return Math.min(BLOCK_BYTES, total - b * BLOCK_BYTES);
            }

            function ask(b, lane) {
                var f = frame(T_BLOCK_REQ, 1, 24, j.kind);
                f.bytes.set(id, HEADER);
                f.view.setUint32(HEADER + 8, b, true);
                f.view.setUint32(HEADER + 12, 1, true);
                f.view.setUint32(HEADER + 16, lane === 'relay' ? PART_RELAY : PART_DIRECT, true);
                f.bytes[HEADER + 20] = accept;
                var went = send(1, f, lane);
                if (!went)
                    return false;
                asked.set(b, { lane: went, at: Date.now() });
                via = went;
                return true;
            }

            // One block at a time to the way with the fewest out, so a small
            // file still spreads over them all.
            function pump() {
                var busy = {};
                var open = lanes();
                for (var lane of open)
                    busy[lane] = 0;
                asked.forEach(function(a) { busy[a.lane] = (busy[a.lane] || 0) + 1; });
                for (;;) {
                    var best = null;
                    for (var lane of open) {
                        if (busy[lane] < (lane === 'relay' ? BLOCKS_RELAY : BLOCKS_DIRECT) &&
                            (best === null || busy[lane] < busy[best]))
                            best = lane;
                    }
                    if (best === null)
                        return;
                    var again = redo.length > 0;
                    var b = again ? redo.shift() : next < nb ? next++ : -1;
                    if (b < 0)
                        return;
                    if (have[b] || asked.has(b))
                        continue;
                    if (!ask(b, best)) {
                        if (again)
                            redo.unshift(b);
                        else
                            next--;
                        return;
                    }
                    busy[best]++;
                }
            }

            function finished(b, out) {
                if (j.done)
                    return;
                put(b * BLOCK_BYTES, out);
                done++;
                bytes += out.length;
                ui.progress(bytes, total, via, meter(bytes));
                if (done === nb) {
                    j.onBlock = null;
                    resolve();
                }
            }

            j.onBlock = function(data) {
                if (data.byteLength < HEADER + BLOCK_HEADER || !sameId(new Uint8Array(data, HEADER, 8), id))
                    return;
                var v = new DataView(data);
                var b = v.getUint32(HEADER + 8, true);
                var zlen = v.getUint32(HEADER + 12, true);
                var at = v.getUint32(HEADER + 16, true);
                var codec = new Uint8Array(data)[HEADER + 20];
                var len = data.byteLength - HEADER - BLOCK_HEADER;
                if (b >= next || have[b])
                    return;
                var raw = rawSize(b);
                if (codec === CODEC_STORED ? zlen !== raw : codec !== CODEC_DEFLATE || !accept || !zlen || zlen >= raw)
                    return;
                if (at % UNIT || !len || at + len > zlen || (len % UNIT && at + len !== zlen))
                    return;
                var p = parts.get(b);
                if (!p || p.zlen !== zlen || p.codec !== codec) {
                    var units = Math.ceil(zlen / UNIT);
                    p = { zlen: zlen, codec: codec, buf: new Uint8Array(zlen), units: new Uint8Array(units), left: units };
                    parts.set(b, p);
                }
                p.buf.set(new Uint8Array(data, HEADER + BLOCK_HEADER, len), at);
                for (var u = at / UNIT; u < Math.ceil((at + len) / UNIT); u++) {
                    if (!p.units[u]) {
                        p.units[u] = 1;
                        p.left--;
                    }
                }
                lastProgress = Date.now();
                var a = asked.get(b);
                if (a)
                    a.at = lastProgress;
                if (p.left)
                    return;
                have[b] = 1;
                asked.delete(b);
                parts.delete(b);
                (codec === CODEC_DEFLATE ? inflate(p.buf, raw) : Promise.resolve(p.buf)).then(function(out) {
                    finished(b, out);
                }).catch(function(e) {
                    reject(e && e.reason ? e : fail('corrupt'));
                });
                pump();
            };

            // Blocks on a way that's gone, or that haven't moved for a while,
            // are asked for again.
            function tick() {
                if (j.done || done === nb)
                    return;
                var now = Date.now();
                if (now - lastProgress > STALL_MS)
                    return reject(fail('stalled'));
                var open = lanes();
                asked.forEach(function(a, b) {
                    if (open.indexOf(a.lane) < 0 || now - a.at > CHUNK_RESEND_MS) {
                        asked.delete(b);
                        redo.push(b);
                    }
                });
                redo.sort(function(x, y) { return x - y; });
                pump();
                j.timers.push(setTimeout(tick, 250));
            }

            ui.progress(0, total, via, 0);
            pump();
            tick();
        });
    }

    // Pulls the stream in numbered chunks, a window at a time, asking again
    // for chunks that don't come (lost when the link changed).
    function download(j, info, chunkSize) {
        return new Promise(function(resolve, reject) {
            var total = info.streamSize;
            var n = Math.ceil(total / chunkSize);
            var id = idOf(info.sha256);
            var buf = new Uint8Array(total);
            var got = new Uint8Array(n);
            var received = 0, bytes = 0, next = 0;
            var asked = new Map(); // chunk -> time asked
            var window = Math.max(4, Math.floor(WINDOW_BYTES / chunkSize));
            // Asking for half a window at a time keeps requests to a few per
            // window: through the relay each one counts against the lobby's
            // daily limit.
            var batch = Math.max(1, Math.floor(window / 2));
            var lastProgress = Date.now();
            var via = directUp() ? 'direct' : 'relay';

            j.cancel = reject;

            function ask(first, count) {
                var f = frame(T_CHUNK_REQ, 1, 20, j.kind);
                f.bytes.set(id, HEADER);
                f.view.setUint32(HEADER + 8, chunkSize, true);
                f.view.setUint32(HEADER + 12, first, true);
                f.view.setUint32(HEADER + 16, count, true);
                via = send(1, f, directUp() ? 'direct' : 'relay') || via;
                var now = Date.now();
                for (var i = first; i < first + count; i++)
                    asked.set(i, now);
            }

            function pump() {
                while (next < n) {
                    var count = Math.min(n - next, window - asked.size, MAX_CHUNKS_PER_REQ);
                    if (count < Math.min(batch, n - next))
                        break;
                    ask(next, count);
                    next += count;
                }
            }

            j.onChunk = function(data) {
                if (data.byteLength < HEADER + 12 || !sameId(new Uint8Array(data, HEADER, 8), id))
                    return;
                var i = new DataView(data).getUint32(HEADER + 8, true);
                var len = Math.min(chunkSize, total - i * chunkSize);
                if (i >= n || got[i] || data.byteLength !== HEADER + 12 + len)
                    return;
                buf.set(new Uint8Array(data, HEADER + 12, len), i * chunkSize);
                got[i] = 1;
                asked.delete(i);
                received++;
                bytes += len;
                lastProgress = Date.now();
                ui.progress(bytes, total, via);
                if (received === n) {
                    j.onChunk = null;
                    resolve(buf);
                } else {
                    pump();
                }
            };

            function tick() {
                if (j.done || received === n)
                    return;
                var now = Date.now();
                if (now - lastProgress > STALL_MS)
                    return reject(fail('stalled'));
                var late = [];
                asked.forEach(function(t, i) { if (now - t > CHUNK_RESEND_MS) late.push(i); });
                late.sort(function(a, b) { return a - b; });
                for (var k = 0; k < late.length; ) {
                    var first = late[k], count = 1;
                    while (k + count < late.length && late[k + count] === first + count && count < MAX_CHUNKS_PER_REQ)
                        count++;
                    ask(first, count);
                    k += count;
                }
                j.timers.push(setTimeout(tick, 500));
            }

            ui.progress(0, total, via);
            pump();
            tick();
        });
    }

    function onJoinerFrame(from, type, data, kind) {
        var j = jobs[kind];
        if (from !== 1 || !j || j.done)
            return;
        if (type === T_INFO && j.onInfo) {
            var info = null;
            try { info = JSON.parse(new TextDecoder().decode(new Uint8Array(data, HEADER))); } catch (e) {}
            if (info && typeof info.status === 'string')
                j.onInfo(info);
        } else if (type === T_CHUNK && j.onChunk) {
            j.onChunk(data);
        } else if (type === T_BLOCK && j.onBlock) {
            j.onBlock(data);
        }
    }

    // Frames from player `from`, over the map data channel ('direct') or the
    // lobby ('relay').
    function onFrame(from, data, via) {
        if (!isFrame(data))
            return;
        var b = new Uint8Array(data, 12, 3);
        if (b[1] !== VERSION || (b[2] !== KIND_MAP && b[2] !== KIND_MOD))
            return;
        if (b[0] === T_INFO_REQ || b[0] === T_CHUNK_REQ || b[0] === T_BLOCK_REQ)
            onHostFrame(from, b[0], data, via, b[2]);
        else
            onJoinerFrame(from, b[0], data, b[2]);
    }

    Module.jkMaps = {
        lib: lib,
        isFrame: isFrame,
        onFrame: onFrame,
        host: host,
        unhost: unhost,
        fetch: fetchMap,
        abort: function(reason) { abort(reason, KIND_MAP); },
        state: function() { return state(KIND_MAP); },
        // What mods.js builds on to send the host's mod the same way.
        core: {
            KIND_MOD: KIND_MOD, STATE_BUSY: STATE_BUSY, STATE_READY: STATE_READY,
            STATE_FAILED: STATE_FAILED, STATE_RESTART: STATE_RESTART,
            CHUNK_DIRECT: CHUNK_DIRECT, CHUNK_RELAY: CHUNK_RELAY, CHECKING_UI_MS: CHECKING_UI_MS,
            offer: offer, withdraw: withdraw, startJob: startJob, abort: abort, state: state,
            askInfo: askInfo, checkInfo: checkInfo, download: download, receive: receive,
            waitForDirect: waitForDirect, directUp: directUp, fail: fail, check: check, sleep: sleep, ui: ui,
            findPath: findPath, isFile: isFile, fileBytes: fileBytes, gameRoot: gameRoot,
            retailSources: retailSources, makeStore: makeStore
        }
    };

    // Maps saved from earlier games go into episode/ before the engine starts,
    // after the page has written the picked folder (its preRun runs first).
    if (typeof indexedDB !== 'undefined') {
        Module.preRun = Module.preRun || [];
        if (typeof Module.preRun === 'function')
            Module.preRun = [Module.preRun];
        Module.preRun.push(function() {
            var mots = (Module['arguments'] || []).indexOf('-motsCompat') >= 0;
            Module.addRunDependency('jk-maps');
            installStoredMaps(mots ? '/mots' : '/jk1', mots ? 'mots' : 'jk1').then(function(names) {
                if (names.length)
                    console.log('OpenJKDF2: added maps saved from earlier games: ' + names.join(', '));
            }, function(e) {
                console.warn('OpenJKDF2: could not load maps saved from earlier games: ' + e);
            }).finally(function() {
                Module.removeRunDependency('jk-maps');
            });
        });
    }
})();
