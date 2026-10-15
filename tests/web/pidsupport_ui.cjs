// Exercises the support-mask panel against a captured API response, so the
// rendering is checked rather than assumed.
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');

const SAMPLE = {
  responderCount: 2, maxResponders: 8,
  note: 'n/a',
  responders: [
    { id: '0x7E8', address: 2024, groups: [
      { base: '0x00', known: true, mask: '41 9D F1 80', count: 21,
        polledCount: 8,
        polledPids: ['0x01 Monitor status / MIL / DTC count','0x03 Calculated engine load','0x05 Engine coolant temperature','0x0B Intake manifold absolute pressure','0x0C Engine speed (RPM)','0x0D Vehicle speed','0x0F Intake air temperature','0x11 Absolute throttle position'],
        pids: ['0x01 Monitor status / MIL / DTC count','0x03 Calculated engine load','0x05 Engine coolant temperature','0x0B Intake manifold absolute pressure','0x0C Engine speed (RPM)','0x0D Vehicle speed','0x0F Intake air temperature','0x11 Absolute throttle position','0x1C Status of MONO or group injection','0x1F Run time since engine start'] },
      { base: '0x20', known: true, mask: '00 00 00 00', count: 0, polledCount: 0, polledPids: [], pids: [] },
      { base: '0x40', known: true, mask: '21 00 01 80', count: 5, polledCount: 3,
        polledPids: ['0x42 Control module voltage','0x44 Commanded equivalence ratio','0x5E Fuel rate engine'],
        pids: ['0x42 Control module voltage','0x44 Commanded equivalence ratio','0x45 Relative throttle position','0x5E Fuel rate engine','0x5F Engine fuel temperature'] },
    ]},
    { id: '0x7E9', address: 2025, groups: [
      { base: '0x00', known: true, mask: '00 08 00 00', count: 1, polledCount: 1,
        polledPids: ['0x0D Vehicle speed'],
        pids: ['0x0D Vehicle speed'] },
      { base: '0x20', known: false, mask: '00 00 00 00', count: 0, polledCount: 0, polledPids: [], pids: [] },
      { base: '0x40', known: false, mask: '00 00 00 00', count: 0, polledCount: 0, polledPids: [], pids: [] },
    ]},
  ],
  union: [ { base: '0x00', known: true, sources: 2, mask: '41 9D F1 80' } ],
};

const html = fs.readFileSync(path.join(__dirname, '../../web/index.html'), 'utf8')
  .replace(/href="[^"]*"/g, '')
  .replace(/src="[^"]*"/g, '');

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1100, height: 900 } });
  let served = 0;
  // A real origin is required: the panel fetches a relative path, and about:blank
  // has no base to resolve it against.
  await page.route('http://h2g.test/**', route => {
    const p = new URL(route.request().url()).pathname;
    if (p === '/') return route.fulfill({ contentType: 'text/html; charset=utf-8', body: html });
    if (p === '/api/pid-support') { served++; return route.fulfill({ contentType: 'application/json', body: JSON.stringify(SAMPLE) }); }
    return route.fulfill({ contentType: 'application/json', body: '{}' });
  });
  await page.goto('http://h2g.test/');
  await page.click('.tab[data-tab="diag"]');
  await page.waitForFunction(() => document.getElementById('pidSupportBox').textContent.includes('0x7E8'), null, { timeout: 5000 });
  // The panel writes each PID name with non-breaking spaces so a name cannot
  // wrap in the middle. textContent returns what the browser rendered, which
  // means those come back as U+00A0 and a plain-space comparison would never
  // match a multi-word name. Normalise before comparing.
  const text = (await page.locator('#pidSupportBox').textContent())
    .replace(/ /g, ' ');
  for (const needle of ['0x7E8', '0x7E9', '41 9D F1 80', 'Calculated engine load', 'Vehicle speed', 'не ответил', '2 из 8', 'опрос: 8']) {
    if (!text.includes(needle)) throw new Error('missing from panel: ' + needle);
  }
  // A polled PID must be marked, and one that is merely supported must not be.
  // The panel substitutes non-breaking spaces, so compare against the rendered
  // text with those folded back to plain spaces.
  const marked = (await page.locator('#pidSupportBox b').allTextContents())
    .map(t => t.replace(/ /g, ' '));
  if (!marked.some(t => t.includes('Engine speed'))) throw new Error('polled PID is not marked');
  for (const unpolled of ['Engine fuel temperature', 'Relative throttle position', 'Run time since engine start']) {
    if (marked.some(t => t.includes(unpolled))) throw new Error('unpolled PID is marked as polled: ' + unpolled);
  }
  if (served < 1) throw new Error('panel never fetched /api/pid-support');
  await page.click('#pidSupportRefresh');
  await page.waitForTimeout(200);
  if (served < 2) throw new Error('manual refresh did not refetch');
  console.log('PASS support-mask panel: per-block listing, masks, names, non-answering groups and refetch');
  await browser.close();
})().catch(e => { console.error('FAIL', e.message); process.exit(1); });
