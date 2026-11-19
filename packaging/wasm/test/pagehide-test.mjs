// Checks the page's back/forward cache step (index.html, "Left for another
// page, but kept by the browser"): every socket the page or the game opens
// is closed as the page goes into the cache, and the page starts again when
// it's come back to. Runs index.html's script for it on a stand-in window.
//
//   node test/pagehide-test.mjs

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const at = html.indexOf('// ---- Left for another page, but kept by the browser');
assert.ok(at > 0, 'index.html has the step');
const start = html.lastIndexOf('<script>', at) + '<script>'.length;
const source = html.substring(start, html.indexOf('</script>', at));

class RealSocket {
    constructor(url, protocols) {
        this.url = url;
        this.protocols = protocols;
        this.args = arguments.length;
        this.closed = 0;
        this.listeners = {};
    }
    addEventListener(type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); }
    close() {
        this.closed++;
        (this.listeners.close || []).forEach(fn => fn());
    }
}
Object.assign(RealSocket, { CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 });

function page() {
    const handlers = {};
    const win = {
        WebSocket: RealSocket,
        reloads: 0,
        addEventListener(type, fn) { (handlers[type] = handlers[type] || []).push(fn); },
        fire(type, ev) { (handlers[type] || []).forEach(fn => fn(ev)); },
    };
    const location = { reload() { win.reloads++; } };
    new Function('window', 'location', source)(win, location);
    return win;
}

let failed = 0, passed = 0;
function test(name, fn) {
    try {
        fn();
        passed++;
        console.log('ok   ' + name);
    } catch (e) {
        failed++;
        console.log('FAIL ' + name + '\n' + (e && e.stack || e));
    }
}

test('sockets are the browser\'s own, with its constants', () => {
    const win = page();
    assert.notEqual(win.WebSocket, RealSocket);
    const a = new win.WebSocket('wss://lobby.test/relay');
    assert.ok(a instanceof RealSocket);
    assert.ok(a instanceof win.WebSocket);
    assert.equal(a.url, 'wss://lobby.test/relay');
    assert.equal(a.args, 1, 'no protocols given when none were');
    const b = new win.WebSocket('wss://lobby.test/feed/7', ['x']);
    assert.deepEqual(b.protocols, ['x']);
    assert.equal(win.WebSocket.OPEN, 1);
    assert.equal(win.WebSocket.CLOSED, 3);
});

test('going into the back/forward cache closes every socket still open; leaving for good doesn\'t need to', () => {
    const win = page();
    const a = new win.WebSocket('wss://lobby.test/relay');
    const b = new win.WebSocket('wss://lobby.test/feed/7');
    const c = new win.WebSocket('wss://lobby.test/feed/8');
    c.close();
    win.fire('pagehide', { persisted: false });
    assert.equal(a.closed + b.closed, 0);
    win.fire('pagehide', { persisted: true });
    assert.equal(a.closed, 1);
    assert.equal(b.closed, 1);
    assert.equal(c.closed, 1, 'one already closed is left alone');
    win.fire('pagehide', { persisted: true });
    assert.equal(a.closed, 1, 'only once');
});

test('a page come back to from the cache starts again; a page opened normally doesn\'t', () => {
    const win = page();
    win.fire('pageshow', { persisted: false });
    assert.equal(win.reloads, 0);
    win.fire('pageshow', { persisted: true });
    assert.equal(win.reloads, 1);
});

test('a browser without WebSocket is left as it is', () => {
    const win = { addEventListener() { assert.fail('listened'); } };
    new Function('window', 'location', source)(win, {});
    assert.equal(win.WebSocket, undefined);
});

console.log(failed ? `${failed} failed, ${passed} passed` : `all ${passed} passed`);
process.exit(failed ? 1 : 0);
