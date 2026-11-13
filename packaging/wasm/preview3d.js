// The Level Library's 3D view (src/Gui/jkGUILibrary.c, library.js): a
// level's map as Massassi's 3D preview reads it from the level (jkview, by
// Stephan Reiter, on 3dpreview.massassi.net), a copy of which the site keeps
// (src/levels.js in the jkdf2mp-bb-server repository):
//
//   /levels/p/<id>-<m>.map.json  map m: its surfaces (corners with their
//                                place, texture place and light), the
//                                sky's, a color for each texture, the
//                                starting places
//   /levels/p/<id>-<m>.mat.json  its textures as PNGs, with their names
//   /levels/p/<id>.tex.json      the textures and colormaps the level ships,
//                                and each of its levels' master colormap
//
// Drawn with WebGL 2 on a canvas the page puts over a frame the game's menu
// leaves for it, as it does the screenshot: a small one on the Overview that
// turns slowly, and a big one you can move around in.
//
// Textures: the preview was asked for the level without ownsgame=1, so the
// site keeps none of the game's own art. The textures the level ships are
// its PNGs, exact. The game's own it has only as blurred stand-ins: those
// are read from the player's own game files instead and turned into colors
// with the level's master colormap, as the game does (Engine/rdMaterial.c).
// One that's in neither stays a stand-in, and the view says how many.

