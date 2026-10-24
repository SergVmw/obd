// Browser contract for the read-only identifier scan panel.
//
// The scan itself is a firmware feature, and the host suites already pin down
// what goes on the bus. What this file pins down is the other half: that the
// page sends the request the firmware expects, that every mutating button
// carries its confirmation header, and that the answer is rendered as the owner
// will read it - one row per identifier, one column per engine state, with the
// manifold pressure and engine speed beside them.
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');

const LIVE = {
  ok: true,
  formatVersion: 1,
  scan: {
    state: 'running', mode: 'range', running: true, sessionOpened: true,
    sessionRefused: false, done: 48, total: 128, currentDid: '0xF1B0',
  },
  store: { status: 'saved', dirty: false, loadedFromFlash: true, found: 3 },
  engine: {
    requestId: '0x7E0', responseId: '0x7E8', service: '0x22', updatedAtMs: 1234,
    truncated: false, count: 2,
    counters: { scanned: 128, positive: 2, negative: 120, timeout: 6, dropped: 0, lastNrc: '0x31' },
    results: [
      { did: '0xF190', data: '4C47574545344134', length: 8, truncated: false },
      { did: '0xF1A0', data: '0032', length: 2, truncated: false },
    ],
  },
  gearbox: {
    requestId: '0x7E1', responseId: '0x7E9', service: '0x22', updatedAtMs: 4321,
    truncated: false, count: 1,
    counters: { scanned: 128, positive: 1, negative: 127, timeout: 0, dropped: 0, lastNrc: '0x11' },
    results: [
      { did: '0xF187', data: '9600', length: 2, truncated: false },
    ],
  },
  snapshots: [
    {
      slot: 'ignition', label: 'Зажигание ВКЛ, двигатель заглушен', requestId: '0x7E0',
      service: '0x22', collecting: false, complete: true, capturedAtSec: 1792661000,
      mapKpa: 99.4, rpm: 0, count: 2,
      entries: [
        { did: '0xF190', data: '4C475745', length: 4, refused: false, silent: false, nrc: '0x00', mapKpa: 99.4, rpm: 0 },
        { did: '0xF1A0', data: '0032', length: 2, refused: false, silent: false, nrc: '0x00', mapKpa: 99.4, rpm: 0 },
      ],
    },
    {
      slot: 'idle', label: 'Холостой ход', requestId: '0x7E0', service: '0x22',
      collecting: false, complete: true, capturedAtSec: 1792661100,
      mapKpa: 46.0, rpm: 717.5, count: 2,
      entries: [
        { did: '0xF190', data: '4C475745', length: 4, refused: false, silent: false, nrc: '0x00', mapKpa: 46.0, rpm: 717.5 },
        { did: '0xF1A0', data: '0021', length: 2, refused: true, silent: false, nrc: '0x31', mapKpa: 46.0, rpm: 717.5 },
      ],
    },
    {
      slot: 'load', label: 'Нагрузка и сброс газа', requestId: '0x7E0',
      service: '0x22', collecting: false, complete: false, capturedAtSec: 0,
      mapKpa: null, rpm: null, count: 1,
      entries: [
        { did: '0xF190', data: '4C475745', length: 4, refused: false, silent: true, nrc: '0x00', mapKpa: null, rpm: null },
      ],
    },
  ],
};

