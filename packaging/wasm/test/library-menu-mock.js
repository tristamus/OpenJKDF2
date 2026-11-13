// A stand-in for packaging/wasm/library.js, for library-menu-test.c: the
// Module.jkLibrary the Level Library menu (src/Gui/jkGUILibrary.c) talks to,
// with made-up levels, and a record of what the menu asked it to do.

(function() {
    var LONG = 'A Very Long Level Title That Goes On And On Past The Column';
    var M = {
        calls: [], game: '', open: false, available: true, beginOk: true,
        rows: ['Alpha Arena\t1999  12 KB', '* ' + LONG + '\tCTF 2003  3.4 MB', 'Gamma\tSP 2001  80 KB'],
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
        files: '12 KB\talpha.gob  (episode: Alpha Arena)\n1 KB\treadme.txt'
    };

    function touch(bits) { M.changed |= bits; }

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
            M.changed = 31;
            return true;
        },
        end: function() { M.calls.push('end'); M.open = false; },
        text: function(what, arg) {
            switch (what) {
            case 0: return M.rows.length + ' levels from Massassi.net';
            case 1: return M.rows[arg] || '';
            case 2: return M.sorts[M.sort];
            case 3: return M.sel >= 0 ? M.titles[M.sel] : '';
            case 4: return M.sel >= 0 ? M.overview : '';
            case 5: return M.readme;
            case 6: return M.files;
            case 7: return M.busy ? M.msg : 'One download at a time.';
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
        take: function() { var r = M.result; M.result = ''; return r; },
        _M: M
    };
})();