(function() {
    var BASE = '/levels/p/';
    // Where the game's own textures and colormaps are, in the order the game
    // has them: resource/ first (always loaded), then the episodes.
    var GAME_GOBS = {
        jk1: ['resource/Res2.gob', 'resource/Res1hi.gob', 'episode/JK1MP.gob', 'episode/JK1CTF.gob', 'episode/JK1.gob'],
        mots: ['resource/Jkmres.goo', 'episode/JKM_MP.goo', 'episode/JKM_KFY.goo', 'episode/JKM_SABER.goo', 'episode/JKM.goo']
    };
    var REST_MS = 600;
    var TURN_S = 48;
    var FOV = 70 * Math.PI / 180;
    // Eye height over a starting place, as Massassi's preview has it.
    var EYE = 1 / 16;

    // ---- Formats

    function u32(b, at) { return (b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)) >>> 0; }
    function i32(b, at) { return u32(b, at) | 0; }

    // A GOB's files: name (lowercased, '/') -> [offset, size].
    function gobFiles(b) {
        var out = {};
        if (!b || b.length < 16 || b[0] !== 0x47 || b[1] !== 0x4f || b[2] !== 0x42 || b[3] !== 0x20 || u32(b, 4) !== 20)
            return null;
        var table = u32(b, 8);
        if (table + 4 > b.length)
            return null;
        var count = u32(b, table);
        if (table + 4 + count * 136 > b.length)
            return null;
        for (var i = 0; i < count; i++) {
            var at = table + 4 + i * 136, offset = u32(b, at), size = u32(b, at + 4);
            if (offset + size > b.length)
                continue;
            var name = '';
            for (var j = 0; j < 128 && b[at + 8 + j]; j++)
                name += String.fromCharCode(b[at + 8 + j]);
            out[name.replace(/\\/g, '/').toLowerCase()] = [offset, size];
        }
        return out;
    }

    // A colormap's 256 colors (768 bytes) after its 64-byte header ("CMP ").
    function cmpColors(b) {
        if (!b || b.length < 64 + 768 || u32(b, 0) !== 0x20504d43)
            return null;
        return b.subarray(64, 64 + 768);
    }

    // A MAT's first cel as RGBA, rows as the file has them (row 0 = v 0), as
    // jkview's mat.py reads it: 8-bit through the colormap (the cel's
    // transparent color clear), 16-bit by the file's own channel layout.
    // A color MAT (no texture) is one pixel.
    function decodeMat(b, pal) {
        if (!b || b.length < 76 || u32(b, 0) !== 0x2054414d || i32(b, 4) !== 50)
            return null;
        var type = i32(b, 8), count = i32(b, 12);
        var bits = i32(b, 24);
        var cb = [i32(b, 28), i32(b, 32), i32(b, 36)], cs = [i32(b, 40), i32(b, 44), i32(b, 48)],
            ce = [i32(b, 52), i32(b, 56), i32(b, 60)];
        var at = 76;
        if (count < 1 || count > 4096)
            return null;
        if (type === 0) {
            var c = u32(b, at + 4) & 255;
            var px = new Uint8Array(4);
            if (pal) { px[0] = pal[c * 3]; px[1] = pal[c * 3 + 1]; px[2] = pal[c * 3 + 2]; } else { px[0] = px[1] = px[2] = c; }
            px[3] = 255;
            return { w: 1, h: 1, rgba: px };
        }
        if (type !== 2)
            return null;
        var transparent = u32(b, at + 4) & 255;
        at += count * 40;
        if (at + 24 > b.length)
            return null;
        var w = i32(b, at), h = i32(b, at + 4), hasT = i32(b, at + 8);
        at += 24;
        if (w < 1 || h < 1 || w > 4096 || h > 4096)
            return null;
        var n = w * h, out = new Uint8Array(n * 4);
        if (bits === 8) {
            if (at + n > b.length)
                return null;
            for (var i = 0; i < n; i++) {
                var k = b[at + i], o = i * 4;
                if (pal) { out[o] = pal[k * 3]; out[o + 1] = pal[k * 3 + 1]; out[o + 2] = pal[k * 3 + 2]; }
                else { out[o] = out[o + 1] = out[o + 2] = k; }
                out[o + 3] = hasT && k === transparent ? 0 : 255;
            }
        } else if (bits === 16) {
            if (at + n * 2 > b.length)
                return null;
            var mr = (1 << cb[0]) - 1, mg = (1 << cb[1]) - 1, mb = (1 << cb[2]) - 1;
            for (var j = 0; j < n; j++) {
                var v = b[at + j * 2] | (b[at + j * 2 + 1] << 8), q = j * 4;
                out[q] = ((v >> cs[0]) & mr) << ce[0];
                out[q + 1] = ((v >> cs[1]) & mg) << ce[1];
                out[q + 2] = ((v >> cs[2]) & mb) << ce[2];
                out[q + 3] = 255;
            }
        } else {
            return null;
        }
        return { w: w, h: h, rgba: out };
    }

    // ---- The player's own game files

    var gameCache = {};

    // Finds a file in the game's own GOBs by its path (lowercased, '/'):
    // its bytes, or null.
    function gameFiles(game) {
        if (gameCache[game])
            return gameCache[game];
        var c = Module.jkMaps && Module.jkMaps.core;
        var gobs = [];
        if (c && c.findPath && c.fileBytes && Module.FS) {
            var root = c.gameRoot();
            for (var rel of GAME_GOBS[game] || []) {
                var path = c.findPath(root, rel);
                if (!path || !c.isFile(path))
                    continue;
                try {
                    var bytes = c.fileBytes(path);
                    var files = gobFiles(bytes);
                    if (files)
                        gobs.push({ bytes: bytes, files: files });
                } catch (e) {}
            }
        }
        var api = {
            count: gobs.length,
            find: function(name) {
                for (var g of gobs) {
                    var f = g.files[name];
                    if (f)
                        return g.bytes.subarray(f[0], f[0] + f[1]);
                }
                return null;
            }
        };
        if (gobs.length)
            gameCache[game] = api;
        return api;
    }

    // The game looks for a level's textures in mat/ and 3do/mat/.
    function findMat(files, name) {
        var base = name.split('/').pop();
        return files.find(name) || files.find('mat/' + base) || files.find('3do/mat/' + base);
    }

    // ---- A level's map, ready to draw

    function b64bytes(s) {
        var bin = atob(s), out = new Uint8Array(bin.length);
        for (var i = 0; i < bin.length; i++)
            out[i] = bin.charCodeAt(i);
        return out;
    }

    // Surfaces as triangles (fans, as the preview's own viewer makes them),
    // one draw a texture: { verts: [x, y, z, u, v, light]..., index, draws:
    // [{ mat, first, count }], min, max }.
    function buildGeometry(groups) {
        var nv = 0, ni = 0;
        for (var g of groups) {
            for (var s of g || []) {
                var k = (s.vertices || []).length;
                if (k >= 3) { nv += k; ni += (k - 2) * 3; }
            }
        }
        var verts = new Float32Array(nv * 6), byMat = new Map();
        var min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
        var v0 = 0;
        for (var g2 of groups) {
            for (var s2 of g2 || []) {
                var vs = s2.vertices || [];
                if (vs.length < 3)
                    continue;
                for (var i = 0; i < vs.length; i++) {
                    var p = vs[i][0] || [0, 0, 0], t = vs[i][1] || [0, 0], o = (v0 + i) * 6;
                    verts[o] = +p[0] || 0; verts[o + 1] = +p[1] || 0; verts[o + 2] = +p[2] || 0;
                    verts[o + 3] = +t[0] || 0; verts[o + 4] = +t[1] || 0;
                    verts[o + 5] = vs[i][2] == null ? 1 : +vs[i][2];
                    for (var a = 0; a < 3; a++) {
                        if (verts[o + a] < min[a]) min[a] = verts[o + a];
                        if (verts[o + a] > max[a]) max[a] = verts[o + a];
                    }
                }
                var list = byMat.get(s2.material | 0);
                if (!list)
                    byMat.set(s2.material | 0, list = []);
                for (var j = 2; j < vs.length; j++)
                    list.push(v0, v0 + j - 1, v0 + j);
                v0 += vs.length;
            }
        }
        var index = new Uint32Array(ni), draws = [], at = 0;
        byMat.forEach(function(list, mat) {
            index.set(list, at);
            draws.push({ mat: mat, first: at, count: list.length });
            at += list.length;
        });
        return { verts: verts, index: index, draws: draws, min: min, max: max, empty: nv === 0 };
    }

    // A map as it's drawn: the world's triangles, the sky's, a color for
    // each texture, the starting places.
    function prepareMap(map) {
        return {
            world: buildGeometry([map.surfaces, map.model_surfaces]),
            sky: buildGeometry([map.sky_surfaces]),
            colors: map.material_colors || [],
            spawns: map.spawn_points || []
        };
    }

    // What each texture is drawn with: { src: 'level' | 'game' | 'standin' |
    // 'color', name }, from the level's list of its own textures (tex.json).
    // Without that list nothing is swapped: the preview's PNGs as they are.
    function textureSources(mats, tex, mapName, files) {
        var own = {};
        for (var n of (tex && tex.own) || [])
            own[String(n).toLowerCase()] = true;
        var cmpName = tex && tex.cmp ? (tex.cmp[mapName] || tex.cmp[String(mapName).toLowerCase()] || null) : null;
        var pal = null;
        if (tex && cmpName) {
            if (tex.pal && tex.pal[cmpName])
                pal = b64bytes(tex.pal[cmpName]);
            else if (files)
                pal = cmpColors(files.find('misc/cmp/' + cmpName));
        }
        var out = [];
        for (var i = 0; i < (mats || []).length; i++) {
            var m = mats[i], name = m && m.name ? String(m.name).replace(/\\/g, '/').toLowerCase() : '';
            if (!m || !m.data) {
                out.push({ src: 'color', name: name });
            } else if (!tex || !pal || own[name] || own['mat/' + name.split('/').pop()] || own['3do/mat/' + name.split('/').pop()]) {
                out.push({ src: tex && pal ? 'level' : 'standin', name: name });
            } else {
                var b = files ? findMat(files, name) : null;
                out.push(b ? { src: 'game', name: name, bytes: b } : { src: 'standin', name: name });
            }
        }
        return { list: out, pal: pal, cmp: cmpName };
    }

    // A PNG (data: URL) as RGBA, rows bottom first (row 0 = v 0, as the
    // preview made it from the MAT: flipped top to bottom).
    async function pngPixels(dataUrl) {
        var comma = dataUrl.indexOf(',');
        var blob = new Blob([b64bytes(dataUrl.substring(comma + 1))], { type: 'image/png' });
        var bmp = await createImageBitmap(blob);
        var w = bmp.width, h = bmp.height;
        var cv = typeof OffscreenCanvas === 'function' ? new OffscreenCanvas(w, h) : Object.assign(document.createElement('canvas'), { width: w, height: h });
        var g = cv.getContext('2d');
        g.drawImage(bmp, 0, 0);
        if (bmp.close)
            bmp.close();
        var top = g.getImageData(0, 0, w, h).data, out = new Uint8Array(w * h * 4), row = w * 4;
        for (var y = 0; y < h; y++)
            out.set(top.subarray((h - 1 - y) * row, (h - y) * row), y * row);
        return { w: w, h: h, rgba: out };
    }

    function hexColor(s) {
        var m = /^#?([0-9a-f]{6})$/i.exec(String(s || ''));
        var v = m ? parseInt(m[1], 16) : 0x808080;
        return [(v >> 16 & 255) / 255, (v >> 8 & 255) / 255, (v & 255) / 255];
    }

    // ---- Drawing

    var VS = '#version 300 es\n' +
        'in vec3 aPos; in vec2 aUv; in float aLight;\n' +
        'uniform mat4 uMvp;\n' +
        'out vec2 vUv; out float vLight;\n' +
        'void main() { gl_Position = uMvp * vec4(aPos, 1.0); vUv = aUv; vLight = aLight; }\n';
    // The surface's light as the level has it, with a floor so that a dark
    // corner still shows from outside.
    var FS = '#version 300 es\n' +
        'precision mediump float;\n' +
        'uniform sampler2D uTex; uniform vec3 uColor; uniform float uUseTex; uniform float uLit;\n' +
        'in vec2 vUv; in float vLight; out vec4 o;\n' +
        'void main() {\n' +
        '  vec4 c = uUseTex > 0.5 ? texture(uTex, vUv) : vec4(uColor, 1.0);\n' +
        '  if (c.a < 0.5) discard;\n' +
        '  float l = mix(1.0, 0.3 + 0.7 * clamp(vLight, 0.0, 1.0), uLit);\n' +
        '  o = vec4(c.rgb * l, 1.0);\n' +
        '}\n';

    function Renderer(canvas) {
        var gl = canvas.getContext('webgl2', { antialias: true, alpha: false, preserveDrawingBuffer: false });
        if (!gl)
            throw new Error('no WebGL 2');
        // Lost (the graphics card was reset) and not back yet: try later.
        if (gl.isContextLost())
            throw { lost: true };
        var sh = function(type, src) {
            var s = gl.createShader(type);
            gl.shaderSource(s, src);
            gl.compileShader(s);
            if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))
                throw new Error(gl.getShaderInfoLog(s));
            return s;
        };
        var prog = gl.createProgram();
        gl.attachShader(prog, sh(gl.VERTEX_SHADER, VS));
        gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, FS));
        gl.bindAttribLocation(prog, 0, 'aPos');
        gl.bindAttribLocation(prog, 1, 'aUv');
        gl.bindAttribLocation(prog, 2, 'aLight');
        gl.linkProgram(prog);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS))
            throw new Error(gl.getProgramInfoLog(prog));
        this.gl = gl;
        this.prog = prog;
        this.u = {};
        for (var n of ['uMvp', 'uTex', 'uColor', 'uUseTex', 'uLit'])
            this.u[n] = gl.getUniformLocation(prog, n);
        this.meshes = [];
        this.textures = [];
        this.colors = [];
    }

    Renderer.prototype.mesh = function(geo) {
        var gl = this.gl;
        var vao = gl.createVertexArray();
        gl.bindVertexArray(vao);
        var vb = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, vb);
        gl.bufferData(gl.ARRAY_BUFFER, geo.verts, gl.STATIC_DRAW);
        var ib = gl.createBuffer();
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib);
        gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, geo.index, gl.STATIC_DRAW);
        gl.enableVertexAttribArray(0);
        gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 24, 0);
        gl.enableVertexAttribArray(1);
        gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 24, 12);
        gl.enableVertexAttribArray(2);
        gl.vertexAttribPointer(2, 1, gl.FLOAT, false, 24, 20);
        gl.bindVertexArray(null);
        var m = { vao: vao, vb: vb, ib: ib, draws: geo.draws };
        this.meshes.push(m);
        return m;
    };

    Renderer.prototype.texture = function(i, px) {
        var gl = this.gl;
        var t = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, t);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, px.w, px.h, 0, gl.RGBA, gl.UNSIGNED_BYTE, px.rgba);
        gl.generateMipmap(gl.TEXTURE_2D);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        if (this.textures[i])
            gl.deleteTexture(this.textures[i]);
        this.textures[i] = t;
    };

    Renderer.prototype.clear = function() {
        var gl = this.gl;
        for (var m of this.meshes) {
            gl.deleteVertexArray(m.vao);
            gl.deleteBuffer(m.vb);
            gl.deleteBuffer(m.ib);
        }
        for (var t of this.textures) {
            if (t)
                gl.deleteTexture(t);
        }
        this.meshes = [];
        this.textures = [];
        this.colors = [];
    };

    Renderer.prototype.draw = function(w, h, mvp, meshes) {
        var gl = this.gl;
        gl.viewport(0, 0, w, h);
        gl.clearColor(0, 0, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
        gl.enable(gl.DEPTH_TEST);
        gl.disable(gl.CULL_FACE);
        gl.useProgram(this.prog);
        gl.uniformMatrix4fv(this.u.uMvp, false, mvp);
        gl.uniform1i(this.u.uTex, 0);
        gl.activeTexture(gl.TEXTURE0);
        for (var e of meshes) {
            gl.uniform1f(this.u.uLit, e.lit ? 1 : 0);
            gl.bindVertexArray(e.mesh.vao);
            for (var d of e.mesh.draws) {
                var t = this.textures[d.mat];
                gl.uniform1f(this.u.uUseTex, t ? 1 : 0);
                if (t)
                    gl.bindTexture(gl.TEXTURE_2D, t);
                else
                    gl.uniform3fv(this.u.uColor, this.colors[d.mat] || [0.5, 0.5, 0.5]);
                gl.drawElements(gl.TRIANGLES, d.count, gl.UNSIGNED_INT, d.first * 4);
            }
        }
        gl.bindVertexArray(null);
    };

    // ---- Cameras (z is up, as in the game)

    function perspective(fovy, aspect, near, far) {
        var f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
        return [f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) * nf, -1, 0, 0, 2 * far * near * nf, 0];
    }

    function lookDir(eye, dir) {
        var up = [0, 0, 1];
        var f = norm(dir);
        var s = norm(cross(f, up));
        if (!isFinite(s[0]) || (s[0] === 0 && s[1] === 0 && s[2] === 0))
            s = [1, 0, 0];
        var u = cross(s, f);
        return [s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0,
                -dot(s, eye), -dot(u, eye), dot(f, eye), 1];
    }

    function mul(a, b) {
        var o = new Float32Array(16);
        for (var c = 0; c < 4; c++)
            for (var r = 0; r < 4; r++)
                o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
        return o;
    }

    function cross(a, b) { return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; }
    function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    function norm(a) { var l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0] / l, a[1] / l, a[2] / l]; }

    // Where the eye looks along yaw (0 = +y, as a starting place's) and pitch.
    function direction(yaw, pitch) {
        return [Math.sin(yaw) * Math.cos(pitch), Math.cos(yaw) * Math.cos(pitch), Math.sin(pitch)];
    }

    // ---- The view

    var V = {
        // The level shown: { id, m, game } and its load.
        want: null, have: null, state: 'none', error: '', abort: null, since: 0,
        scene: null, renderer: null, canvas: null, icon: null, failedGl: false,
        // The frames the menu leaves: small (Overview), big (the 3D view menu).
        small: { on: false, x: 0, y: 0, w: 0, h: 0, polled: 0 },
        big: { on: false, x: 0, y: 0, w: 0, h: 0, polled: 0 },
        mode: 'orbit', orbit: null, walk: null, start: 0, raf: 0, last: 0,
        keys: {}, drag: null, speed: 1.5, changed: false
    };

    function fail(text) {
        V.state = 'failed';
        V.error = text;
        V.changed = true;
    }

    function canvas() {
        if (V.canvas || typeof document === 'undefined' || !document.body)
            return V.canvas;
        var c = document.createElement('canvas');
        c.id = 'jk-lib-3d';
        c.style.cssText = 'position:fixed;z-index:1;display:none;pointer-events:none;background:#000;touch-action:none;';
        c.addEventListener('pointerdown', onDown);
        c.addEventListener('pointermove', onMove);
        c.addEventListener('pointerup', onUp);
        c.addEventListener('pointercancel', onUp);
        c.addEventListener('wheel', onWheel, { passive: false });
        c.addEventListener('contextmenu', function(e) { e.preventDefault(); });
        c.addEventListener('webglcontextlost', function(e) {
            e.preventDefault();
            V.renderer = null;
            V.scene = null;
            V.have = null;
            if (V.want)
                fail('The 3D view stopped (the graphics card was reset). Click to try again.');
        });
        document.body.appendChild(c);
        // The button that makes it bigger, drawn over the small view's corner
        // (the menu takes the click: anywhere on the small view).
        var icon = document.createElement('div');
        icon.id = 'jk-lib-3d-icon';
        icon.style.cssText = 'position:fixed;z-index:2;display:none;pointer-events:none;';
        icon.innerHTML = '<svg viewBox="0 0 16 16" width="100%" height="100%" style="display:block;filter:drop-shadow(0 0 1px #000) drop-shadow(0 0 1px #000)">' +
            '<path fill="none" stroke="#ffcc33" stroke-width="1.6" stroke-linecap="square" ' +
            'd="M2 6V2h4M10 2h4v4M14 10v4h-4M6 14H2v-4M2 2l4 4M14 2l-4 4M14 14l-4-4M2 14l4-4"/></svg>';
        document.body.appendChild(icon);
        window.addEventListener('keydown', onKey, true);
        window.addEventListener('keyup', onKey, true);
        window.addEventListener('blur', function() { V.keys = {}; });
        V.canvas = c;
        V.icon = icon;
        return c;
    }

    function renderer() {
        if (V.renderer || V.failedGl)
            return V.renderer;
        try {
            V.renderer = new Renderer(canvas());
        } catch (e) {
            if (e && e.lost)
                return null;
            V.failedGl = true;
            console.warn('OpenJKDF2: the 3D view can\'t draw here: ' + e);
        }
        return V.renderer;
    }

    // Asks for a level's map m; it loads once the pick has rested a moment.
    function want(id, m, mapName, game) {
        var w = V.want;
        if (w && w.id === id && w.m === m && w.game === game)
            return;
        if (V.abort)
            V.abort.abort();
        V.abort = null;
        V.want = id == null ? null : { id: id, m: m | 0, name: mapName || '', game: game };
        V.since = Date.now();
        V.state = id == null ? 'none' : 'wait';
        V.error = '';
        V.changed = true;
        kick();
    }

    async function getJson(path, signal, optional) {
        var res = await fetch(BASE + path, { signal: signal });
        if (!res.ok) {
            if (optional)
                return null;
            throw new Error('HTTP ' + res.status + ' for ' + path);
        }
        return res.json();
    }

    // Reading a big map (up to about 20 MB of JSON) and making its triangles
    // would hold the game's thread for a third of a second: a worker does
    // it, where the browser lets the page start one.
    var worker = { url: null, off: false };

    function workerUrl() {
        if (!worker.url) {
            var src = 'var buildGeometry = ' + buildGeometry.toString() + ';\n' +
                'var prepareMap = ' + prepareMap.toString() + ';\n' +
                'onmessage = function(e) {\n' +
                '  fetch(e.data.url).then(function(res) {\n' +
                '    if (!res.ok) throw new Error("HTTP " + res.status + " for " + e.data.path);\n' +
                '    return res.json();\n' +
                '  }).then(function(map) {\n' +
                '    var m = prepareMap(map);\n' +
                '    postMessage({ map: m }, [m.world.verts.buffer, m.world.index.buffer, m.sky.verts.buffer, m.sky.index.buffer]);\n' +
                '  }).catch(function(err) { postMessage({ error: String(err && err.message || err) }); });\n' +
                '};\n';
            worker.url = URL.createObjectURL(new Blob([src], { type: 'text/javascript' }));
        }
        return worker.url;
    }

    function fetchMap(path, signal) {
        var here = function() { return getJson(path, signal).then(prepareMap); };
        if (worker.off || typeof Worker !== 'function' || typeof URL.createObjectURL !== 'function')
            return here();
        var wk;
        try {
            wk = new Worker(workerUrl());
        } catch (e) {
            worker.off = true;
            return here();
        }
        return new Promise(function(resolve, reject) {
            var done = function() {
                wk.terminate();
                signal.removeEventListener('abort', onAbort);
            };
            var onAbort = function() {
                done();
                reject(new Error('aborted'));
            };
            signal.addEventListener('abort', onAbort);
            wk.onmessage = function(e) {
                done();
                if (e.data && e.data.map)
                    resolve(e.data.map);
                else
                    reject(new Error(e.data && e.data.error || 'no map'));
            };
            // A worker the page can't start: here, from now on.
            wk.onerror = function(e) {
                if (e && e.preventDefault)
                    e.preventDefault();
                done();
                worker.off = true;
                here().then(resolve, reject);
            };
            wk.postMessage({ url: new URL(BASE + path, location.href).href, path: path });
        });
    }

    async function load(w) {
        var ac = new AbortController();
        V.abort = ac;
        V.state = 'loading';
        V.changed = true;
        var r = renderer();
        if (!r) {
            fail(V.failedGl ? 'The 3D view needs WebGL 2, which this browser has turned off.'
                            : 'The 3D view stopped (the graphics card was reset). Click to try again.');
            return;
        }
        try {
            var got = await Promise.all([
                fetchMap(w.id + '-' + w.m + '.map.json', ac.signal),
                getJson(w.id + '-' + w.m + '.mat.json', ac.signal, true),
                getJson(w.id + '.tex.json', ac.signal, true)
            ]);
            if (V.want !== w)
                return;
            var map = got[0], mats = got[1] || [], tex = got[2];
            if (map.world.empty)
                throw new Error('no surfaces');
            r.clear();
            r.colors = map.colors.map(hexColor);
            var scene = {
                id: w.id, m: w.m, world: r.mesh(map.world), sky: map.sky.empty ? null : r.mesh(map.sky),
                min: map.world.min, max: map.world.max, spawns: map.spawns, missing: 0, swapped: 0, total: 0
            };
            V.scene = scene;
            V.have = w;
            V.orbit = null;
            V.walk = null;
            V.start = 0;
            if (V.mode === 'walk')
                toStart(0);
            V.state = 'ready';
            V.changed = true;
            kick();
            await loadTextures(w, scene, mats, tex, r, ac.signal);
        } catch (e) {
            if (ac.signal.aborted || V.want !== w)
                return;
            console.warn('OpenJKDF2: the 3D view of level ' + w.id + ' did not load: ' + e);
            fail('The 3D view didn\'t load. Click to try again.');
        } finally {
            if (V.abort === ac)
                V.abort = null;
        }
    }

    // One texture's pixels: the game's own from the player's files, else
    // the preview's PNG (counted as missing when it stands in for one of the
    // game's).
    async function texturePixels(scene, srcs, mats, tex, i) {
        var s = srcs.list[i], px = null;
        try {
            if (s.src === 'game') {
                px = decodeMat(s.bytes, srcs.pal);
                if (px)
                    scene.swapped++;
            }
            if (!px && mats[i] && mats[i].data) {
                px = await pngPixels(mats[i].data);
                if (s.src !== 'level' && tex && srcs.pal)
                    scene.missing++;
            }
        } catch (e) {
            px = null;
        }
        return px;
    }

    // In batches: the browser reads a batch's PNGs at once, and the game
    // keeps its frames between them.
    var TEXTURE_BATCH = 16;

    async function loadTextures(w, scene, mats, tex, r, signal) {
        var files = tex ? gameFiles(w.game) : null;
        var srcs = textureSources(mats, tex, w.name, files && files.count ? files : null);
        scene.total = srcs.list.length;
        for (var at = 0; at < srcs.list.length; at += TEXTURE_BATCH) {
            if (signal.aborted || V.scene !== scene)
                return;
            var jobs = [];
            for (var i = at; i < Math.min(at + TEXTURE_BATCH, srcs.list.length); i++)
                jobs.push(texturePixels(scene, srcs, mats, tex, i));
            var got = await Promise.all(jobs);
            if (signal.aborted || V.scene !== scene || V.renderer !== r)
                return;
            for (var k = 0; k < got.length; k++) {
                if (got[k])
                    r.texture(at + k, got[k]);
            }
        }
        scene.loaded = true;
        V.changed = true;
    }

    // ---- Moving around

    function center(s) { return [(s.min[0] + s.max[0]) / 2, (s.min[1] + s.max[1]) / 2, (s.min[2] + s.max[2]) / 2]; }
    function radius(s) { return Math.max(0.5, Math.hypot(s.max[0] - s.min[0], s.max[1] - s.min[1], s.max[2] - s.min[2]) / 2); }

    function orbitStart(s) {
        return { target: center(s), yaw: Math.PI * 0.25, pitch: 0.6, dist: radius(s) / Math.sin(FOV / 2) * 0.95 };
    }

    function toStart(k) {
        var s = V.scene;
        if (!s)
            return;
        var list = s.spawns;
        if (!list.length) {
            var o = orbitStart(s);
            V.walk = { pos: center(s), yaw: o.yaw, pitch: 0 };
            return;
        }
        V.start = ((k % list.length) + list.length) % list.length;
        var p = list[V.start].pos || [0, 0, 0], rot = list[V.start].rot || [0, 0, 0];
        V.walk = { pos: [+p[0] || 0, +p[1] || 0, (+p[2] || 0) + EYE], yaw: -(+rot[1] || 0) * Math.PI / 180, pitch: 0 };
    }

    function setMode(mode) {
        V.mode = mode === 'walk' ? 'walk' : 'orbit';
        if (V.mode === 'walk' && !V.walk)
            toStart(V.start);
        V.changed = true;
    }

    function onDown(e) {
        if (!V.big.on)
            return;
        V.canvas.setPointerCapture(e.pointerId);
        V.drag = { x: e.clientX, y: e.clientY, pan: e.button === 2 || e.button === 1 || e.shiftKey };
        V.canvas.style.cursor = 'grabbing';
        e.preventDefault();
    }

    function onMove(e) {
        var d = V.drag;
        if (!d || !V.scene)
            return;
        var dx = e.clientX - d.x, dy = e.clientY - d.y;
        d.x = e.clientX;
        d.y = e.clientY;
        var k = 3.0 / Math.max(200, V.canvas.clientHeight);
        if (V.mode === 'walk') {
            V.walk.yaw += dx * k;
            V.walk.pitch = Math.max(-1.5, Math.min(1.5, V.walk.pitch - dy * k));
        } else {
            var o = V.orbit || (V.orbit = orbitStart(V.scene));
            if (d.pan) {
                var f = direction(o.yaw, -o.pitch), sd = norm(cross(f, [0, 0, 1])), ud = cross(sd, f);
                var m = o.dist * Math.tan(FOV / 2) * 2 / Math.max(200, V.canvas.clientHeight);
                for (var a = 0; a < 3; a++)
                    o.target[a] += (-sd[a] * dx + ud[a] * dy) * m;
            } else {
                o.yaw += dx * k;
                o.pitch = Math.max(-1.5, Math.min(1.5, o.pitch + dy * k));
            }
        }
    }

    function onUp(e) {
        V.drag = null;
        if (V.canvas)
            V.canvas.style.cursor = V.big.on ? 'grab' : '';
    }

    function onWheel(e) {
        if (!V.big.on || !V.scene)
            return;
        e.preventDefault();
        var notches = Math.max(-5, Math.min(5, e.deltaY / (e.deltaMode === 1 ? 3 : 100)));
        if (V.mode === 'walk') {
            var w = V.walk, f = direction(w.yaw, w.pitch);
            for (var a = 0; a < 3; a++)
                w.pos[a] -= f[a] * notches * 0.25;
        } else {
            var o = V.orbit || (V.orbit = orbitStart(V.scene));
            o.dist = Math.max(0.2, Math.min(radius(V.scene) * 20, o.dist * Math.pow(1.15, notches)));
        }
    }

    var MOVE_KEYS = { KeyW: 1, KeyA: 1, KeyS: 1, KeyD: 1, KeyQ: 1, KeyE: 1, KeyC: 1, Space: 1, ArrowUp: 1, ArrowDown: 1,
                      ArrowLeft: 1, ArrowRight: 1, ShiftLeft: 1, ShiftRight: 1 };

    // The keys move the eye only while the big view is up; the game gets
    // them as well (Escape is the menu's Back).
    function onKey(e) {
        if (!V.big.on || !MOVE_KEYS[e.code])
            return;
        V.keys[e.code] = e.type === 'keydown';
    }

    function step(dt) {
        if (!V.big.on || V.mode !== 'walk' || !V.walk)
            return;
        var k = V.keys, w = V.walk;
        var fw = (k.KeyW || k.ArrowUp ? 1 : 0) - (k.KeyS || k.ArrowDown ? 1 : 0);
        var side = (k.KeyD ? 1 : 0) - (k.KeyA ? 1 : 0);
        var up = (k.KeyE || k.Space ? 1 : 0) - (k.KeyQ || k.KeyC ? 1 : 0);
        w.yaw += ((k.ArrowRight ? 1 : 0) - (k.ArrowLeft ? 1 : 0)) * dt * 1.8;
        var sp = V.speed * (k.ShiftLeft || k.ShiftRight ? 3 : 1) * dt;
        var f = direction(w.yaw, w.pitch), s = norm(cross(f, [0, 0, 1]));
        for (var a = 0; a < 3; a++)
            w.pos[a] += (f[a] * fw + s[a] * side) * sp;
        w.pos[2] += up * sp;
    }

    // ---- Each frame while a frame of it is up

    function place(el, f, inset) {
        var c = Module.canvas;
        var r = c.getBoundingClientRect();
        var scale = r.height / 480, left = r.left + (r.width - r.height * 4 / 3) / 2;
        el.style.left = (left + f.x * scale + (inset || 0)) + 'px';
        el.style.top = (r.top + f.y * scale + (inset || 0)) + 'px';
        el.style.width = Math.max(1, f.w * scale - 2 * (inset || 0)) + 'px';
        el.style.height = Math.max(1, f.h * scale - 2 * (inset || 0)) + 'px';
        return scale;
    }

    function frame() {
        V.raf = 0;
        var c = V.canvas;
        if (!c)
            return;
        var now = Date.now();
        var big = V.big.on && now - V.big.polled < 500;
        var small = !big && V.small.on && now - V.small.polled < 500;
        // A pick that has rested starts loading.
        if (V.want && V.state === 'wait' && (small || big) && now - V.since >= (big ? 0 : REST_MS))
            load(V.want);
        var show = (big || small) && V.state === 'ready' && V.scene && V.renderer && Module.canvas && Module.canvas.getBoundingClientRect;
        if (!show) {
            c.style.display = 'none';
            V.icon.style.display = 'none';
            if ((V.small.on || V.big.on) && (V.want || V.state !== 'none'))
                V.raf = requestAnimationFrame(frame);
            V.last = 0;
            return;
        }
        var f = big ? V.big : V.small;
        var scale = place(c, f);
        c.style.display = 'block';
        c.style.pointerEvents = big ? 'auto' : 'none';
        c.style.cursor = big ? (V.drag ? 'grabbing' : 'grab') : '';
        if (small) {
            var size = Math.max(12, Math.round(14 * scale / 2) * 2);
            V.icon.style.left = (parseFloat(c.style.left) + parseFloat(c.style.width) - size - 3) + 'px';
            V.icon.style.top = (parseFloat(c.style.top) + parseFloat(c.style.height) - size - 3) + 'px';
            V.icon.style.width = V.icon.style.height = size + 'px';
            V.icon.style.display = 'block';
        } else {
            V.icon.style.display = 'none';
        }
        var dpr = window.devicePixelRatio || 1;
        var w = Math.max(1, Math.round(c.clientWidth * dpr)), h = Math.max(1, Math.round(c.clientHeight * dpr));
        if (c.width !== w || c.height !== h) {
            c.width = w;
            c.height = h;
        }
        var dt = V.last ? Math.min(0.1, (now - V.last) / 1000) : 0;
        V.last = now;
        step(dt);
        draw(w, h, small, now);
        V.raf = requestAnimationFrame(frame);
    }

    function draw(w, h, small, now) {
        var s = V.scene, aspect = w / h, eye, dir, near, far, list;
        if (small || V.mode === 'orbit') {
            var o = small ? orbitStart(s) : (V.orbit || (V.orbit = orbitStart(s)));
            var yaw = small ? o.yaw + (now / 1000) * Math.PI * 2 / TURN_S : o.yaw;
            dir = direction(yaw, -o.pitch);
            eye = [o.target[0] - dir[0] * o.dist, o.target[1] - dir[1] * o.dist, o.target[2] - dir[2] * o.dist];
            near = Math.max(0.01, o.dist - radius(s) * 1.5) * 0.5;
            far = o.dist + radius(s) * 3;
            list = [{ mesh: s.world, lit: true }];
        } else {
            var wk = V.walk;
            eye = wk.pos;
            dir = direction(wk.yaw, wk.pitch);
            near = 0.005;
            far = radius(s) * 8 + 100;
            list = s.sky ? [{ mesh: s.sky, lit: false }, { mesh: s.world, lit: true }] : [{ mesh: s.world, lit: true }];
        }
        var mvp = mul(perspective(FOV, aspect, Math.min(near, 1), far), lookDir(eye, dir));
        V.renderer.draw(w, h, mvp, list);
    }

    function kick() {
        if (!V.raf && typeof requestAnimationFrame === 'function' && canvas())
            V.raf = requestAnimationFrame(frame);
    }

    function setFrame(f, on, x, y, w, h) {
        f.on = !!on;
        f.x = x; f.y = y; f.w = w; f.h = h;
        f.polled = Date.now();
        kick();
    }

    // ---- What the menu and library.js ask

    function stateNumber() {
        switch (V.state) {
        case 'wait': case 'loading': return 1;
        case 'ready': return 2;
        case 'failed': return 3;
        }
        return 0;
    }

    Module.jkPreview3d = {
        // Which level's map to show (id null: none), and the small and big
        // frames, polled each frame they're up.
        want: want,
        small: function(on, x, y, w, h) { setFrame(V.small, on, x, y, w, h); },
        big: function(on, x, y, w, h) {
            var was = V.big.on;
            setFrame(V.big, on, x, y, w, h);
            if (on && !was) {
                V.keys = {};
                V.orbit = null;
                if (V.mode === 'walk')
                    toStart(V.start);
            }
            if (!on) {
                V.keys = {};
                V.drag = null;
            }
        },
        hide: function() {
            V.small.on = V.big.on = false;
            V.keys = {};
            if (V.canvas) {
                V.canvas.style.display = 'none';
                V.icon.style.display = 'none';
            }
        },
        // After it didn't load: the same map again.
        retry: function() {
            var w = V.want;
            if (!w || V.state !== 'failed')
                return;
            V.want = null;
            want(w.id, w.m, w.name, w.game);
        },
        state: stateNumber,
        error: function() { return V.error; },
        mode: function() { return V.mode; },
        setMode: setMode,
        nextStart: function() {
            if (V.scene && V.scene.spawns.length) {
                setMode('walk');
                toStart(V.start + 1);
            }
        },
        starts: function() { return V.scene ? V.scene.spawns.length : 0; },
        start: function() { return V.start; },
        // How many of the game's own textures weren't in the player's files.
        missing: function() { return V.scene ? V.scene.missing : 0; },
        swapped: function() { return V.scene ? V.scene.swapped : 0; },
        // Whether something the menu shows changed since it last asked.
        changed: function() { var c = V.changed; V.changed = false; return c; },
        // For tests.
        _decodeMat: decodeMat, _cmpColors: cmpColors, _gobFiles: gobFiles, _buildGeometry: buildGeometry, _prepareMap: prepareMap, _worker: worker,
        _textureSources: textureSources, _pngPixels: pngPixels, _V: V, _gameCache: gameCache
    };
})();
