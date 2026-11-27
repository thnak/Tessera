// SPDX-License-Identifier: MIT
//
// Runs webgpu_path_raster_web (the wasm-webgpu preset's browser build of the GPU path rasterizer
// conformance and 4K measurement, issue #685) in a real Chrome, headless, and prints what the
// program printed. Dev tooling, not shipped.
//
//   cmake --build build/wasm-webgpu --target webgpu_path_raster_web
//   node tools/webgpu_browser/run_path_raster.mjs [--chrome <exe>] [--flags a,b] -- <program args>
//
// A local server on 127.0.0.1 serves the build output and the page; the page POSTs the result. The
// Chrome process and its children are killed at the end. Exit code: the program's.

import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';

const argv = process.argv.slice(2);
let chrome = 'C:/Program Files/Google/Chrome/Application/chrome.exe';
let extra = [];
let timeoutMs = 240000;
const cut = argv.indexOf('--');
const own = cut < 0 ? argv : argv.slice(0, cut);
const programArgs = cut < 0 ? [] : argv.slice(cut + 1);
for (let i = 0; i < own.length; i++) {
    if (own[i] === '--chrome') chrome = own[++i];
    else if (own[i] === '--flags') extra = own[++i].split(',').filter(Boolean);
    else if (own[i] === '--timeout') timeoutMs = Number(own[++i]);
}
const buildDir = path.resolve('build/wasm-webgpu/src/rendering/backend/webgpu');
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const types = { '.js': 'text/javascript', '.wasm': 'application/wasm', '.html': 'text/html' };

let resolveResult;
const result = new Promise((r) => (resolveResult = r));
const server = http.createServer((req, res) => {
    if (req.method === 'POST' && req.url === '/result') {
        let body = '';
        req.on('data', (c) => (body += c));
        req.on('end', () => { res.end('ok'); resolveResult(JSON.parse(body)); });
        return;
    }
    const name = req.url.split('?')[0] === '/index.html' ? path.join(here, 'page.html')
        : path.join(buildDir, path.basename(req.url.split('?')[0]));
    if (!fs.existsSync(name)) { res.statusCode = 404; return res.end('missing'); }
    // Cross-origin isolation lifts the 100 us coarsening of performance.now to 5 us.
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    res.setHeader('Content-Type', types[path.extname(name)] ?? 'application/octet-stream');
    res.end(fs.readFileSync(name));
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'tessera-chrome-'));
const url = `http://127.0.0.1:${port}/index.html?args=${encodeURIComponent(programArgs.join(','))}`;
const child = spawn(chrome, ['--headless=new', '--enable-unsafe-webgpu', '--no-first-run',
    '--no-default-browser-check', `--user-data-dir=${profile}`,
    '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
    '--disable-backgrounding-occluded-windows', ...extra, url], { stdio: 'ignore' });
let code = 5;
try {
    const timer = new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), timeoutMs));
    const r = await Promise.race([result, timer]);
    console.log('user agent:', r.ua);
    console.log(r.lines.join('\n'));
    code = r.code;
} catch (e) {
    console.error('run_path_raster:', e.message);
} finally {
    spawnSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], { stdio: 'ignore' });
    server.close();
    try { fs.rmSync(profile, { recursive: true, force: true }); } catch {}
}
process.exit(code);
