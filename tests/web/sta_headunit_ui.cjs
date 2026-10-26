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
    boostMinBar: -1.0,
    boostMaxBar: 1.2,
    boostWarningBar: 0.9,
    boostDangerBar: 1.05,
    coolantC: 90.0,
    iatC: 24.0,
    voltageV: 14.12,
    loadPct: 38.0,
    stftPct: -1.5,
    ltftPct: 1.5,
    trimSumPct: 0.0,
    trimWarn: false,
    o2V: 0.683,
    timingAdvance: 21.5,
    downstreamO2: 0.455,
    evapPurge: 0,
    airflowGps: 9.85,
    fuelValid: true,
    perHour: false,
    consumption: 11.8,
    fuelLph: 6.37,
    windowL100: 14.2,
    clock: { synced: true, epochSec: 1792664040, utcOffsetMin: 180, localDate: '2026-10-22', localTime: '11:14' },
    dtc: { milOn: false, stored: 0, pending: 0, permanent: 0, firstCode: '',
      readinessKnown: true, readinessSupported: 6, readinessComplete: 4 },
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
        date: '2026-10-23',
        dateUnknown: false,
        tripCount: 2,
        distanceKm: 30.0,
        petrolLiters: 0.50,
        lpgLiters: 5.50,
        movingAverageL100: 17.5,
        averageL100: 20.0,
        preMoveSeconds: 60,
        inTripIdleSeconds: 200,
        // No peak for this day: the chart must leave it out, not draw a zero.
        maxSpeedKnown: false,
        maxSpeedKph: null,
      },
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
  // 1x1 transparent PNG, identical to include/probe_png.h. Read from the header
  // rather than repeated here, so a change to the beacon cannot pass this test
  // while the device sends something else.
  const PROBE_PNG = (() => {
    const header = fs.readFileSync(path.join(__dirname, '../../include/probe_png.h'), 'utf8');
    const bytes = [...header.matchAll(/0x([0-9a-fA-F]{2})/g)].map(m => parseInt(m[1], 16));
    assert.equal(bytes.length, 68, 'probe_png.h must carry exactly 68 bytes');
    assert.deepEqual(bytes.slice(0, 8), [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a],
      'probe_png.h must start with the PNG signature');
    return Buffer.from(bytes);
  })();

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
      if (u.pathname === '/h2probe') {
        // The beacon, byte for byte what the firmware sends. An <img> decodes
        // it without any CORS involvement, which is the whole point: the
        // Android 11 case below runs with no Access-Control-Allow-Origin on
        // the JSON and still has to find the gauge.
        res.writeHead(200, { ...common, 'Content-Type': 'image/png' });
        res.end(PROBE_PNG);
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
      // The three parameters the block supports and the gauge never asked for.
      // The zero purge is a value, not a dash: an engine at idle with no purge
      // scheduled really does command 0 %, and the two must not look alike.
      assert.equal(await page.textContent('#lvAngle'), '21.5');
      assert.equal(await page.textContent('#lvO2Down'), '0.455 В');
      assert.equal(await page.textContent('#lvPurge'), '0%');
      assert.match(await page.textContent('#lvDtcSub'), /Мониторы: 4\/6/);

      // 3. Check zero horizontal overflow on every tab:
      for (const tabId of ['cur', 'live', 'hist', 'days']) {
        await page.click(`.tab[data-tab="${tabId}"]`);
        const overflow = await page.evaluate(() =>
          document.documentElement.scrollWidth > document.documentElement.clientWidth + 1);
        assert.equal(overflow, false, `${vp.label}: horizontal overflow detected on tab ${tabId}`);
      }

      // 3b. The month chart on the days tab. The head unit is the screen the
      //     owner actually looks at, so the same two rules as the portal apply:
      //     the month figure is litres over kilometres, and a day without a
      //     peak speed stays empty instead of becoming a zero.
      await page.click('.tab[data-tab="days"]');
      await page.waitForFunction(() => document.querySelectorAll('#chartFuel rect').length > 0,
        null, { timeout: 5000 });
      const monthSum = await page.textContent('#chartSummary');
      // Two dated days: 18.42 km / 3.33 L and 30.0 km / 6.0 L, so 48.42 km and
      // 9.33 L. Weighted: 19.3 L/100. The mean of the two daily means is 19.0,
      // which is exactly the number this must NOT show.
      assert.match(monthSum, /48\.4 км/, `${vp.label}: chart must sum the dated days, got ${monthSum}`);
      assert.match(monthSum, /19\.3 л\/100 км/, `${vp.label}: weighted figure wrong: ${monthSum}`);
      assert.ok(!monthSum.includes('19.0 л/100'), `${vp.label}: the month is a mean of means`);
      assert.equal(await page.$$eval('#chartFuel rect', rs => rs.length), 2,
        `${vp.label}: the undated day has no place on a date axis`);
      assert.equal(await page.$$eval('#chartSpeed circle', rs => rs.length), 1,
        `${vp.label}: the day without a peak must stay empty, not become a zero`);
      // And the line must not be drawn through that gap: with one point and one
      // gap the path holds a single "M" and no "L" at all.
      const speedPath = await page.$eval('#chartSpeed path', el => el.getAttribute('d'));
      assert.ok(!speedPath.includes('L'),
        `${vp.label}: the line was bridged across a day with no peak: ${speedPath}`);

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


    // The boost scale is the device's, not the page's. Engine configuration
    // changes the ceiling (1.2 bar default, lower for a stock car), and a page
    // that keeps its own numbers would silently mis-scale every reading.
    const boostTicks = () => page.$$eval('#dial-boost .dial-scale', els => els.map(e => e.textContent));
    let ticks = await boostTicks();
    assert.ok(ticks.includes('+1.2'), `boost dial must end at the configured +1.2, saw ${ticks.join(',')}`);
    assert.equal(ticks[0], '-1.0', 'boost dial must start at the configured -1.0');

    liveState.boostMaxBar = 1.4;
    liveState.boostWarningBar = 1.0;
    liveState.boostDangerBar = 1.2;
    await page.waitForFunction(() =>
      Array.from(document.querySelectorAll('#dial-boost .dial-scale'))
        .some(e => e.textContent === '+1.4'), null, { timeout: 4000 });
    ticks = await boostTicks();
    assert.ok(ticks.includes('+1.4') && !ticks.includes('+1.2'),
      `the boost dial must rebuild on a new ceiling, saw ${ticks.join(',')}`);

    // A scale that cannot be true is ignored instead of drawn: warning above
    // danger would paint a zone that inverts mid-arc.
    liveState.boostMaxBar = 1.2;
    liveState.boostWarningBar = 1.1;
    liveState.boostDangerBar = 1.0;
    await page.waitForTimeout(900);
    ticks = await boostTicks();
    assert.ok(ticks.includes('+1.4'),
      `an invalid scale must leave the previous dial alone, saw ${ticks.join(',')}`);
    liveState.boostMaxBar = 1.2;
    liveState.boostWarningBar = 0.9;
    liveState.boostDangerBar = 1.05;

    // Simulate live Check Engine P0171 + closed trip #3 appearing at 4 Hz without manual reload:
    const pollsBefore = livePollCount;
    liveState.dtc = { milOn: true, stored: 1, pending: 1, permanent: 0,
      firstCode: 'P0171', readinessKnown: true, readinessSupported: 8,
      readinessComplete: 5 };
    liveState.history.headSequence = 3;
    tripsPayload.headSequence = 3;
    tripsPayload.total = 3;
    await page.waitForFunction(() => document.getElementById('milPill')?.textContent === 'CHECK P0171');
    await page.waitForFunction(() => /Мониторы: 5\/8/.test(document.getElementById('lvDtcSub').textContent));
    // A parameter the block stops answering is a dash, never a stale number and
    // never a zero: the head unit has no room for an explanation.
    liveState.timingAdvance = null;
    await page.waitForFunction(() => document.getElementById('lvAngle').textContent === '—');
    liveState.timingAdvance = 21.5;
    await page.waitForFunction(() => document.getElementById('lvAngle').textContent === '21.5');
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

      // Android 11: the same page, the same gauge, one difference - the JSON
      // read is blocked by CORS because the response carries no
      // Access-Control-Allow-Origin. Android 7's WebView allowed it; Chrome
      // does not, and the 2026-10-25 report was exactly this: the head unit
      // running Android 7 found the gauge, the phone running Android 11 scanned
      // forever and only a typed address worked.
      //
      // The beacon is what makes it work: an <img> load is not subject to CORS,
      // so discovery no longer depends on being allowed to read anything.
      const strictGauge = http.createServer((req, res) => {
        const u = new URL(req.url, 'http://127.0.0.1');
        if (u.pathname === '/h2probe') {
          res.writeHead(200, { 'Content-Type': 'image/png', 'Cache-Control': 'no-store' });
          res.end(PROBE_PNG);
          return;
        }
        // Deliberately no Access-Control-Allow-Origin, like a server that has
        // not been taught to answer a file:// page.
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ version: '0.4.40', hostname: 'h2', obdConnected: true, cur: {} }));
      });
      await new Promise(resolve => strictGauge.listen(0, '127.0.44.77', resolve));
      const strictPort = strictGauge.address().port;
      // A device on the same subnet that answers 200 on every path - the decoy
      // that a payload sniff would have accepted and an image cannot.
      const talker = http.createServer((_req, res) => {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end('{"status":"printer","version":"1.0","hostname":"x","obdConnected":false,"cur":{}}');
      });
      await new Promise(resolve => talker.listen(strictPort, '127.0.44.9', resolve));
      try {
        const strictCtx = await browser.newContext({ viewport: { width: 1024, height: 600 } });
        const strictPage = await strictCtx.newPage();
        const strictErrors = [];
        strictPage.on('pageerror', e => strictErrors.push(String(e)));
        // noredirect=1 so the page stays put and the report can be read back:
        // the redirect itself is covered by the test above.
        const strictUrl = `file://${openHtmlPath}?prefix=127.0.44&maxHost=250&port=${strictPort}` +
          `&probe=/h2probe&apHost=&noredirect=1`;
        await strictPage.goto(strictUrl, { waitUntil: 'domcontentloaded' });
        await strictPage.waitForFunction(
          () => document.getElementById('stateBadge').textContent === 'НАЙДЕН',
          null, { timeout: 15000 });
        assert.match(await strictPage.textContent('#statusTxt'),
          /127\.0\.44\.77/, 'the found address must be reported');
        // No version, because the JSON was unreadable - and the page says so
        // instead of pretending it read one.
        assert.match(await strictPage.textContent('#detailTxt'),
          /Прибор ответил/, 'a found host with unreadable JSON must be reported as found');
        assert.doesNotMatch(await strictPage.textContent('#detailTxt'),
          /v0\.4\.40/, 'no version may be invented when the payload could not be read');
        assert.deepEqual(strictErrors, [], `Android 11 discovery page errors: ${strictErrors.join('; ')}`);
        await strictCtx.close();
        console.log(`  [ok] file://h2-open.html found 127.0.44.77:${strictPort} with /api/ro/live blocked by CORS (Android 11 case), decoy .9 ignored`);
      } finally {
        await new Promise(r => strictGauge.close(r));
        await new Promise(r => talker.close(r));
      }

      // ---- 0.4.44 regressions: the four ways the sweep could still fail ------
      //
      // The page used to truncate the gateway candidate list to its 24 fastest
      // entries and then sweep those in every round. A phone whose browser hides
      // the local address produces dozens of subnets that refuse instantly, so
      // the real one could rank below the cut-off - and because the list is
      // rebuilt identically every round, it was never scanned at all. This test
      // builds exactly that: 39 subnets that refuse in well under a millisecond,
      // and a real one whose gateway takes 150 ms to answer, which sorts it last.
      {
        const slowGw = http.createServer((_req, res) => {
          setTimeout(() => { res.writeHead(404, { 'Access-Control-Allow-Origin': '*' }); res.end('gw'); }, 150);
        });
        await new Promise(r => slowGw.listen(0, '127.0.20.1', r));
        const wGwPort = slowGw.address().port;
        const wGauge = http.createServer(makeH2Handler());
        await new Promise(r => wGauge.listen(0, '127.0.20.187', r));
        const wLanPort = wGauge.address().port;
        try {
          const wCtx = await browser.newContext({ viewport: { width: 1024, height: 600 } });
          const wPage = await wCtx.newPage();
          const wErrors = [];
          wPage.on('pageerror', e => wErrors.push(String(e)));
          const wUrl = `file://${openHtmlPath}?gwBase=127.0&maxSub=40&gwPort=${wGwPort}` +
            `&port=${wLanPort}&apHost=&probe=/h2probe&noredirect=1`;
          await wPage.goto(wUrl, { waitUntil: 'domcontentloaded' });
          // Round 1 must end without the gauge, because its subnet is not in the
          // first window. If it turns up there, the setup proves nothing and the
          // test says so instead of passing on a technicality.
          const stage = await wPage.waitForFunction(() => {
            const badge = document.getElementById('stateBadge').textContent;
            const st = document.getElementById('statusTxt').textContent;
            if (badge === 'НАЙДЕН') return 'found-in-round-1';
            if (/попытка #/.test(st)) return 'round-2';
            return false;
          }, null, { timeout: 20000 }).then(h => h.jsonValue());
          assert.equal(stage, 'round-2',
            'the slow gateway must rank outside the first window (setup check)');
          await wPage.waitForFunction(
            () => document.getElementById('stateBadge').textContent === 'НАЙДЕН',
            null, { timeout: 30000 });
          assert.match(await wPage.textContent('#statusTxt'), /127\.0\.20\.187/,
            'the subnet that ranked last must still be swept and found');
          assert.deepEqual(wErrors, [], `window-rotation page errors: ${wErrors.join('; ')}`);
          await wCtx.close();
          console.log('  [ok] 39 instant-reject subnets cannot hide the real one: window rotates and 127.0.20.187 is found in round 2');
        } finally {
          await new Promise(r => slowGw.close(r));
          await new Promise(r => wGauge.close(r));
        }
      }

      // A network with no gauge on it must keep saying so. Nothing may be
      // reported as found, and the page must survive a full round of failures.
      {
        const aKill = net.createServer(sock => sock.destroy());
        await new Promise(r => aKill.listen(0, '0.0.0.0', r));
        const aLanPort = aKill.address().port;
        try {
          const aCtx = await browser.newContext({ viewport: { width: 1024, height: 600 } });
          const aPage = await aCtx.newPage();
          const aErrors = [];
          aPage.on('pageerror', e => aErrors.push(String(e)));
          const aUrl = `file://${openHtmlPath}?gwBase=127.0&maxSub=2&port=${aLanPort}` +
            `&apHost=&probe=/h2probe&noredirect=1`;
          await aPage.goto(aUrl, { waitUntil: 'domcontentloaded' });
          await aPage.waitForTimeout(6000);
          assert.notEqual(await aPage.textContent('#stateBadge'), 'НАЙДЕН',
            'an empty network must never be reported as found');
          assert.match(await aPage.textContent('#statusTxt'), /попытка #/,
            'the page must keep retrying instead of stopping');
          assert.deepEqual(aErrors, [], `empty-network page errors: ${aErrors.join('; ')}`);
          await aCtx.close();
          console.log('  [ok] empty network: no false НАЙДЕН, retries continue, zero page errors');
        } finally {
          await new Promise(r => aKill.close(r));
        }
      }

      // «Повтор» has to interrupt, not wait its turn. Four gateways answer, no
      // host ever does, so one round is a sweep of four subnets with 450 ms per
      // probe - about eight seconds. The click has to produce a fresh gateway
      // sweep within a couple of seconds; before 0.4.44 the guard in
      // runDiscoveryRound() swallowed it whole.
      {
        const freeProbe = net.createServer();
        await new Promise(r => freeProbe.listen(0, '127.0.0.1', r));
        const rGwPort = freeProbe.address().port;
        await new Promise(r => freeProbe.close(r));
        let gwHits = 0;
        const rGws = [];
        for (let s = 0; s <= 3; s++) {
          const srv = http.createServer((_q, r) => {
            gwHits++;
            r.writeHead(404, { 'Access-Control-Allow-Origin': '*' });
            r.end('gw');
          });
          await new Promise(r => srv.listen(rGwPort, `127.0.${s}.1`, r));
          rGws.push(srv);
        }
        const hanging = new Set();
        const rSlow = net.createServer(sock => {
          hanging.add(sock);
          sock.on('close', () => hanging.delete(sock));
        });
        await new Promise(r => rSlow.listen(0, '0.0.0.0', r));
        const rLanPort = rSlow.address().port;
        try {
          const rCtx = await browser.newContext({ viewport: { width: 1024, height: 600 } });
          const rPage = await rCtx.newPage();
          const rErrors = [];
          rPage.on('pageerror', e => rErrors.push(String(e)));
          const rUrl = `file://${openHtmlPath}?gwBase=127.0&maxSub=3&gwPort=${rGwPort}` +
            `&port=${rLanPort}&apHost=&probe=/h2probe&noredirect=1`;
          await rPage.goto(rUrl, { waitUntil: 'domcontentloaded' });
          // Wait until the sweep is genuinely in progress: the status line names
          // a host range, which only the subnet scanner does.
          await rPage.waitForFunction(
            () => /Сканирование/.test(document.getElementById('detailTxt').textContent),
            null, { timeout: 15000 });
          await rPage.waitForTimeout(1200);
          const hitsBeforeClick = gwHits;
          assert.ok(hitsBeforeClick >= 4, `round 1 should have probed all four gateways, saw ${hitsBeforeClick}`);
          await rPage.click('#rescanBtn');
          const deadline = Date.now() + 2500;
          while (gwHits === hitsBeforeClick && Date.now() < deadline) {
            await rPage.waitForTimeout(50);
          }
          assert.ok(gwHits > hitsBeforeClick,
            `«Повтор» must start a new generation immediately, but the gateway sweep did not restart ` +
            `within 2.5 s (hits stayed at ${hitsBeforeClick}) - the old sweep is still running`);
          assert.deepEqual(rErrors, [], `retry-during-sweep page errors: ${rErrors.join('; ')}`);
          await rCtx.close();
          console.log(`  [ok] «Повтор» interrupts a running sweep (new gateway pass in <2.5 s, was ${hitsBeforeClick} hits)`);
        } finally {
          for (const s of hanging) s.destroy();
          await new Promise(r => rSlow.close(r));
          for (const srv of rGws) await new Promise(r => srv.close(r));
        }
      }

      // The priority pass sweeps .2..63 of the four best candidates and the full
      // pass used to sweep those same 62 addresses again: 24x253 + 4x62 = 6320
      // probes per round instead of 6072. One candidate subnet makes the count
      // exact - a whole round is 253 probes, or 315 with the duplicate.
      {
        const dGw = http.createServer((_q, r) => {
          r.writeHead(404, { 'Access-Control-Allow-Origin': '*' });
          r.end('gw');
        });
        await new Promise(r => dGw.listen(0, '127.0.0.1', r));
        const dGwPort = dGw.address().port;
        let probes = 0;
        const dKill = net.createServer(sock => {
          probes++;
          sock.destroy();
        });
        await new Promise(r => dKill.listen(0, '0.0.0.0', r));
        const dLanPort = dKill.address().port;
        try {
          const dCtx = await browser.newContext({ viewport: { width: 1024, height: 600 } });
          const dPage = await dCtx.newPage();
          const dUrl = `file://${openHtmlPath}?gwBase=127.0&maxSub=0&gwPort=${dGwPort}` +
            `&port=${dLanPort}&apHost=&probe=/h2probe&noredirect=1`;
          await dPage.goto(dUrl, { waitUntil: 'domcontentloaded' });
          // One round finishes in tens of milliseconds; the retry timer is 2.5 s,
          // so 1.2 s of observation holds exactly one round.
          await dPage.waitForTimeout(1200);
          assert.equal(probes, 253,
            `one round of one subnet must probe .2..254 exactly once each; got ${probes} ` +
            `(315 would mean the priority pass swept .2..63 a second time)`);
          await dCtx.close();
          console.log('  [ok] no duplicate low-half sweep: 253 probes for one subnet, not 315');
        } finally {
          await new Promise(r => dKill.close(r));
          await new Promise(r => dGw.close(r));
        }
      }
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
