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
  let posts = 0, resetHeader = '', otaHeader = '', otaPreflightHeader = '';
  let otaBytes = 0, otaPreflights = 0, otaBehavior = 'jsonError';
  let otaPreflightReject = true, otaStatusError = null, offline = false;
  const assetPreflights = {}, assetBodies = {}, assetHeaders = {};
  let assetGeneration = 0;
  const assetState = {
    background: { present: false, enabled: false, valid: false },
    logo: { present: false, enabled: false, valid: false },
  };
  const crc32 = data => {
    let crc = 0xffffffff;
    for (const value of data) {
      crc ^= value;
      for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
    }
    return (crc ^ 0xffffffff) >>> 0;
  };
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
        return json({ version: '0.3.9', target: 'ESP32-S3', obdConnected: false,
          boostBar: null, fuelMode: '95', ota: { runningPartition: 'app0', runningAddress: 0x10000,
            maxImageBytes: 2097152, probeBytes: 288, idleTimeoutMs: 2000, totalTimeoutMs: 180000,
            bootPartition: 'app0', bootAddress: 0x10000, nextPartition: 'app1', nextAddress: 0x410000,
            runningState: 'valid', resetReason: 'software', diagnosticsStorageHealthy: true,
            slots: [
              { partition: 'app0', address: 0x10000, descriptorReadable: true,
                versionKnown: true, version: '0.3.9', versionSource: 'manifest',
                running: true, bootSelected: true, nextUpdate: false, state: 'valid' },
              { partition: 'app1', address: 0x410000, descriptorReadable: true,
                versionKnown: true, version: '0.3.6', versionSource: 'known_legacy',
                running: false, bootSelected: false, nextUpdate: true, state: 'valid' },
            ],
            last: otaLast }, 
          brightness: { ...sample, mode: saved.display.brightnessMode,
            manualNight: saved.display.manualNight, calibrationLearning: saved.display.brightnessMode === 0 && saved.display.lightAutoCalibrate },
          persistence: { littlefsMounted: true, littlefsStatus: 'mounted', journalHealthy: true,
            nvsHealthy: true, latestSequence: 44, journalSequence: 44, nvsSequence: 43,
            journalWrites: 5, nvsWrites: 2, journalFailures: 0, nvsFailures: 0,
            lastJournalWriteAgeMs: 3700, lastNvsWriteAgeMs: 24100,
            latestRecordCrcValid: true, recoverySource: 'littlefs_journal',
            activeSegment: 'A', activeSegmentBytes: 700, activeTailClean: true,
            rotationPending: false } });
      }
      if (url.pathname === '/api/assets/status') return json({
        ok: true, filesystemMounted: true, filesystemStatus: 'mounted', storageHealthy: true,
        limits: { sourceImageMaxBytes: 8388608, backgroundWidth: 240, backgroundHeight: 240,
          backgroundPayloadBytes: 115200, logoMaxWidth: 220, logoMaxHeight: 80,
          logoMaxPayloadBytes: 35200, pixelFormat: 'RGB565_LE', uploadTimeoutMs: 30000 },
        background: assetState.background, logo: assetState.logo,
      });
      if (url.pathname === '/api/assets/preflight') {
        assert.equal(req.headers()['x-h2g-action'], 'asset-upload');
        const body = req.postDataJSON();
        assert.ok(body.type === 'background' || body.type === 'logo');
        assert.equal(body.payloadBytes, body.width * body.height * 2);
        if (body.type === 'background') {
          assert.deepEqual([body.width, body.height, body.payloadBytes], [240, 240, 115200]);
        } else {
          assert.ok(body.width <= 220 && body.height <= 80 && body.width > 0 && body.height > 0);
        }
        assetPreflights[body.type] = body;
        return json({ accepted: true, ...body, generation: ++assetGeneration,
          bank: assetGeneration % 2 ? 'A' : 'B', timeoutMs: 30000 });
      }
      const rawAsset = url.pathname.match(/^\/api\/assets\/(background|logo)$/);
      if (rawAsset && req.method() === 'POST') {
        const type = rawAsset[1], body = req.postDataBuffer(), plan = assetPreflights[type];
        assert.ok(plan, 'raw asset upload must follow matching preflight');
        assert.equal(req.headers()['content-type'], 'application/octet-stream');
        assert.equal(req.headers()['x-h2g-action'], type === 'logo' ? 'asset-logo' : 'asset-background');
        assert.equal(body.length, plan.payloadBytes);
        assert.equal(crc32(body), plan.crc32);
        assetBodies[type] = body;
        assetHeaders[type] = req.headers();
        assetState[type] = { present: true, enabled: true, valid: true,
          width: plan.width, height: plan.height, payloadBytes: body.length,
          crc32: plan.crc32, generation: assetGeneration, bank: assetGeneration % 2 ? 'A' : 'B', error: 'none' };
        return json({ ok: true, verified: true, enabled: true, width: plan.width,
          height: plan.height, payloadBytes: body.length, crc32: plan.crc32,
          generation: assetGeneration, rebootRequired: true });
      }
      const assetMode = url.pathname.match(/^\/api\/assets\/(background|logo)\/enabled$/);
      if (assetMode && req.method() === 'POST') {
        assert.equal(req.headers()['x-h2g-action'], 'asset-settings');
        assetState[assetMode[1]].enabled = req.postDataJSON().enabled;
        return json({ ok: true, rebootRequired: true });
      }
      const assetDelete = url.pathname.match(/^\/api\/assets\/(background|logo)\/delete$/);
      if (assetDelete && req.method() === 'POST') {
        assert.equal(req.headers()['x-h2g-action'], 'asset-delete');
        assetState[assetDelete[1]] = { present: false, enabled: false, valid: false };
        return json({ ok: true, rebootRequired: true });
      }
      if (url.pathname === '/api/brightness/calibration/reset') {
        resetHeader = req.headers()['x-h2g-action'];
        sample.observedMinAdc = sample.observedMaxAdc = null;
        return json({ ok: true });
      }
      if (url.pathname === '/api/ota/preflight') {
        otaPreflightHeader = req.headers()['x-h2g-action'];
        const body = req.postDataJSON();
        ++otaPreflights;
        assert.equal(body.filename, 'h2-gauge-v0.3.9-esp32s3-n16r8.bin');
        assert.equal(body.size, 5000);
        assert.equal(body.probeHex.length, 576);
        assert.match(body.probeHex, /^(a5)+$/);
        if (otaPreflightReject) {
          otaPreflightReject = false;
          return route.fulfill({ status: 422, contentType: 'application/json', body: JSON.stringify({
            ok: false, status: 422, code: 'invalid_image',
            error: 'Firmware image is not for ESP32-S3' }) });
        }
        return json({ ok: true, accepted: true, imageSize: body.size,
          maxImageBytes: 2097152, targetPartition: 'app1', timeoutMs: 180000 });
      }
      if (url.pathname === '/api/ota/status') {
        return json({ ok: true, inProgress: false, error: '', code: '',
          minImageBytes: 4096, maxImageBytes: 2097152, probeBytes: 288,
          timeoutMs: 180000, ...(otaStatusError || {}) });
      }
      if (url.pathname === '/api/ota') {
        otaHeader = req.headers()['x-h2g-action'];
        otaBytes = req.postDataBuffer().length;
        if (otaBehavior === 'jsonError') {
          otaBehavior = 'networkError';
          return route.fulfill({ status: 408, contentType: 'application/json', body: JSON.stringify({
            ok: false, status: 408, code: 'total_timeout', error: 'absolute deadline',
            receivedBytes: 4096, expectedBytes: otaBytes, timeoutMs: 180000 }) });
        }
        if (otaBehavior === 'networkError') {
          otaBehavior = 'success';
          otaStatusError = { ok: false, status: 408, code: 'idle_timeout',
            error: 'No OTA upload progress for 2 seconds', receivedBytes: 2048,
            expectedBytes: otaBytes, timeoutMs: 180000 };
          return route.abort('connectionreset');
        }
        otaStatusError = null;
        return json({ ok: true, verified: true, bootVerified: true, rebooting: true,
          bytes: otaBytes, sourcePartition: 'app0', targetPartition: 'app1' });
      }
      if (url.pathname === '/api/fuel/petrol-calibration') return json({ active: false, currentCorrection: 1 });
      return route.fulfill({ status: 404, body: 'Unexpected fixture URL: ' + url.pathname });
    });
    await page.goto('http://h2g.test/');
    await page.waitForFunction(() => document.getElementById('lightRaw').textContent === '1830');
    assert.equal(await page.locator('#otaFirmware').textContent(), '0.3.9');
    assert.equal(await page.locator('#slotCurrentVersion').textContent(), '0.3.9');
    assert.equal(await page.locator('#slotCurrentMeta').textContent(), 'app0 · запущена');
    assert.equal(await page.locator('#slotApp0Version').textContent(), '0.3.9');
    assert.match(await page.locator('#slotApp0Meta').textContent(), /запущен/);
    assert.equal(await page.locator('#slotApp0Card').getAttribute('class'), 'card slotCard running');
    assert.equal(await page.locator('#slotApp1Version').textContent(), '0.3.6');
    assert.match(await page.locator('#slotApp1Meta').textContent(), /следующий OTA/);
    assert.equal(await page.locator('#otaRunning').textContent(), 'app0');
    assert.equal(await page.locator('#otaBoot').textContent(), 'app0');
    assert.equal(await page.locator('#otaLast').textContent(), 'Применено');
    assert.match(await page.locator('#otaRuntime').textContent(), /Следующее обновление: app1@0x410000/);
    assert.match(await page.locator('#persistRuntime').textContent(), /CRC: OK/);
    assert.match(await page.locator('#persistRuntime').textContent(), /3[,.]7 с \/ 24 с/);
    assert.match(await page.locator('#persistRuntime').textContent(), /хвост целый/);

    await page.waitForFunction(() => document.getElementById('backgroundInstalled').textContent.includes('Установленного файла нет'));
    await page.evaluate(() => chooseAsset('background', new File(['GIF89a'], 'bad.gif', { type: 'image/gif' })));
    assert.match(await page.locator('#backgroundCandidate').textContent(), /PNG, JPEG и WebP/);
    await page.setInputFiles('#backgroundFile', path.join(__dirname, '../../assets/haval_startup_preview.png'));
    await page.waitForFunction(() => document.getElementById('backgroundCandidate').textContent.includes('RGB565 240×240'));
    assert.match(await page.locator('#backgroundCandidate').textContent(), /115.?200 байт/);
    await page.click('#backgroundUpload');
    await page.waitForFunction(() => document.getElementById('backgroundInstalled').textContent.includes('240×240'));
    assert.equal(assetBodies.background.length, 115200);
    assert.equal(assetHeaders.background['x-h2g-action'], 'asset-background');
    assert.equal(await page.locator('#backgroundUploadBar').evaluate(e => e.style.width), '100%');

    await page.setInputFiles('#logoFile', path.join(__dirname, '../../assets/haval_logo_source_crop.png'));
    await page.waitForFunction(() => document.getElementById('logoCandidate').textContent.includes('RGB565 220×39'));
    assert.match(await page.locator('#logoCandidate').textContent(), /17.?160 байт/);
    await page.click('#logoUpload');
    await page.waitForFunction(() => document.getElementById('logoInstalled').textContent.includes('220×39'));
    assert.equal(assetBodies.logo.length, 220 * 39 * 2);
    assert.equal(assetHeaders.logo['x-h2g-action'], 'asset-logo');

    await page.click('#backgroundMode');
    await page.waitForFunction(() => document.getElementById('backgroundMode').textContent.includes('пользовательский'));
    assert.equal(assetState.background.enabled, false);
    await page.click('#logoDelete');
    await page.waitForFunction(() => document.getElementById('logoInstalled').textContent.includes('Установленного файла нет'));
    assert.equal(assetState.logo.present, false);

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
    await page.locator('#firmware').setInputFiles({ name: 'too-large.bin', mimeType: 'application/octet-stream', buffer: Buffer.alloc(2097153) });
    assert.equal(await page.locator('#install').isDisabled(), true);
    assert.match(await page.locator('#otaText').textContent(), /2\.00 МБ/);
    await page.locator('#firmware').setInputFiles({ name: 'h2-gauge-v0.3.9-esp32s3-n16r8.bin', mimeType: 'application/octet-stream', buffer: Buffer.alloc(5000, 0xA5) });
    assert.equal(await page.locator('#install').isEnabled(), true);
    await page.click('#install');
    await page.waitForFunction(() => document.getElementById('otaText').textContent.includes('[invalid_image]'));
    assert.match(await page.locator('#otaText').textContent(), /not for ESP32-S3/);
    assert.equal(otaBytes, 0, 'binary body was sent after rejected preflight');
    assert.equal(await page.locator('#install').isEnabled(), true);
    await page.click('#install');
    await page.waitForFunction(() => document.getElementById('otaText').textContent.includes('[total_timeout]'));
    assert.match(await page.locator('#otaText').textContent(), /4096 из 5000 байт/);
    assert.equal(await page.locator('#install').isEnabled(), true);
    await page.click('#install');
    await page.waitForFunction(() => document.getElementById('otaText').textContent.includes('[idle_timeout]'));
    assert.match(await page.locator('#otaText').textContent(), /2048 из 5000 байт/);
    assert.equal(await page.locator('#install').isEnabled(), true);
    await page.click('#install');
    await page.waitForFunction(() => document.getElementById('otaText').textContent.includes('выбран app1'));
    assert.equal(otaPreflightHeader, 'ota');
    assert.equal(otaPreflights, 4);
    assert.equal(otaHeader, 'ota');
    assert.equal(otaBytes, 5000);
    await page.evaluate(() => { otaActive = false; }); // simulate post-reboot page lifecycle
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
    console.log('PASS browser: assets RGB565/CRC/raw controls, persistence diagnostics, brightness/live data, slot cards, OTA preflight/error recovery/install, validation/reset and 360–1280 px layouts');
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
