// A stand-in for packaging/wasm/library.js, for library-menu-test.c: the
// Module.jkLibrary the Level Library menu (src/Gui/jkGUILibrary.c) talks to,
// with made-up levels, and a record of what the menu asked it to do.

(function() {
    var LONG = 'A Very Long Level Title That Goes On And On Past The Column';
    var M = {
        calls: [], game: '', open: false, available: true, beginOk: true,
        rows: ['Alpha Arena\t1999 12 KB', '* ' + LONG + '\t2003 3.4 MB\tCTF', 'Gamma\t2001 80 KB\tSP'],
        titles: ['Alpha Arena', LONG, 'Gamma'],
        sel: 0, changed: 0, busy: false, done: false, result: '', shots: 2, msg: '',
        q: null, mask: -1, sort: 0, lastShot: null, nextShots: 0, pages: 0, doneIn: -1,
        sorts: ['Name', 'Newest first', 'Oldest first', 'Most downloaded', 'Top rated', 'Smallest first'],
        overview: [
            'Author:\tTester',
            'Released:\tMay 2, 1999 (from its readme; Massassi says added Jun 2, 2000, the day it imported most older levels)',
            'Type:\tMultiplayer',
            '',
            'A description long enough that it has to be broken into several lines to fit in the panel, ' +
                'with a word too long for one line: ' + 'x'.repeat(80) + ' and the end.',
            '',
            'Last line.'
        ].join('\n'),
        readme: '--- readme.txt ---\n\nTitle: Alpha Arena\n\nA readme line.',
        files: '12 KB\talpha.gob  (episode: Alpha Arena)\n1 KB\treadme.txt',
        // The 3D view: 0 none, 1 coming, 2 there, 3 failed; what the page
        // was last told about its frames, and Walk or Orbit.
        pv: 0, lastPreview: null, lastView: null, views: 0, walk: false, status: ''
    };

    function touch(bits) { M.changed |= bits; }
    function pv() { return M.sel >= 0 ? M.pv : 0; }

    Module.jkLibrary = {
        available: function() { return M.available; },
        begin: function(game) {
            M.calls.push('begin ' + game);
            if (!M.beginOk)
                return false;
            M.game = game;
            M.open = true;
            M.done = false;
            M.result = '';
            M.changed = 127;
            return true;
        },
        end: function() { M.calls.push('end'); M.open = false; },
        text: function(what, arg) {
            switch (what) {
            case 0: return M.status || M.rows.length + ' levels from Massassi.net';
            case 1: return M.rows[arg] || '';
            case 2: return M.sorts[M.sort];
            case 3: return M.sel >= 0 ? M.titles[M.sel] : '';
            case 4: return M.sel >= 0 ? M.overview : '';
            case 5: return M.readme;
            case 6: return M.files;
            case 7: return M.busy ? M.msg : 'One download at a time.';
            case 8: return ['No 3D view for this level yet', 'Loading the 3D view...', '', 'The 3D view failed.'][pv()];
            case 9: return M.sel >= 0 ? M.titles[M.sel] : '';
            case 10: return M.walk ? 'Walk: W A S D, drag to look.' : 'Orbit: drag to turn, wheel to zoom.';
            case 11: return M.walk ? 'Starting place 1 of 3' : '3D view from the test.';
            case 12: return pv() === 2 ? (M.walk ? 'Orbit' : 'Walk') : '';
            case 13: return pv() === 2 ? (M.walk ? 'Next Start' : 'Next Map') : pv() ? 'Next Map' : '';
            }
            return '';
        },
        number: function(what) {
            switch (what) {
            case 0: { var c = M.changed; M.changed = 0; return c; }
            case 1: return M.rows.length;
            case 2: return M.sel;
            case 3: return !M.busy && M.sel >= 0 && M.sel !== 2 ? 1 : 0;
            case 4: return M.busy ? 1 : 0;
            case 5:
                if (M.doneIn > 0 && --M.doneIn === 0) {
                    M.busy = false;
                    M.done = true;
                    M.result = 'alpha';
                }
                return M.done ? 1 : 0;
            case 6: return M.sel >= 0 ? M.shots : 0;
            case 7: return pv();
            }
            return 0;
        },
        query: function(q, mask, sort) {
            M.calls.push('query ' + JSON.stringify(q) + ' ' + mask + ' ' + sort);
            M.q = q;
            M.mask = mask;
            M.sort = sort;
            touch(2);
        },
        select: function(i) {
            M.calls.push('select ' + i);
            if (M.busy || i === M.sel || i < 0 || i >= M.rows.length)
                return;
            M.sel = i;
            touch(4 | 8 | 16);
        },
        use: function() {
            M.calls.push('use');
            if (M.busy || M.sel < 0)
                return false;
            M.busy = true;
            M.msg = 'Getting ' + M.titles[M.sel] + ' (50%)';
            M.doneIn = M.useFinishes ? 3 : -1;
            touch(8 | 16);
            return true;
        },
        cancel: function() {
            M.calls.push('cancel');
            if (!M.busy)
                return false;
            M.busy = false;
            M.doneIn = -1;
            touch(8 | 16);
            return true;
        },
        page: function() { M.pages++; },
        shot: function(on, x, y, w, h) { M.lastShot = [on, x, y, w, h]; },
        nextShot: function() { M.nextShots++; },
        preview: function(on, x, y, w, h) { M.lastPreview = [on, x, y, w, h]; },
        view: function(on, x, y, w, h) {
            if (on && !(M.lastView && M.lastView[0]))
                M.views++;
            M.lastView = [on, x, y, w, h];
        },
        viewCommand: function(cmd) {
            M.calls.push('viewCommand ' + cmd);
            if (cmd === 1)
                M.walk = !M.walk;
            touch(64);
        },
        take: function() { var r = M.result; M.result = ''; return r; },
        _M: M
    };
})();
