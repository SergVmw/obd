// Exercise the diagnostic-only download button with explicit API fixtures.
// The fixtures are not device telemetry; this tests the export workflow only.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const zlib = require('node:zlib');

const header = fs.readFileSync(path.join(__dirname, '../../src/diagnostic_web_ui_gz.h'), 'utf8');
const compressed = Buffer.from([...header.matchAll(/0x([0-9a-fA-F]{2})/g)].map(m => parseInt(m[1], 16)));
const html = zlib.gunzipSync(compressed).toString('utf8');
const start = html.indexOf('async function downloadDiagnosticBundle()');
const end = html.indexOf('function set(id,value){', start);
assert.ok(start >= 0 && end > start, 'diagnostic export script markers exist');
const exportScript = html.slice(start, end);

function makeHarness(apiImpl) {
  const button = {
    disabled: false,
    handler: null,
    addEventListener(name, callback) {
      assert.equal(name, 'click');
      this.handler = callback;
    },
  };
  const status = { textContent: '' };
  const calls = [];
  const downloads = [];
  const timers = [];
  class TestBlob {
    constructor(parts, options) {
      this.parts = parts;
      this.type = options.type;
    }
  }
  const document = {
    createElement(tag) {
      assert.equal(tag, 'a');
      return {
        href: '', download: '', clicked: false, removed: false,
        click() { this.clicked = true; },
        remove() { this.removed = true; },
      };
    },
    body: { appendChild(link) { downloads.push(link); } },
  };
  const context = {
    $: id => id === 'diagnosticExport' ? button : status,
    api: async url => { calls.push(url); return apiImpl(url); },
    Blob: TestBlob,
    URL: {
      createObjectURL(blob) { context.lastBlob = blob; return 'blob:diagnostic-test'; },
      revokeObjectURL() {},
    },
    document,
    setTimeout(callback, delay) { timers.push({ callback, delay }); return 1; },
    lastBlob: null,
  };
  vm.runInNewContext(exportScript, context);
  return { button, status, calls, downloads, timers, context };
}

(async () => {
  const statusFixture = { version: '0.4.3-diag.1', target: 'esp32s3-n16r8-diag' };
  const traceFixture = { schema: 'h2-obd-trace-v1', events: [{ reasonFlags: 1 }] };
  const success = makeHarness(async url => url === '/api/status' ? statusFixture : traceFixture);
  assert.equal(typeof success.button.handler, 'function');
  await success.button.handler();
  assert.deepEqual(success.calls, ['/api/status', '/api/diagnostics/trace']);
  assert.equal(success.button.disabled, false);
  assert.equal(success.context.lastBlob.type, 'application/json');
  const bundle = JSON.parse(success.context.lastBlob.parts[0]);
  assert.equal(bundle.schema, 'h2-gauge-diagnostic-bundle-v1');
  assert.deepEqual(bundle.currentStatus, statusFixture);
  assert.deepEqual(bundle.trace, traceFixture);
  assert.equal(success.downloads.length, 1);
  assert.match(success.downloads[0].download, /^h2-gauge-diagnostic-.*\.json$/);
  assert.equal(success.downloads[0].clicked, true);
  assert.equal(success.downloads[0].removed, true);
  assert.match(success.status.textContent, /событий: 1/);

  const failure = makeHarness(async url => {
    if (url === '/api/status') return statusFixture;
    throw new Error('trace unavailable');
  });
  await failure.button.handler();
  assert.equal(failure.button.disabled, false);
  assert.equal(failure.downloads.length, 0);
  assert.match(failure.status.textContent, /Ошибка выгрузки: trace unavailable/);

  console.log('Diagnostic JSON export UI: success bundle/download and API error handling OK');
})().catch(error => { console.error(error); process.exit(1); });
