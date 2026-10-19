// What the settings page does when the device does not answer the first
// config request.
//
// Reported from the field: saving through the page "did not take on the first
// attempt". This exercises the path where the initial GET fails, which is the
// only way the form can be showing values the device never sent.
//
// API responses here are explicit fixtures, NOT a device.
// Run: NODE_PATH=/path/to/node_modules node tests/web/config_first_load.cjs
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const STORED = {
  display: { brightnessDay: 93, brightnessNight: 26, brightnessMode: 1,
    mainCenterValue: 2, peakEnabled: true, pageFuel: true, pageTemperature: true,
    pageDiagnostics: true, rotation: 0, startPage: 0, uiLanguage: 0,
    autoReturnSec: 10, manualNight: true, lightAutoCalibrate: true,
    lightNightAdc: 600, lightDayAdc: 3000, lightDimDelayMs: 3000,
    lightBrightenDelayMs: 1000, colorText: '#F6FAF6', colorVacuum: '#5ACAEE',
    colorBoost: '#5AF2AC', colorWarning: '#FFBE5A', colorDanger: '#FF5D6A',
    displaySpeedOffsetKph: 5 },
  boost: { baroSource: 0, fixedBaroKpa: 99.4, boostOffsetBar: 0, boostMinBar: -1,
    boostWarningBar: 0.9, boostDangerBar: 1.15, boostMaxBar: 1.5, smoothingMs: 250 },
  fuel: { fuelSource: 5, dfcoEnabled: true, fuelTrimEnabled: true, saveTrip: true,
    switchSpeedKph: 5, petrolCorrection: 1, lpgCorrection: 1.2, petrolAfr: 14.7,
    petrolDensity: 740, lpgAfr: 15.5, lpgDensity: 540 },
  lpg: {
    lpgEnabled: true, lpgActiveLow: true, lpgDebounceMs: 800,
    lpgOffDelayMs: 1000, lpgOnDelayMs: 10000,
  },
  controls: { longPressMs: 1800, serviceHoldMs: 5000, serviceTimeoutMin: 15,
    lockLongWhenMoving: false, screenOffDelaySec: 60 },
  service: { deviceName: 'H2 Gauge', apName: 'H2-Gauge', apPassword: 'h2gauge18' },
  obd: { obdTimeoutMs: 200, maxRequestsPerSecond: 50 },
};

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();
  const html = fs.readFileSync(path.join(__dirname, '../../web/index.html'), 'utf8');

  let configFails = true;
  let posts = 0, postBody = null;

  await page.route('http://h2g.test/**', async route => {
    const req = route.request();
    const url = new URL(req.url());
    if (url.pathname === '/') {
      return route.fulfill({ status: 200, contentType: 'text/html', body: html });
    }
    const json = (obj, status = 200) => route.fulfill({
      status, contentType: 'application/json', body: JSON.stringify(obj) });
    if (url.pathname === '/api/config') {
      if (req.method() === 'POST') {
        postBody = req.postDataJSON(); ++posts; return json({ ok: true });
      }
      if (configFails) return json({ error: 'busy' }, 503);
      return json(STORED);
    }
    if (url.pathname === '/api/status') {
      return json({ version: '0.4.15', engineEcuId: '0x7E8', screenOffReason: 'lit' });
    }
    return json({});
  });

  await page.goto('http://h2g.test/');
  await page.waitForFunction(() => document.getElementById('msg').textContent.length > 0);

  const message = await page.textContent('#msg');
  assert.match(message, /Нет ответа ESP32/,
    'the page must say the device did not answer');

  // The form is showing markup defaults, not what the device holds. Everything
  // the owner tuned is invisible right now.
  assert.equal(await page.inputValue('#displaySpeedOffsetKph'), '0',
    'with the device silent the form can only show what the markup carries');

  await page.click('.tab[data-tab="display"]');
  await page.waitForSelector('#displaySpeedOffsetKph', { state: 'visible' });
  await page.fill('#displaySpeedOffsetKph', '5');

  // The button is inert, so there is nothing to press and nowhere for a
  // factory value to escape through.
  assert.equal(await page.isDisabled('#save'), true,
    'saving must be impossible while the stored config is unknown');
  // Even a forced dispatch must not reach the device.
  await page.evaluate(() => document.getElementById('save').click());
  await page.waitForTimeout(400);
  assert.equal(posts, 0,
    'a form that never received the stored config must not be able to save it');
  assert.equal(postBody, null, 'nothing may be POSTed while the config is unknown');

  // Once the device answers, saving works as normal.
  configFails = false;
  await page.reload();
  await page.waitForFunction(() => document.getElementById('displaySpeedOffsetKph').value !== '');
  await page.click('.tab[data-tab="display"]');
  await page.waitForSelector('#displaySpeedOffsetKph', { state: 'visible' });
  assert.equal(await page.inputValue('#displaySpeedOffsetKph'), '5',
    'a good load must fill the form with the stored values');
  await page.click('#save');
  await page.waitForFunction(() => document.getElementById('msg').textContent.includes('Сохранено'));
  assert.equal(posts, 1);
  // Display-only since 0.4.19, so it belongs to display and not to the obd group
  // where the engine's own settings live.
  assert.equal(postBody.display.displaySpeedOffsetKph, 5);
  assert.equal(postBody.fuel.fuelSource, 5,
    'the source the device holds must not be overwritten with a factory guess');

  console.log('  [ok] a form that failed to load does not post factory values over stored ones');
  console.log('  [ok] saving resumes with the real values once the device answers');
  await browser.close();
})().catch(err => {
  console.error(err);
  process.exit(1);
});