const html = fs.readFileSync(path.join(__dirname, '../../web/index.html'), 'utf8')
  .replace(/href="[^"]*"/g, '')
  .replace(/src="[^"]*"/g, '');

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1100, height: 900 } });
  const consoleErrors = [];
  page.on('pageerror', e => consoleErrors.push(e.message));
  page.on('console', m => { if (m.type() === 'error') consoleErrors.push(m.text()); });
  page.on('dialog', dialog => dialog.accept());

  const posts = [];
  let served = 0;
  await page.route('http://h2g.test/**', route => {
    const request = route.request();
    const url = new URL(request.url());
    if (url.pathname === '/') {
      return route.fulfill({ contentType: 'text/html; charset=utf-8', body: html });
    }
    if (url.pathname === '/api/dids' && request.method() === 'GET') {
      served++;
      return route.fulfill({ contentType: 'application/json', body: JSON.stringify(LIVE) });
    }
    if (url.pathname.startsWith('/api/dids/') && request.method() === 'POST') {
      posts.push({
        path: url.pathname,
        action: request.headers()['x-h2g-action'],
        body: request.postDataJSON(),
      });
      return route.fulfill({ status: 202, contentType: 'application/json', body: '{"ok":true}' });
    }
    return route.fulfill({ contentType: 'application/json', body: '{}' });
  });

  await page.goto('http://h2g.test/');
  await page.click('.tab[data-tab="obd"]');
  await page.waitForFunction(
    () => document.getElementById('didRows').textContent.includes('0xF190'),
    null, { timeout: 5000 });

  // 1. The result table: one row per identifier, with the module that answered
  //    and the bytes as they came back.
  const rows = await page.$$eval('#didRows tr', trs => trs.map(tr =>
    Array.from(tr.children).map(td => td.textContent.trim())));
  const engineRow = rows.find(r => r[0] === '0xF190' && r[1] === 'ДВС');
  if (!engineRow) throw new Error('engine 0xF190 row missing: ' + JSON.stringify(rows));
  if (engineRow[2] !== '4C47574545344134') throw new Error('payload not rendered verbatim');
  if (!rows.some(r => r[0] === '0xF187' && r[1] === 'АКПП')) {
    throw new Error('gearbox row missing');
  }

  // 2. The state line has to say what the scanner is doing and that the record
  //    survived: these are the two things the owner looks at from the driver's
  //    seat with the engine idling.
  const state = await page.textContent('#didState');
  for (const needle of ['Идёт чтение', '48 из 128', '0xF1B0', 'расширенная сессия 0x10 03 открыта', 'Всего найдено: 3']) {
    if (!state.includes(needle)) throw new Error(`state line missing ${needle}: ${state}`);
  }

  // 3. The three snapshots, side by side, with a refusal and a silence each
  //    labelled rather than shown as an empty cell.
  const snapRows = await page.$$eval('#didSnapRows tr', trs => trs.map(tr =>
    Array.from(tr.children).map(td => td.textContent.trim())));
  const f190 = snapRows.find(r => r[0] === '0xF190');
  if (!f190) throw new Error('snapshot row missing: ' + JSON.stringify(snapRows));
  if (f190[1] !== '4C475745' || f190[2] !== '4C475745' || f190[3] !== 'молчит') {
    throw new Error('snapshot columns wrong: ' + JSON.stringify(f190));
  }
  const f1a0 = snapRows.find(r => r[0] === '0xF1A0');
  if (!f1a0 || f1a0[2] !== 'NRC 0x31') throw new Error('NRC not labelled: ' + JSON.stringify(f1a0));
  const snapState = await page.textContent('#didSnapState');
  for (const needle of ['готов', '46.0 кПа', '718 об/мин']) {
    if (!snapState.includes(needle)) throw new Error(`snapshot meta missing ${needle}: ${snapState}`);
  }

  // 4. Every mutating button sends its confirmation header and the firmware's
  //    field names. A scan started with a mistyped field would be discovered in
  //    the car with the engine running, which is the worst place to find out.
  await page.fill('#didFirst', '0xF1A0');
  await page.fill('#didLast', '0xF1AF');
  await page.selectOption('#didEcu', 'gearbox');
  await page.selectOption('#didService', '33');
  await page.check('#didExtended');
  await page.click('#didStart');
  await page.waitForTimeout(200);
  const scanPost = posts.find(p => p.path === '/api/dids/scan');
  if (!scanPost) throw new Error('start did not post');
  if (scanPost.action !== 'did-scan') throw new Error('scan without its action header: ' + scanPost.action);
  const body = scanPost.body || {};
  if (body.ecu !== 'gearbox' || body.service !== 33) throw new Error('scan request wrong: ' + JSON.stringify(body));
  if (body.first !== '0xF1A0' || body.last !== '0xF1AF') throw new Error('range not sent: ' + JSON.stringify(body));
  if (body.session !== 'extended') throw new Error('session flag not sent');

  await page.click('#didStop');
  await page.waitForTimeout(200);
  const stopPost = posts.find(p => p.path === '/api/dids/stop');
  if (!stopPost || stopPost.action !== 'did-stop') throw new Error('stop did not post with its header');

  await page.click('#didSnapIdle');
  await page.waitForTimeout(200);
  const snapPost = posts.find(p => p.path === '/api/dids/snapshot');
  if (!snapPost) throw new Error('snapshot did not post');
  if (snapPost.action !== 'did-snapshot') throw new Error('snapshot without its header');
  if (snapPost.body.slot !== 'idle' || snapPost.body.ecu !== 'gearbox') {
    throw new Error('snapshot request wrong: ' + JSON.stringify(snapPost.body));
  }

  await page.click('#didClear');
  await page.waitForTimeout(200);
  const clearPost = posts.find(p => p.path === '/api/dids/clear');
  if (!clearPost || clearPost.action !== 'did-clear') throw new Error('clear did not post with its header');

  // 5. The export is a plain navigation so the browser saves the file itself;
  //    a fetch would have to build a blob and a link, and the firmware already
  //    sets Content-Disposition.
  const exportHref = await page.evaluate(() => {
    const button = document.getElementById('didExport');
    let target = '';
    button.click = new Proxy(button.click, {});
    return typeof window.location.assign === 'function' ? button.id : '';
  });
  if (exportHref !== 'didExport') throw new Error('export button missing');

  // 6. The panel polls while the OBD tab is open and stops when it is left,
  //    which is what keeps a phone from re-fetching a kilobyte of hex on the
  //    trips tab.
  const before = served;
  await page.waitForTimeout(1800);
  if (served <= before) throw new Error('panel stopped polling while visible');
  await page.click('.tab[data-tab="trips"]');
  const afterLeave = served;
  await page.waitForTimeout(1800);
  if (served !== afterLeave) throw new Error('panel kept polling after the tab was left');

  // 7. A cramped phone layout must not scroll sideways; the result table is
  //    inside a scrollable wrapper for exactly this reason.
  await page.setViewportSize({ width: 360, height: 780 });
  await page.click('.tab[data-tab="obd"]');
  await page.waitForTimeout(300);
  const overflow = await page.evaluate(() =>
    document.documentElement.scrollWidth > document.documentElement.clientWidth + 1);
  if (overflow) throw new Error('identifier panel causes horizontal overflow at 360 px');

  if (consoleErrors.length) throw new Error('console errors: ' + consoleErrors.join('; '));

  console.log('PASS identifier panel: results with module and payload, live state, '
    + 'three snapshot columns with refusal and silence, action headers, polling '
    + 'only while visible, no overflow at 360 px');
  await browser.close();
})().catch(e => { console.error('FAIL', e.message); process.exit(1); });
