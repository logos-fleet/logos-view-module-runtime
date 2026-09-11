// THE ONE CHECK THAT NEEDS A BROWSER.
//
// Everything else about the QML runtime is checked without one: the transport
// and the bridge on the desktop against a real QRemoteObjectHost and a real
// QQmlEngine (tests/), and the image itself by reading symbols back off the
// link (nix/wasm.nix). What none of that can reach is the class of failure a
// STATIC Qt keeps to itself — a QML module whose plugin was never linked. The
// library is in the image, the build succeeds, the runtime boots and paints its
// own shell, and the first module document that imports the missing module
// fails to instantiate with `plugin "…" not found`. Only running it finds that,
// and it found it three times while this file was being written:
//
//   1. `QGuiApplication::exec()` RETURNS in a wasm image, so an engine on
//      main()'s stack is destroyed before the page's first call arrives;
//   2. `import QtCore` (Logos.Theme persists the theme with Settings) needed
//      Qt6::QmlCore linked AND named in an import the build could see;
//   3. `return 1` from main() aborts the emscripten runtime, after which every
//      call from the page throws a bare pointer — including the one that would
//      have asked what went wrong.
//
// NOT A NIX CHECK, and it cannot become one: the nix sandbox has no browser and
// darwin has no chromium in nixpkgs. Run it by hand after a runtime build, and
// on any venue with a Chrome:
//
//     nix build .#qml-runtime-wasm
//     node wasm/runtime/browser-smoke/run.mjs result/www
//
// It serves the built `www/` over http (a `file://` page cannot fetch a sibling
// .wasm), drives it with headless Chrome, and the page posts its verdict back
// to the same server. Exit 0 is every check passing.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { join, extname, normalize } from 'node:path';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';

const www = process.argv[2];
if (!www) {
  console.error('usage: node run.mjs <dir with logos_qml_runtime.{html,js,wasm} and qtloader.js>');
  process.exit(2);
}
const here = fileURLToPath(new URL('.', import.meta.url));

// Chrome is not a dependency anything declares, so it is named rather than
// found: a venue without one should say so, not fail obscurely.
const CHROME = process.env.LOGOS_CHROME
  || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';

const TIMEOUT_MS = Number(process.env.LOGOS_SMOKE_TIMEOUT_MS || 180000);

const MIME = {
  '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm',
  '.svg': 'image/svg+xml', '.json': 'application/json',
};

let settle;
const verdict = new Promise((resolve) => { settle = resolve; });

const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1');
  if (url.pathname === '/result') {
    let body = '';
    for await (const chunk of req) body += chunk;
    res.writeHead(204).end();
    settle(body);
    return;
  }
  // The smoke page first, the build output second: the page is served from this
  // directory and everything it loads from the build's. `normalize` keeps a
  // request for `/../something` inside the two directories that are on offer.
  const name = normalize(url.pathname === '/' ? '/smoke.html' : url.pathname);
  for (const dir of [here, www]) {
    try {
      const data = await readFile(join(dir, name));
      res.writeHead(200, { 'Content-Type': MIME[extname(name)] || 'application/octet-stream' })
         .end(data);
      return;
    } catch (e) { /* try the next directory */ }
  }
  res.writeHead(404).end('not found');
});

server.listen(0, '127.0.0.1', () => {
  const port = server.address().port;
  const profile = mkdtempSync(join(tmpdir(), 'logos-qml-runtime-smoke-'));

  // --enable-unsafe-swiftshader: Qt for WebAssembly needs WebGL2, and a
  // headless Chrome has no GPU. Everything else is noise suppression.
  const chrome = spawn(CHROME, [
    '--headless=new',
    '--no-sandbox',
    '--disable-dev-shm-usage',
    '--enable-unsafe-swiftshader',
    '--use-gl=angle',
    '--use-angle=swiftshader',
    '--window-size=480,640',
    '--no-first-run',
    '--no-default-browser-check',
    '--disable-background-networking',
    '--disable-component-update',
    `--user-data-dir=${profile}`,
    `http://127.0.0.1:${port}/smoke.html`,
  ], { stdio: ['ignore', 'pipe', 'pipe'] });

  let chromeErr = '';
  chrome.stderr.on('data', (d) => { chromeErr += d; });
  chrome.on('error', (e) => settle(JSON.stringify({
    ok: false, error: `could not start ${CHROME}: ${e.message} (set LOGOS_CHROME)`,
  })));

  const timer = setTimeout(() => settle(JSON.stringify({
    ok: false,
    error: `the page did not report within ${TIMEOUT_MS} ms`,
    chromeErr: chromeErr.slice(-2000),
  })), TIMEOUT_MS);

  verdict.then((body) => {
    clearTimeout(timer);
    try { chrome.kill(); } catch (e) { /* already gone */ }
    server.close();

    let result;
    try { result = JSON.parse(body); }
    catch (e) { result = { ok: false, error: `unparseable verdict: ${body}` }; }

    for (const c of result.checks || [])
      console.log(`${c.ok ? 'PASS' : 'FAIL'}  ${c.name}${c.detail ? '  — ' + c.detail : ''}`);
    for (const line of result.logs || [])
      console.log(`      ${line.trimEnd()}`);
    if (result.error) console.log(`\n${result.error}`);
    if (!result.ok && chromeErr) console.log(`\n--- chrome stderr ---\n${chromeErr.slice(-3000)}`);
    console.log(`\n${result.ok ? 'PASS' : 'FAIL'}: logos-qml-runtime browser smoke`);
    process.exit(result.ok ? 0 : 1);
  });
});
