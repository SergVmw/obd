// Config round-trip contract test. API responses here are explicit fixtures,
// NOT a device.
//
// Written because saving a setting through the page was reported as not taking
// on the first attempt. The route below stores what the page POSTs and hands it
// back on the next GET, so a value that fails to survive the trip shows up as a
// mismatch rather than as a shrug from the driver.
//
// Run: NODE_PATH=/path/to/node_modules node tests/web/config_roundtrip.cjs
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const DEVICE = {
  display: {
    brightnessDay: 93, brightnessNight: 26, brightnessMode: 1,
    manualNight: true, lightAutoCalibrate: true, lightNightAdc: 600,
    lightDayAdc: 3000, lightDimDelayMs: 3000, lightBrightenDelayMs: 1000,
    rotation: 0, startPage: 0, mainCenterValue: 2, uiLanguage: 0,
    autoReturnSec: 10, peakEnabled: true, pageFuel: true, pageTemperature: true,
    pageDiagnostics: true, colorText: '#F6FAF6', colorVacuum: '#5ACAEE',
    colorBoost: '#5AF2AC', colorWarning: '#FFBE5A', colorDanger: '#FF5D6A',
  },
  boost: {
    baroSource: 0, fixedBaroKpa: 99.4, boostOffsetBar: 0, boostMinBar: -1,
    boostWarningBar: 0.9, boostDangerBar: 1.15, boostMaxBar: 1.5,
    smoothingMs: 250,
  },
  fuel: {
    fuelSource: 0, dfcoEnabled: true, fuelTrimEnabled: true, saveTrip: true,
    switchSpeedKph: 5, petrolCorrection: 1, lpgCorrection: 1.2,
    petrolAfr: 14.7, petrolDensity: 740, lpgAfr: 15.5, lpgDensity: 540,
  },
  lpg: { lpgEnabled: true, lpgActiveLow: true, lpgDebounceMs: 800, lpgOffDelayMs: 1000 },
  controls: {
    longPressMs: 1800, serviceHoldMs: 5000, serviceTimeoutMin: 15,
    lockLongWhenMoving: true, screenOffDelaySec: 60,
  },
  service: { deviceName: 'H2 Gauge', apName: 'H2-Gauge', apPassword: 'h2gauge18' },
  obd: { obdTimeoutMs: 120, maxRequestsPerSecond: 50 },
};

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();
  const html = fs.readFileSync(path.join(__dirname, '../../web/index.html'), 'utf8');

  let stored = JSON.parse(JSON.stringify(DEVICE));
  let posts = 0, postBody = null, message = '';

  await page.route('http://h2g.test/**', async route => {
    const req = route.request();
    const url = new URL(req.url());
    if (url.pathname === '/') {
      return route.fulfill({ status: 200, contentType: 'text/html', body: html });
    }
    const json = (obj, status = 200) => route.fulfill({
      status, contentType: 'application/json', body: JSON.stringify(obj),
    });
    if (url.pathname === '/api/config') {
      if (req.method() === 'POST') {
        postBody = req.postDataJSON();
        ++posts;
        stored = postBody;
        return json({ ok: true, brightnessAppliedLive: true, rebootRecommended: true });
      }
      return json(stored);
    }
    if (url.pathname === '/api/status') {
      return json({ version: '0.4.15', engineEcuId: '0x7E8', engineEcuLocked: true,
        screenOffReason: 'lit', fuelValueValid: true, rpm: 800, speedKph: 0 });
    }
    return json({});
  });

  // A first attempt is exactly this: the page opens, the driver changes one
  // field and presses save without reloading anything.
  await page.goto('http://h2g.test/');
  await page.waitForFunction(() => document.getElementById('fuelSource').value !== '');

  assert.equal(await page.inputValue('#fuelSource'), '0',
    'the page must show what the device holds, not a factory guess');

  // Settings live behind tabs, so the driver picks a tab before touching the
  // field. Reproducing that is the point: the switch is part of the attempt.
  await page.click('.tab[data-tab="fuel"]');
  await page.waitForSelector('#fuelSource', { state: 'visible' });
  await page.selectOption('#fuelSource', '5');
  await page.click('#save');
  await page.waitForFunction(() => document.getElementById('msg').textContent.includes('Сохранено'));

  assert.equal(posts, 1, 'one press must be one request');
  assert.equal(postBody.fuel.fuelSource, 5,
    'the posted body must carry the choice that was just made');

  // Nothing else may drift while that one field is being changed.
  assert.equal(postBody.boost.boostMinBar, -1);
  assert.equal(postBody.boost.boostWarningBar, 0.9);
  assert.equal(postBody.boost.boostDangerBar, 1.15);
  assert.equal(postBody.boost.boostMaxBar, 1.5);
  assert.equal(postBody.display.brightnessDay, 93);
  assert.equal(postBody.controls.screenOffDelaySec, 60);

  // And it has to still be there after a reload, which is where "did not take"
  // would otherwise be discovered.
  await page.reload();
  await page.waitForFunction(() => document.getElementById('fuelSource').value !== '');
  assert.equal(await page.inputValue('#fuelSource'), '5',
    'the saved source must survive a reload');

  // A numeric field, the other shape the form posts, behind its own tab. Since
  // 0.4.19 this one is display-only and sits with the display settings.
  await page.click('.tab[data-tab="display"]');
  await page.waitForSelector('#displaySpeedOffsetKph', { state: 'visible' });
  await page.fill('#displaySpeedOffsetKph', '5');
  await page.click('#save');
  await page.waitForFunction(() => document.getElementById('msg').textContent.includes('Сохранено'));
  assert.equal(postBody.display.displaySpeedOffsetKph, 5);
  assert.equal(postBody.obd.speedCorrectionKph, undefined,
    'a display-only setting must not appear under the obd group');
  assert.equal(postBody.fuel.fuelSource, 5,
    'changing one number must not roll a previously saved choice back');

  await page.reload();
  await page.waitForFunction(() => document.getElementById('displaySpeedOffsetKph').value !== '');
  assert.equal(await page.inputValue('#displaySpeedOffsetKph'), '5');

  console.log('  [ok] a field set on the first attempt is what gets saved');
  console.log('  [ok] unrelated settings are not dragged along');
  console.log('  [ok] saved values survive a reload');
  await browser.close();
})().catch(err => {
  console.error(err);
  process.exit(1);
});
