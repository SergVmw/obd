// Head-Unit Read-Only Dashboard browser contract & responsive layout test.
// Verifies 1280x720 (HD HU), 1024x600 (WSVGA HU) and 400x820 (Xiaomi 14):
// - 6 analog SVG dials (12 o'clock = 90 C on coolant & 0% on fuel trim),
// - [Вид: Стрелки / Цифры] toggle + reload persistence via localStorage,
// - 4 Hz live polling updates + Check Engine alert + automatic history reload on headSequence change,
// - Zero console errors, zero horizontal scrollbar overflow, >= 44px touch targets.
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const zlib = require('node:zlib');

(async () => {
  const htmlRaw = fs.readFileSync(path.join(__dirname, '../../web/headunit.html'));
  const openHtmlPath = path.join(__dirname, '../../web/h2-open.html');
  const openHtmlRaw = fs.readFileSync(openHtmlPath);
  const htmlGz = zlib.gzipSync(htmlRaw, { level: 9 });

  let liveState = {
    version: '0.4.33',
    hostname: 'h2',
    uptimeMs: 42000,
    obdConnected: true,
    rpmFresh: true,
    lpgEnabled: true,
    lpgActive: true,
    dfco: false,
    fuelMode: 'LPG',
    fuelLoop: 'PID 03 0x02',
    ecuId: '0x7E8',
    rpm: 2150,
    speedKph: 56,
    rawSpeedKph: 54,
    mapKpa: 118.4,
    baroKpa: 99.4,
    boostBar: 0.19,
    peakBoostBar: 0.80,
    coolantC: 90.0,
    iatC: 24.0,
    voltageV: 14.12,
    loadPct: 38.0,
    stftPct: -1.5,
    ltftPct: 1.5,
    trimSumPct: 0.0,
    trimWarn: false,
    o2V: 0.683,
    airflowGps: 9.85,
    fuelValid: true,
    perHour: false,
    consumption: 11.8,
    fuelLph: 6.37,
    windowL100: 14.2,
    clock: { synced: true, epochSec: 1792664040, utcOffsetMin: 180, localDate: '2026-10-22', localTime: '11:14' },
    dtc: { milOn: false, stored: 0, pending: 0, permanent: 0, firstCode: '' },
    history: { ready: true, healthy: true, headSequence: 2 },
    cur: {
      startEpochSec: 1792661556,
      dateUnknown: false,
      merged: false,
      localDate: '2026-10-22',
      localTime: '10:32',
      wallDurationSec: 2484,
      engineSeconds: 2465,
      movingSeconds: 1523,
      idleSeconds: 942,
      preMoveSeconds: 124,
      inTripIdleSeconds: 818,
      distanceKm: 11.0,
      petrolDistanceKm: 0.76,
      lpgDistanceKm: 10.24,
      totalLiters: 2.91,
      petrolLiters: 0.26,
      lpgLiters: 2.65,
      preMoveLiters: 0.10,
      petrolPreMoveLiters: 0.10,
      lpgPreMoveLiters: 0.0,
      inTripIdleLiters: 0.66,
      idleLiters: 0.76,
      movingLiters: 2.15,
      averageL100: 26.5,
      movingAverageL100: 19.5,
      petrolMovingL100: 20.6,
      lpgMovingL100: 19.4,
      lpgSharePercent: 90.9,
      idleSharePercent: 26.2,
      avgMovingSpeedKph: 26.0,
      maxSpeedKnown: true,
      maxSpeedKph: 66.0,
    },
  };

  let tripsPayload = {
    ready: true,
    healthy: true,
    headSequence: 2,
    capacity: 2048,
    offset: 0,
    limit: 50,
    total: 2,
    hasMore: false,
    corruptTail: false,
    trips: [
      {
        sequence: 2,
        startEpochSec: 1792660000,
        utcOffsetMin: 180,
        dateUnknown: false,
        merged: false,
        localDate: '2026-10-22',
        localTime: '10:06',
        wallDurationSec: 1710,
        engineSeconds: 1698,
        movingSeconds: 1290,
        idleSeconds: 420,
        preMoveSeconds: 78,
        inTripIdleSeconds: 342,
        distanceKm: 18.42,
        totalLiters: 3.33,
        preMoveLiters: 0.06,
        inTripIdleLiters: 0.24,
        idleLiters: 0.30,
        movingLiters: 3.03,
        averageL100: 18.1,
        movingAverageL100: 15.8,
        lpgSharePercent: 94.2,
        idleSharePercent: 9.0,
        avgMovingSpeedKph: 48.2,
        maxSpeedKnown: true,
        maxSpeedKph: 84.0,
        petrolCorrection: 1.0,
        lpgCorrection: 1.2,
        petrol: { rawLiters: 0.19, liters: 0.19, preMoveLiters: 0.06, inTripIdleLiters: 0.01, distanceKm: 0.92, movingAverageL100: 13.8 },
        lpg: { rawLiters: 2.62, liters: 3.14, preMoveLiters: 0.0, inTripIdleLiters: 0.23, distanceKm: 17.5, movingAverageL100: 15.9 },
      },
      {
        sequence: 1,
        startEpochSec: 0,
        utcOffsetMin: 180,
        dateUnknown: true,
        merged: false,
        localDate: '',
        localTime: '',
        wallDurationSec: 0,
        engineSeconds: 11687,
        movingSeconds: 6858,
        idleSeconds: 4829,
        preMoveSeconds: 0,
        inTripIdleSeconds: 4829,
        distanceKm: 61.62,
        totalLiters: 14.6,
        preMoveLiters: 0.0,
        inTripIdleLiters: 3.69,
        idleLiters: 3.69,
        movingLiters: 10.91,
        averageL100: 23.7,
        movingAverageL100: 17.7,
        lpgSharePercent: 91.5,
        idleSharePercent: 25.3,
        avgMovingSpeedKph: 32.3,
        maxSpeedKnown: false,
        maxSpeedKph: null,
        petrolCorrection: 1.0,
        lpgCorrection: 1.2,
        petrol: { rawLiters: 1.24, liters: 1.24, preMoveLiters: 0.0, inTripIdleLiters: 0.73, distanceKm: 1.98, movingAverageL100: 26.1 },
        lpg: { rawLiters: 11.13, liters: 13.36, preMoveLiters: 0.0, inTripIdleLiters: 2.96, distanceKm: 59.64, movingAverageL100: 17.4 },
      },
    ],
  };

  const daysPayload = {
    ready: true,
    healthy: true,
    limit: 31,
    totalDays: 2,
    totalTrips: 2,
    hasMore: false,
    corruptTail: false,
    days: [
      {
        date: '2026-10-22',
        dateUnknown: false,
        tripCount: 1,
        distanceKm: 18.42,
        petrolLiters: 0.19,
        lpgLiters: 3.14,
        movingAverageL100: 15.8,
        averageL100: 18.1,
        preMoveSeconds: 78,
        inTripIdleSeconds: 342,
        maxSpeedKnown: true,
        maxSpeedKph: 84.0,
      },
      {
        date: '',
        dateUnknown: true,
        tripCount: 1,
        distanceKm: 61.62,
        petrolLiters: 1.24,
        lpgLiters: 13.36,
        movingAverageL100: 17.7,
        averageL100: 23.7,
        preMoveSeconds: 0,
        inTripIdleSeconds: 4829,
        maxSpeedKnown: false,
        maxSpeedKph: null,
      },
    ],
  };

  let livePollCount = 0;
  function makeH2Handler() {
    return (req, res) => {
      const common = {
        'Access-Control-Allow-Origin': '*',
        'Cache-Control': 'no-store',
        'X-Content-Type-Options': 'nosniff',
      };
      if (req.method !== 'GET') {
        res.writeHead(405, { ...common, 'Allow': 'GET', 'Content-Type': 'application/json' });
        res.end('{"error":"read_only","allow":"GET"}');
        return;
      }
      const u = new URL(req.url, 'http://127.0.0.1');
      if (u.pathname === '/') {
        res.writeHead(200, {
          ...common,
          'Content-Type': 'text/html; charset=utf-8',
          'Content-Encoding': 'gzip',
        });
        res.end(htmlGz);
        return;
      }
      if (u.pathname === '/h2-open.html') {
        res.writeHead(200, {
          ...common,
          'Content-Type': 'text/html; charset=utf-8',
          'Content-Disposition': 'attachment; filename="h2-open.html"',
        });
        res.end(openHtmlRaw);
        return;
      }
      if (u.pathname === '/api/ro/live') {
        ++livePollCount;
        res.writeHead(200, { ...common, 'Content-Type': 'application/json; charset=utf-8' });
        res.end(JSON.stringify(liveState));
        return;
      }
      if (u.pathname === '/api/ro/trips') {
        res.writeHead(200, { ...common, 'Content-Type': 'application/json; charset=utf-8' });
        res.end(JSON.stringify(tripsPayload));
        return;
      }
      if (u.pathname === '/api/ro/trips/days') {
        res.writeHead(200, { ...common, 'Content-Type': 'application/json; charset=utf-8' });
        res.end(JSON.stringify(daysPayload));
        return;
      }
      res.writeHead(404, { ...common, 'Content-Type': 'application/json' });
      res.end('{"error":"not_found"}');
    };
  }

  const server = http.createServer(makeH2Handler());

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  const baseUrl = `http://127.0.0.1:${port}/`;

  const browser = await chromium.launch();
  try {
    const viewports = [
      { label: 'HD Head Unit 1280x720', width: 1280, height: 720 },
      { label: 'WSVGA Head Unit 1024x600', width: 1024, height: 600 },
      { label: 'Xiaomi 14 Portrait 400x820', width: 400, height: 820 },
    ];

    for (const vp of viewports) {
      const context = await browser.newContext({ viewport: { width: vp.width, height: vp.height } });
      const page = await context.newPage();
      const errors = [];
      page.on('pageerror', e => errors.push(String(e)));
      page.on('console', msg => {
        if (msg.type() === 'error') errors.push(msg.text());
      });

      await page.goto(baseUrl, { waitUntil: 'domcontentloaded' });
      await page.waitForFunction(() => document.getElementById('dial-econ-txt')?.textContent === '11.8');

      // 1. Touch target heights >= 44px and zero horizontal overflow across all 4 tabs:
      const btnHeights = await page.$$eval('.tab, .mode-btn', els =>
        els.map(e => Math.round(e.getBoundingClientRect().height)));
      for (const h of btnHeights) {
        assert.ok(h >= 44, `${vp.label}: button height ${h}px < 44px touch target`);
      }

      // 2. Verify 90 C coolant and 0.0% fuel trim needles point straight to 12 o'clock (0.0deg)!
      await page.click('.tab[data-tab="live"]');
      const coolantTransform = await page.$eval('#dial-coolant-nd', el => el.style.transform);
      const trimTransform = await page.$eval('#dial-trim-nd', el => el.style.transform);
      assert.equal(coolantTransform, 'rotate(0deg)', `${vp.label}: 90 C coolant must sit at 12 o'clock (0deg)`);
      assert.equal(trimTransform, 'rotate(0deg)', `${vp.label}: 0% trim sum must sit at 12 o'clock (0deg)`);
      assert.equal(await page.textContent('#lvFuelLoop'), 'PID 03 0x02');
      assert.equal(await page.textContent('#lvO2'), '0.68 В');

      // 3. Check zero horizontal overflow on every tab:
      for (const tabId of ['cur', 'live', 'hist', 'days']) {
        await page.click(`.tab[data-tab="${tabId}"]`);
        const overflow = await page.evaluate(() =>
          document.documentElement.scrollWidth > document.documentElement.clientWidth + 1);
        assert.equal(overflow, false, `${vp.label}: horizontal overflow detected on tab ${tabId}`);
      }

      assert.deepEqual(errors, [], `${vp.label}: unexpected console/page errors: ${errors.join('; ')}`);
      await context.close();
      console.log(`  [ok] ${vp.label}: 4 tabs, 6 SVG dials, 12-o'clock geometry, >=44px targets, zero overflow`);
    }

    // Interactive live state-transition & mode persistence test on 1280x720:
    const ctx = await browser.newContext({ viewport: { width: 1280, height: 720 } });
    const page = await ctx.newPage();
    await page.goto(baseUrl, { waitUntil: 'domcontentloaded' });
    await page.waitForFunction(() => document.getElementById('tabHist')?.textContent === 'Поездки (2)');

    // Toggle [Вид: Стрелки -> Вид: Цифры], check visibility & persistence across reload:
    await page.click('#modeToggle');
    assert.equal(await page.textContent('#modeToggle'), 'Вид: Цифры');
    assert.equal(await page.isVisible('#curInstDig'), true);
    assert.equal(await page.isVisible('#dial-econ'), false);
    await page.reload({ waitUntil: 'domcontentloaded' });
    assert.equal(await page.textContent('#modeToggle'), 'Вид: Цифры');
    await page.click('#modeToggle');
    assert.equal(await page.textContent('#modeToggle'), 'Вид: Стрелки');

    // Simulate live Check Engine P0171 + closed trip #3 appearing at 4 Hz without manual reload:
    const pollsBefore = livePollCount;
    liveState.dtc = { milOn: true, stored: 1, pending: 1, permanent: 0, firstCode: 'P0171' };
    liveState.history.headSequence = 3;
    tripsPayload.headSequence = 3;
    tripsPayload.total = 3;
    await page.waitForFunction(() => document.getElementById('milPill')?.textContent === 'CHECK P0171');
    await page.waitForFunction(() => document.getElementById('tabHist')?.textContent === 'Поездки (3)');
    assert.ok(livePollCount > pollsBefore, '4 Hz poller must advance automatically');

    // Verify trip row click toggles detail breakdown cleanly:
    await page.click('.tab[data-tab="hist"]');
    assert.equal(await page.isVisible('#det-2'), true);
    await page.click('.tbl-row[data-seq="2"]');
    assert.equal(await page.isVisible('#det-2'), false);
    await page.click('.tbl-row[data-seq="1"]');
    assert.equal(await page.isVisible('#det-1'), true);

    // Verify configurable refresh rate (#hzToggle: 4 -> 8 -> 10 -> 1 -> 2 -> 4 Гц),
    // real request cadence difference between 1 Гц and 10 Гц, and localStorage persistence:
    assert.equal(await page.textContent('#hzToggle'), '4 Гц');
    assert.equal(await page.textContent('#linkLabel'), 'h2.local · 4 Гц');
    await page.click('#hzToggle');
    assert.equal(await page.textContent('#hzToggle'), '8 Гц');
    await page.click('#hzToggle');
    assert.equal(await page.textContent('#hzToggle'), '10 Гц');
    assert.equal(await page.textContent('#linkLabel'), 'h2.local · 10 Гц');

    const c10Start = livePollCount;
    await page.waitForTimeout(650);
    const pollsAt10Hz = livePollCount - c10Start;
    assert.ok(pollsAt10Hz >= 4, `10 Гц mode expected >= 4 polls in 650 ms, got ${pollsAt10Hz}`);

    // Reload and prove 10 Гц persists in localStorage:
    await page.reload({ waitUntil: 'domcontentloaded' });
    assert.equal(await page.textContent('#hzToggle'), '10 Гц');
    assert.equal(await page.textContent('#linkLabel'), 'h2.local · 10 Гц');

    // Switch to 1 Гц and prove low-rate cadence makes <= 2 polls in 650 ms:
    await page.click('#hzToggle');
    assert.equal(await page.textContent('#hzToggle'), '1 Гц');
    const c1Start = livePollCount;
    await page.waitForTimeout(650);
    const pollsAt1Hz = livePollCount - c1Start;
    assert.ok(pollsAt1Hz <= 2, `1 Гц mode expected <= 2 polls in 650 ms, got ${pollsAt1Hz}`);
    assert.ok(pollsAt10Hz >= pollsAt1Hz + 3, `10 Гц (${pollsAt10Hz}) must poll significantly faster than 1 Гц (${pollsAt1Hz})`);

    // Cycle 1 -> 2 -> 4 Гц:
    await page.click('#hzToggle');
    assert.equal(await page.textContent('#hzToggle'), '2 Гц');
    await page.click('#hzToggle');
    assert.equal(await page.textContent('#hzToggle'), '4 Гц');

    // Verify 1-tap [Ярлык] download delivers byte-for-byte h2-open.html:
    const [download] = await Promise.all([
      page.waitForEvent('download'),
      page.click('#launcherBtn'),
    ]);
    assert.equal(download.suggestedFilename(), 'h2-open.html');
    const dlPath = await download.path();
    assert.deepEqual(fs.readFileSync(dlPath), openHtmlRaw, 'Downloaded h2-open.html must match web/h2-open.html byte-for-byte');

    await ctx.close();
    console.log(`  [ok] live 1..10 Гц refresh rate (10 Гц=${pollsAt10Hz} vs 1 Гц=${pollsAt1Hz} in 650 ms), [Ярлык] download, P0171 & row expand`);

    // End-to-end Android 7 file:// auto-discovery & automatic redirect test across a randomized /24 subnet:
    // - Subnet 127.0.43.0/24 simulates Xiaomi 14 randomizing 192.168.<X>.0/24;
    // - 127.0.43.1:<gwPort> responds like the active hotspot gateway while non-active subnets blackhole (timeout);
    // - 127.0.43.12:<lanPort> is a decoy HTTP 200 JSON server proving zero false positives;
    // - 127.0.43.187:<lanPort> is the live H2 Gauge STA Read-Only server!
    const blackholeSockets = new Set();
    const gwServers = [];
    let gwPort = 0;
    const activeGw = http.createServer((_req, res) => {
      res.writeHead(404, { 'Access-Control-Allow-Origin': '*' });
      res.end('gw');
    });
    await new Promise(resolve => activeGw.listen(0, '127.0.43.1', resolve));
    gwPort = activeGw.address().port;
    gwServers.push(activeGw);
    for (let s = 0; s <= 50; s++) {
      if (s === 43) continue;
      const bh = net.createServer(sock => {
        blackholeSockets.add(sock);
        sock.on('close', () => blackholeSockets.delete(sock));
      });
      await new Promise(resolve => bh.listen(gwPort, `127.0.${s}.1`, resolve));
      gwServers.push(bh);
    }

    const h2LanServer = http.createServer(makeH2Handler());
    await new Promise(resolve => h2LanServer.listen(0, '127.0.43.187', resolve));
    const lanPort = h2LanServer.address().port;

    const decoyServer = http.createServer((_req, res) => {
      res.writeHead(200, { 'Access-Control-Allow-Origin': '*', 'Content-Type': 'application/json' });
      res.end('{"status":"other_device_on_hotspot","version":"1.0"}');
    });
    await new Promise(resolve => decoyServer.listen(lanPort, '127.0.43.12', resolve));

    try {
      const fileCtx = await browser.newContext({ viewport: { width: 1024, height: 600 } });
      const filePage = await fileCtx.newPage();
      const fileErrors = [];
      filePage.on('pageerror', e => fileErrors.push(String(e)));

      const fileUrl = `file://${openHtmlPath}?gwBase=127.0&maxSub=50&gwPort=${gwPort}&port=${lanPort}`;
      await filePage.goto(fileUrl, { waitUntil: 'domcontentloaded' });

      // Wait for completely hands-free discovery across 127.0.43.2..254 + automatic redirect into live dashboard!
      await filePage.waitForURL(`http://127.0.43.187:${lanPort}/`, { timeout: 10000 });
      await filePage.waitForFunction(() => document.getElementById('dial-econ-txt')?.textContent === '11.8');
      assert.deepEqual(fileErrors, [], `h2-open.html page errors: ${fileErrors.join('; ')}`);
      await fileCtx.close();
      console.log(`  [ok] file://h2-open.html cold-discovered 127.0.43.187:${lanPort} (ignoring .12 decoy & blackhole subnets) and redirected to live dashboard`);
    } finally {
      for (const s of blackholeSockets) s.destroy();
      decoyServer.close();
      h2LanServer.close();
      for (const gs of gwServers) gs.close();
    }
  } finally {
    await browser.close();
    server.close();
  }
})().catch(err => {
  console.error(err);
  process.exit(1);
});
