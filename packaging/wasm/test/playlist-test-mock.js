// The page's side for playlist-test.c: localStorage, which the playlist is
// kept in (src/Main/jkPlaylist.c), and Module.jkMaps.next, which gets the
// next level ready (maps.js), as stand-ins the test can look into and set.

(function() {
    var store = new Map();
    globalThis.localStorage = {
        getItem: function(k) { return store.has(k) ? store.get(k) : null; },
        setItem: function(k, v) { store.set(k, String(v)); },
        removeItem: function(k) { store.delete(k); },
        clear: function() { store.clear(); },
        _store: store
    };

    var M = Module.playlistMock = {
        calls: [],
        state: 1,       // 0 still getting it, 1 has it, 2 couldn't
        progress: -1,
        error: '',
        waiting: { count: 0, least: -1 },
        waitingAsked: []
    };
    Module.jkMaps = {
        next: {
            host: function(name, ext) { M.calls.push('host ' + name + '.' + ext); },
            fetch: function(name, ext) { M.calls.push('fetch ' + name + '.' + ext); },
            state: function() { return M.state; },
            progress: function() { return M.progress; },
            error: function() { return M.error; },
            waiting: function(ids) { M.waitingAsked.push(ids.slice()); return M.waiting; }
        }
    };
})();
