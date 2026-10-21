// End-to-end Playwright browser test for Trip History UI, pure-UTC sync under
// foreign browser timezone, pagination, day summaries, warm-up & peak speed
// display, manual close/reset/clear, config round-trip, negative validation,
// and 360px/1280px responsive zero-overflow layout check.
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

(async () => {
  const browser = await chromium.launch();
  // Intentionally emulate New York timezone (UTC-4) while device config is UTC+3 (+180):
  // proves browser timezone NEVER overwrites device utcOffsetMin and pure UTC is sent!
  const context = await browser.newContext({ timezoneId: 'America/New_York' });
  const page = await context.newPage();
  page.on('dialog', dialog => dialog.accept());

  const html = fs.readFileSync(path.join(__dirname, '../../web/index.html'), 'utf8');

  let storedConfig = {
    schemaVersion: 12,
    firmwareVersion: '0.4.31',
    display: {
      brightnessDay: 82, brightnessNight: 26, brightnessMode: 2,
      manualNight: false, lightAutoCalibrate: true, lightNightAdc: 600,
      lightDayAdc: 3000, lightDimDelayMs: 3000, lightBrightenDelayMs: 1000,
      rotation: 0, startPage: 0, mainCenterValue: 0, mainCellLeft: 6,
      mainCellRight: 7, mainDigitSize: 0, displaySpeedOffsetKph: 5,
      uiLanguage: 0, autoReturnSec: 5, pageFuel: true, pageTemperature: true,
      pageDiagnostics: false, peakEnabled: true, colorText: '#F2FAF7',
      colorVacuum: '#5DC9E8', colorBoost: '#5EF0A8', colorWarning: '#FFBD59',
      colorDanger: '#FF5F68',
    },
    boost: {
      baroSource: 0, boostArcStyle: 0, boostPointerMode: 0, boostPointerColor: 0,
      boostPointerTipRadius: 99, boostPointerBaseRadius: 86,
      boostPointerHalfAngleTenths: 35, fixedBaroKpa: 99.4, boostOffsetBar: 0,
      boostMinBar: -1, boostMaxBar: 1.5, boostWarningBar: 0.9,
      boostDangerBar: 1.15, smoothingMs: 250,
    },
    fuel: {
      fuelSource: 0, petrolCorrection: 1.0, lpgCorrection: 1.2, lpgAfr: 15.5,
      lpgDensity: 540, switchSpeedKph: 5, saveTrip: true, dfcoEnabled: true,
      fuelTrimEnabled: true,
    },
    lpg: {
      lpgEnabled: true, lpgActiveLow: true, lpgDebounceMs: 800,
      lpgOffDelayMs: 1000, lpgOnDelayMs: 0,
    },
    controls: {
      longPressMs: 1800, serviceHoldMs: 5000, serviceTimeoutMin: 15,
      screenOffDelaySec: 60, lockLongWhenMoving: true,
    },
    obd: { obdTimeoutMs: 120, maxRequestsPerSecond: 50 },
    service: { deviceName: 'H2 Gauge', apName: 'H2-Gauge', apPassword: 'h2gauge18' },
    trips: { tripGapMin: 5, tripMinDistanceM: 100, utcOffsetMin: 180 },
  };

  // Build 30 realistic trips so pagination (25 + 5) is genuinely exercised
  let allTrips = [];
  for (let seq = 30; seq >= 1; --seq) {
    allTrips.push({
      sequence: seq,
      startEpochSec: seq === 1 ? 0 : 1792569600 + seq * 3600,
      utcOffsetMin: 180,
      dateUnknown: seq === 1,
      merged: seq === 30,
      localDate: seq === 1 ? '' : '2026-10-21',
      localTime: seq === 1 ? '' : '09:30',
      wallDurationSec: 2100,
      engineSeconds: 1800,
      movingSeconds: 1380,
      idleSeconds: 420,
      preMoveSeconds: 240,
      inTripIdleSeconds: 180,
      distanceKm: 24.5,
      totalLiters: 2.45,
      preMoveLiters: 0.18,
      inTripIdleLiters: 0.14,
      idleLiters: 0.32,
      movingLiters: 2.13,
      averageL100: 10.0,
      movingAverageL100: 8.7,
      idleSharePercent: 13.1,
      idleLitersPerHour: 2.74,
      lpgSharePercent: 85.0,
      avgMovingSpeedKph: 63.9,
      maxSpeedKnown: seq !== 1,
      maxSpeedKph: seq === 1 ? null : 112.4,
      petrolCorrection: 1.0,
      lpgCorrection: 1.2,
      petrol: { rawLiters: 0.35, liters: 0.35, preMoveLiters: 0.18, inTripIdleLiters: 0.02, idleLiters: 0.20, movingLiters: 0.15, distanceKm: 3.7, engineSeconds: 420, standingSeconds: 255, averageL100: 9.5, movingAverageL100: 4.1 },
      lpg: { rawLiters: 1.75, liters: 2.10, preMoveLiters: 0.0, inTripIdleLiters: 0.12, idleLiters: 0.12, movingLiters: 1.98, distanceKm: 20.8, engineSeconds: 1380, standingSeconds: 165, averageL100: 10.1, movingAverageL100: 9.5 },
    });
  }

  let epochHeadersSeen = [];
  let timeSyncPayloads = [];
  let closeCalls = 0;
  let clearActionHeader = null;
  let configSavePosts = 0;

  await page.route('http://h2g.test/**', async route => {
    const req = route.request();
    const url = new URL(req.url());
    const h = req.headers();
    if (h['x-h2g-epoch']) epochHeadersSeen.push(Number(h['x-h2g-epoch']));

    const json = (obj, status = 200) => route.fulfill({
      status, contentType: 'application/json', body: JSON.stringify(obj),
    });

    if (url.pathname === '/') {
      return route.fulfill({ status: 200, contentType: 'text/html', body: html });
    }
    if (url.pathname === '/api/config') {
      if (req.method() === 'POST') {
        storedConfig = req.postDataJSON();
        ++configSavePosts;
        return json({ ok: true, brightnessAppliedLive: true, rebootRecommended: true });
      }
      return json(storedConfig);
    }
    if (url.pathname === '/api/status') {
      return json({
        version: '0.4.31',
        target: 'esp32s3-n16r8',
        obdConnected: true,
        ecuId: '0x7E8',
        engineEcuId: '0x7E8',
        engineEcuLocked: true,
        screenOffReason: 'lit',
        tripIdleSharePercent: 14,
        tripIdleLitersPerHour: 1.05,
        clock: {
          synced: true,
          epochSec: 1792584000,
          utcOffsetMin: storedConfig.trips.utcOffsetMin,
          localDate: '2026-10-21',
          localTime: '15:00',
          storageHealthy: true,
        },
        tripHistory: {
          ready: true,
          healthy: true,
          headSequence: allTrips.length,
          badSlots: 0,
          writes: allTrips.length,
          failures: 0,
          capacity: 2048,
        },
        tripBreakdown: {
          distanceKm: 12.6,
          activeFuel: 'lpg',
          preMoveSeconds: 195,
          preMoveLiters: 0.16,
          inTripIdleSeconds: 90,
          inTripIdleLiters: 0.07,
          avgMovingSpeedKph: 58.2,
          maxSpeedKnown: true,
          maxSpeedKph: 104.8,
          petrol: { liters: 0.24, idleLiters: 0.16, distanceKm: 1.6, averageL100: 15.0, movingAverageL100: 5.0, idleSharePercent: 67, engineSeconds: 300 },
          lpg: { liters: 1.18, idleLiters: 0.07, distanceKm: 11.0, averageL100: 10.7, movingAverageL100: 10.1, idleSharePercent: 6, engineSeconds: 690 },
        },
      });
    }
    if (url.pathname === '/api/trips') {
      const offset = Number(url.searchParams.get('offset') || 0);
      const limit = Number(url.searchParams.get('limit') || 25);
      const slice = allTrips.slice(offset, offset + limit);
      return json({
        ready: true,
        healthy: true,
        headSequence: allTrips.length,
        capacity: 2048,
        offset,
        limit,
        total: allTrips.length,
        hasMore: offset + slice.length < allTrips.length,
        corruptTail: false,
        trips: slice,
      });
    }
    if (url.pathname === '/api/trips/days') {
      return json({
        ready: true,
        healthy: true,
        limit: 60,
        totalDays: 2,
        totalTrips: allTrips.length,
        hasMore: false,
        corruptTail: false,
        days: [
          {
            date: '2026-10-21', dateUnknown: false, tripCount: 29, mergedCount: 1,
            wallDurationSec: 60900, engineSeconds: 52200, movingSeconds: 40020,
            idleSeconds: 12180, preMoveSeconds: 6960, inTripIdleSeconds: 5220,
            distanceKm: 710.5, totalLiters: 71.05, petrolLiters: 10.15, lpgLiters: 60.9,
            preMoveLiters: 5.22, inTripIdleLiters: 4.06, idleLiters: 9.28, movingLiters: 61.77,
            averageL100: 10.0, movingAverageL100: 8.7, idleSharePercent: 13.1,
            idleLitersPerHour: 2.74, lpgSharePercent: 85.0, avgMovingSpeedKph: 63.9,
            maxSpeedKnown: true, maxSpeedKph: 112.4,
          },
          {
            date: '', dateUnknown: true, tripCount: 1, mergedCount: 0,
            wallDurationSec: 2100, engineSeconds: 1800, movingSeconds: 1380,
            idleSeconds: 420, preMoveSeconds: 240, inTripIdleSeconds: 180,
            distanceKm: 24.5, totalLiters: 2.45, petrolLiters: 0.35, lpgLiters: 2.10,
            preMoveLiters: 0.18, inTripIdleLiters: 0.14, idleLiters: 0.32, movingLiters: 2.13,
            averageL100: 10.0, movingAverageL100: 8.7, idleSharePercent: 13.1,
            idleLitersPerHour: 2.74, lpgSharePercent: 85.0, avgMovingSpeedKph: 63.9,
            maxSpeedKnown: false, maxSpeedKph: null,
          },
        ],
      });
    }
    if (url.pathname === '/api/time' && req.method() === 'POST') {
      const body = req.postDataJSON();
      timeSyncPayloads.push(body);
      return json({
        ok: true, synced: true, epochSec: body.epochSec,
        utcOffsetMin: storedConfig.trips.utcOffsetMin,
        localDate: '2026-10-21', localTime: '15:01',
      });
    }
    if (url.pathname === '/api/trip/close' && req.method() === 'POST') {
      ++closeCalls;
      allTrips.unshift({ ...allTrips[0], sequence: allTrips.length + 1, merged: false });
      return json({ ok: true, sequence: allTrips.length });
    }
    if (url.pathname === '/api/trips/clear' && req.method() === 'POST') {
      clearActionHeader = h['x-h2g-action'] || null;
      allTrips = [];
      return json({ ok: true });
    }
    return json({});
  });

  await page.goto('http://h2g.test/');
  await page.waitForFunction(() => document.getElementById('tripPreMove').textContent !== '—');

  // 1. Pure UTC check: X-H2G-Epoch matches real Date.now()/1000 within 5s even under America/New_York!
  const browserNowSec = Math.floor(Date.now() / 1000);
  assert.ok(epochHeadersSeen.length > 0, 'X-H2G-Epoch header must be sent automatically');
  assert.ok(Math.abs(epochHeadersSeen[0] - browserNowSec) <= 5,
    'X-H2G-Epoch must carry pure UTC Unix seconds unaffected by browser timezone');

  // 2. Live open-trip warm-up, traffic idle, moving avg speed, and true peak speed
  await page.click('.tab[data-tab="fuel"]');
  assert.match(await page.textContent('#tripPreMove'), /0\.16 л/);
  assert.match(await page.textContent('#tripInTripIdle'), /0\.07 л/);
  assert.equal(await page.textContent('#tripMaxSpeed'), '104.8 км/ч');

  // 3. Switch to Trips tab -> loads 25 of 30 trips, shows warm-up, peak speed & "Показать ещё"
  await page.click('.tab[data-tab="trips"]');
  await page.waitForFunction(() => document.querySelectorAll('#tripsHistoryRows tr').length === 25);
  assert.equal(await page.isHidden('#moreTripsBtn'), false, 'more button shown when 30 > 25');
  const firstRowText = await page.textContent('#tripsHistoryRows tr:first-child');
  assert.match(firstRowText, /#30/);
  assert.match(firstRowText, /объединена/);
  assert.match(firstRowText, /0\.18 л/);
  assert.match(firstRowText, /Макс\.: 112\.4 км\/ч/);

  // 4. Load next page -> all 30 shown, oldest #1 shows "Дата неизвестна" and "Макс.: —"
  await page.click('#moreTripsBtn');
  await page.waitForFunction(() => document.querySelectorAll('#tripsHistoryRows tr').length === 30);
  assert.equal(await page.isHidden('#moreTripsBtn'), true, 'more button hides when all loaded');
  const lastRowText = await page.textContent('#tripsHistoryRows tr:last-child');
  assert.match(lastRowText, /#1/);
  assert.match(lastRowText, /Дата неизвестна/);
  assert.match(lastRowText, /Макс\.: —/);

  // 5. Switch to Day summaries -> renders both dated day and "Дата неизвестна" bucket
  await page.click('#tripsModeDays');
  await page.waitForSelector('#tripsDaysView', { state: 'visible' });
  await page.waitForFunction(() => document.querySelectorAll('#tripDaysRows tr').length === 2);
  const dayText = await page.textContent('#tripDaysRows');
  assert.match(dayText, /2026-10-21/);
  assert.match(dayText, /5\.22 л/);
  assert.match(dayText, /Дата неизвестна/);

  // 6. Explicit UTC sync button sends ONLY {epochSec} and NEVER alters device utcOffsetMin (180)!
  await page.click('#syncClockBtn');
  await page.waitForFunction(() => document.getElementById('msg').textContent.includes('Время UTC синхронизировано'));
  assert.equal(timeSyncPayloads.length, 1);
  assert.deepEqual(Object.keys(timeSyncPayloads[0]), ['epochSec']);
  assert.equal(await page.inputValue('#utcOffsetMin'), '180',
    'device utcOffsetMin MUST stay +180 despite America/New_York browser timezone!');

  // 7. Manual close button archives current trip
  await page.click('#tripsModeTrips');
  await page.click('#closeTripBtn');
  await page.waitForFunction(() => document.getElementById('msg').textContent.includes('#31'));
  assert.equal(closeCalls, 1);

  // 8. Negative validation rejects invalid bounds before sending POST /api/config
  await page.fill('#tripGapMin', '0');
  await page.click('#save');
  assert.equal(configSavePosts, 0, 'invalid tripGapMin=0 must be rejected client-side');

  await page.fill('#tripGapMin', '10');
  await page.fill('#tripMinDistanceM', '250');
  await page.fill('#utcOffsetMin', '300');
  assert.equal(await page.textContent('#utcOffsetLabel'), 'UTC+05:00');
  await page.click('#save');
  await page.waitForFunction(() => document.getElementById('msg').textContent.includes('Сохранено'));
  assert.equal(configSavePosts, 1);
  assert.deepEqual(storedConfig.trips, {
    tripGapMin: 10, tripMinDistanceM: 250, utcOffsetMin: 300,
  });

  // 9. Protected clear sends X-H2G-Action: trips-clear
  await page.click('#clearTripsBtn');
  await page.waitForFunction(() => document.getElementById('msg').textContent.includes('Архив поездок очищен'));
  assert.equal(clearActionHeader, 'trips-clear');

  // 10. Zero horizontal overflow on both 360px mobile and 1280px desktop viewports
  for (const width of [360, 1280]) {
    await page.setViewportSize({ width, height: 800 });
    const overflow = await page.evaluate(() =>
      document.documentElement.scrollWidth > document.documentElement.clientWidth);
    assert.equal(overflow, false, `no horizontal overflow on Trips tab at ${width}px`);
  }

  console.log('PASS trip history UI: pure-UTC sync under foreign TZ, warm-up/idle/peak, pagination, day summary, close/clear, config bounds and 360-1280px layout');
  await browser.close();
})().catch(err => {
  console.error(err);
  process.exit(1);
});
