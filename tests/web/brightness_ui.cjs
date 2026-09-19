// Browser contract tests; API responses here are explicit fixtures, NOT a device.
// Run: NODE_PATH=/path/to/node_modules node tests/web/brightness_ui.cjs
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

(async () => {
  const html = fs.readFileSync(path.join(__dirname, '../../web/index.html'), 'utf8');
  let saved = vm.runInNewContext('(' + html.match(/const DEFAULT_CONFIG=(.*);\n/)[1] + ')');
  let posts = 0, resetHeader = '', otaHeader = '', otaBytes = 0, offline = false;
  let otaLast = { result: 'applied', phase: 'boot_selected', resetReason: 'software', attempt: 1,
    sourceAddress: 0x410000, targetAddress: 0x10000, imageSize: 1080640,
    receivedSize: 1080640, descriptorVersion: 'fixture' };
  let sample = {
    rawAdc: 1830, filteredAdc: 1800, percent: 54, targetPercent: 54, pwmDuty: 138,
    nightAdc: 600, dayAdc: 3000, thresholdSource: 'configured', level: 'adaptive',
    delayRemainingMs: 0, pending: 'none', calibrationReady: false,
    observedMinAdc: 1790, observedMaxAdc: 1810, adcClipped: false, storageHealthy: true,
  };
  const launchOptions = { headless: true };
  if (process.env.H2G_CHROMIUM_EXECUTABLE) {
    launchOptions.executablePath = process.env.H2G_CHROMIUM_EXECUTABLE;
  }
  const browser = await chromium.launch(launchOptions);
  try {
    const page = await browser.newPage({ viewport: { width: 390, height: 844 } });
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    page.on('dialog', d => d.accept());
    await page.route('http://h2g.test/**', async route => {
      const req = route.request(), url = new URL(req.url());
      const json = body => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
      if (url.pathname === '/') return route.fulfill({ contentType: 'text/html', body: html });
      if (url.pathname === '/api/config') {
        if (req.method() === 'POST') { saved = req.postDataJSON(); ++posts; return json({ ok: true, brightnessAppliedLive: true }); }
        return json(saved);
      }
      if (url.pathname === '/api/status') {
        if (offline) return route.fulfill({ status: 503, body: 'test offline' });
        return json({ version: '0.3.7', target: 'ESP32-S3', obdConnected: false,
          boostBar: null, fuelMode: '95', ota: { runningPartition: 'app0', runningAddress: 0x10000,
            bootPartition: 'app0', bootAddress: 0x10000, nextPartition: 'app1', nextAddress: 0x410000,
            runningState: 'valid', resetReason: 'software', diagnosticsStorageHealthy: true,
            slots: [
              { partition: 'app0', address: 0x10000, descriptorReadable: true,
                versionKnown: true, version: '0.3.7', versionSource: 'manifest',
                running: true, bootSelected: true, nextUpdate: false, state: 'valid' },
              { partition: 'app1', address: 0x410000, descriptorReadable: true,
                versionKnown: true, version: '0.3.6', versionSource: 'known_legacy',
                running: false, bootSelected: false, nextUpdate: true, state: 'valid' },
            ],
            last: otaLast }, 
          brightness: { ...sample, mode: saved.display.brightnessMode,
            manualNight: saved.display.manualNight, calibrationLearning: saved.display.brightnessMode === 0 && saved.display.lightAutoCalibrate } });
      }
      if (url.pathname === '/api/brightness/calibration/reset') {
        resetHeader = req.headers()['x-h2g-action'];
        sample.observedMinAdc = sample.observedMaxAdc = null;
        return json({ ok: true });
      }
      if (url.pathname === '/api/ota') {
        otaHeader = req.headers()['x-h2g-action'];
        otaBytes = req.postDataBuffer().length;
        return json({ ok: true, verified: true, bootVerified: true, rebooting: true,
          bytes: otaBytes, sourcePartition: 'app0', targetPartition: 'app1' });
      }
      if (url.pathname === '/api/fuel/petrol-calibration') return json({ active: false, currentCorrection: 1 });
      return route.fulfill({ status: 404, body: 'Unexpected fixture URL: ' + url.pathname });
    });
    await page.goto('http://h2g.test/');
    await page.waitForFunction(() => document.getElementById('lightRaw').textContent === '1830');
    assert.equal(await page.locator('#otaFirmware').textContent(), '0.3.7');
    assert.equal(await page.locator('#slotCurrentVersion').textContent(), '0.3.7');
    assert.equal(await page.locator('#slotCurrentMeta').textContent(), 'app0 · запущена');
    assert.equal(await page.locator('#slotApp0Version').textContent(), '0.3.7');
    assert.match(await page.locator('#slotApp0Meta').textContent(), /запущен/);
    assert.equal(await page.locator('#slotApp0Card').getAttribute('class'), 'card slotCard running');
    assert.equal(await page.locator('#slotApp1Version').textContent(), '0.3.6');
    assert.match(await page.locator('#slotApp1Meta').textContent(), /следующий OTA/);
    assert.equal(await page.locator('#otaRunning').textContent(), 'app0');
    assert.equal(await page.locator('#otaBoot').textContent(), 'app0');
    assert.equal(await page.locator('#otaLast').textContent(), 'Применено');
    assert.match(await page.locator('#otaRuntime').textContent(), /Следующее обновление: app1@0x410000/);
    otaLast = { ...otaLast, result: 'interrupted_upload', phase: 'receiving',
      receivedSize: 1048576, imageSize: 1080640, resetReason: 'task_wdt' };
    await page.evaluate(() => status());
    assert.equal(await page.locator('#otaLast').textContent(), 'Загрузка прервана');
    assert.match(await page.locator('#otaRuntime').textContent(), /1048576 из 1080640 байт/);
    otaLast = { ...otaLast, result: 'interrupted_finalize', phase: 'image_verified', resetReason: 'task_wdt' };
    await page.evaluate(() => status());
    assert.equal(await page.locator('#otaLast').textContent(), 'Финализация прервана');
    assert.match(await page.locator('#otaRuntime').textContent(), /образ был проверен.*выбор boot-раздела не завершился/);
    assert.match(await page.locator('#otaRuntime').textContent(), /task_wdt/);
    otaLast = { ...otaLast, result: 'boot_selection_failed', phase: 'image_verified',
      resetReason: 'not_recorded', errorName: 'ESP_ERR_FLASH_OP_FAIL', errorCode: 261 };
    await page.evaluate(() => status());
    assert.equal(await page.locator('#otaLast').textContent(), 'Ошибка выбора boot');
    assert.match(await page.locator('#otaRuntime').textContent(), /ESP_ERR_FLASH_OP_FAIL/);
    otaLast = { ...otaLast, result: 'applied', phase: 'boot_selected', resetReason: 'software', errorName: 'none', errorCode: 0 };
    assert.equal(await page.locator('#brightnessMode').inputValue(), '2');
    assert.equal(await page.locator('#autoBrightnessFields').isVisible(), false);
    assert.equal(await page.locator('#manualBrightnessRow').isVisible(), false);
    await page.selectOption('#brightnessMode', '0');
    assert.equal(await page.locator('#autoBrightnessFields').isVisible(), true);
    await page.fill('#lightNightAdc', '700');
    await page.evaluate(() => status());
    assert.equal(await page.locator('#lightNightAdc').inputValue(), '700', 'live poll overwrote an unsaved field');
    await page.fill('#brightnessDay', '20');
    await page.fill('#brightnessNight', '26');
    await page.click('#save');
    assert.match(await page.locator('#msg').textContent(), /Ночная яркость/);
    assert.equal(posts, 0);
    await page.fill('#brightnessDay', '82');
    await page.fill('#lightDayAdc', '850');
    await page.click('#save');
    assert.match(await page.locator('#msg').textContent(), /минимум на 200/);
    assert.equal(posts, 0);
    await page.fill('#lightDayAdc', '3000');
    await page.fill('#lightNightAdc', '');
    await page.click('#save');
    assert.equal(posts, 0);
    await page.fill('#lightNightAdc', '700');
    await page.fill('#lightDimDelayMs', '4321');
    await page.click('#save');
    await page.waitForFunction(() => document.getElementById('msg').textContent.startsWith('Сохранено.'));
    assert.equal(posts, 1);
    assert.equal(saved.display.brightnessMode, 0);
    assert.equal(saved.display.lightNightAdc, 700);
    assert.equal(saved.display.lightDimDelayMs, 4321);
    assert.equal(saved.display.lightAutoCalibrate, true);
    await page.selectOption('#brightnessMode', '1');
    assert.equal(await page.locator('#manualBrightnessRow').isVisible(), true);
    assert.equal(await page.locator('#autoBrightnessFields').isVisible(), false);
    await page.check('#manualNight');
    await page.click('#save');
    await page.waitForFunction(() => document.getElementById('lightLive').textContent.includes('На приборе: Ручное'));
    assert.equal(saved.display.manualNight, true);
    await page.evaluate(() => load());
    assert.equal(await page.locator('#manualNight').isChecked(), true);
    await page.click('#resetLightCalibration');
    await page.waitForFunction(() => document.getElementById('msg').textContent.startsWith('Автокалибровка света сброшена.'));
    assert.equal(resetHeader, 'light-calibration-reset');
    assert.equal(saved.display.lightNightAdc, 700, 'reset changed configured thresholds');
    assert.equal(await page.locator('#resetLightCalibration').isDisabled(), true);
    sample.adcClipped = true; sample.storageHealthy = false;
    await page.evaluate(() => status());
    assert.match(await page.locator('#lightLive').textContent(), /ADC у границы/);
    assert.match(await page.locator('#lightLive').textContent(), /Ошибка NVS/);
    await page.click('button[data-tab="system"]');
    await page.locator('#firmware').setInputFiles({ name: 'h2-gauge-factory.bin', mimeType: 'application/octet-stream', buffer: Buffer.alloc(5000) });
    assert.equal(await page.locator('#install').isDisabled(), true);
    assert.match(await page.locator('#otaText').textContent(), /только app-образ/);
    await page.locator('#firmware').setInputFiles({ name: 'h2-gauge-v0.3.6-esp32s3-n16r8.bin', mimeType: 'application/octet-stream', buffer: Buffer.alloc(5000, 0xA5) });
    assert.equal(await page.locator('#install').isEnabled(), true);
    await page.click('#install');
    await page.waitForFunction(() => document.getElementById('otaText').textContent.includes('выбран app1'));
    assert.equal(otaHeader, 'ota');
    assert.equal(otaBytes, 5000);
    await page.click('button[data-tab="display"]');
    offline = true;
    await page.evaluate(() => status());
    assert.equal(await page.locator('#lightRaw').textContent(), '—');
    assert.equal(await page.locator('#slotCurrentVersion').textContent(), '—');
    assert.equal(await page.locator('#slotApp0Version').textContent(), '—');
    assert.match(await page.locator('#lightLive').textContent(), /Нет актуальных данных/);
    offline = false; sample.adcClipped = false; sample.storageHealthy = true;
    await page.selectOption('#brightnessMode', '0');
    await page.evaluate(() => status());
    for (const width of [360, 390, 768, 1280]) {
      await page.setViewportSize({ width, height: 900 });
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true, 'horizontal overflow at ' + width);
    }
    if (process.env.H2G_UI_TOP_SCREENSHOT) {
      await page.setViewportSize({ width: 390, height: 844 });
      await page.evaluate(() => scrollTo(0, 0));
      await page.screenshot({ path: process.env.H2G_UI_TOP_SCREENSHOT });
    }
    if (process.env.H2G_UI_SCREENSHOT) {
      await page.setViewportSize({ width: 768, height: 1000 });
      await page.evaluate(() => document.fonts.ready);
      await page.locator('#brightnessSettings').scrollIntoViewIfNeeded();
      await page.screenshot({ path: process.env.H2G_UI_SCREENSHOT });
    }
    assert.deepEqual(errors, []);
    console.log('PASS browser: brightness/live data, slot cards, interrupted-upload progress, verified OTA UI, validation/reset, stale-state clearing, 360–1280 px layouts');
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
