// The tab's icon in packaging/wasm/index.html: in the page itself, so
// browsers don't ask the site for /favicon.ico (a 404 on every first visit,
// and a request that runs the lobby's Worker), drawn in the page's own
// colors. In Node: node packaging/wasm/test/icon-test.mjs

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const head = html.substring(0, html.indexOf('</head>'));
const links = [...head.matchAll(/<link rel="icon" href="([^"]+)">/g)];
assert.equal(links.length, 1, 'one icon, in the head');
const url = links[0][1];
assert.ok(url.startsWith('data:image/svg+xml,'), 'an SVG data: address');
assert.ok(!/[\s"<>#]/.test(url), 'nothing in it that needs escaping in an address');
const svg = decodeURIComponent(url.substring('data:image/svg+xml,'.length));
assert.match(svg, /^<svg xmlns='http:\/\/www\.w3\.org\/2000\/svg' viewBox='0 0 16 16'>.*<\/svg>$/);
assert.ok(!/<(script|image|text|use|foreignObject)\b|href=/.test(svg), 'shapes only');
// Every color is one the page's styles define (:root).
const palette = new Set([...html.matchAll(/--[a-z-]+: (#[0-9a-f]{6});/g)].map(m => m[1]));
const colors = [...svg.matchAll(/fill='(#[0-9a-f]{6})'/g)].map(m => m[1]);
assert.ok(colors.length >= 2);
colors.forEach(c => assert.ok(palette.has(c), c + ' is in the page\'s palette'));
assert.ok(url.length < 400, 'small: ' + url.length + ' characters');
console.log('all 1 passed');
