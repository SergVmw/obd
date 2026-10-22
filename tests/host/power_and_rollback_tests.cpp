// Host regression tests for 0.4.35 live car fixes:
//   1. Gearbox 0x7E9 PID 0x42 = 0x0000 (0.000 V) & PowerManager key-on
//      deep-sleep reboot loop ("resetReason": "deep_sleep", +109 checkpoints).
//   2. Physical button rollback in Service Mode (boot-hold tail suppression,
//      2-click / 3-click fast burst, natural 450-1500 ms car bezel double-tap,
//      ShortPress cancel before arming, and both LongPress and ServiceHold
//      confirming reboot after "ДЛИННОЕ - ПЕРЕЗАПУСК").

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "Arduino.h"
#include "LittleFS.h"
#include "Preferences.h"
#include "app_config.h"
#include "driver/twai.h"
#include "input_manager.h"
#include "obd_client.h"
#include "power_manager.h"
#include "service_rollback_ui.h"
#include "telemetry.h"

uint32_t hostMillis = 0;
uint16_t hostAdc = 2000;
int hostButton = HIGH;
std::map<std::string, std::vector<uint8_t>> Preferences::storage;
bool Preferences::failWrite = false;
bool Preferences::failBegin = false;
size_t Preferences::writes = 0;
HostLittleFS LittleFS;
namespace hostfs {
size_t maxWritePerCall = std::numeric_limits<size_t>::max();
bool failFlush = false;
size_t flushes = 0;
}  // namespace hostfs

namespace h2g_twai_stub {
void reset();
void queueRx(const twai_message_t& message);
const std::vector<twai_message_t>& transmitted();
void clearTransmitted();
}  // namespace h2g_twai_stub

namespace {

int gTestsRun = 0;
int gFailures = 0;

#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);   \
      ++gFailures;                                                           \
    }                                                                        \
  } while (0)

template <typename Fn>
void test(const char* name, Fn fn) {
  ++gTestsRun;
  fn();
  std::printf("  [OK] %s\n", name);
}

twai_message_t canFrame(uint32_t id, uint8_t b0, uint8_t b1, uint8_t b2,
                        uint8_t b3, uint8_t b4 = 0xAA, uint8_t b5 = 0xAA,
                        uint8_t b6 = 0xAA, uint8_t b7 = 0xAA) {
  twai_message_t msg = {};
  msg.identifier = id;
  msg.data_length_code = 8;
  msg.data[0] = b0;
  msg.data[1] = b1;
  msg.data[2] = b2;
  msg.data[3] = b3;
  msg.data[4] = b4;
  msg.data[5] = b5;
  msg.data[6] = b6;
  msg.data[7] = b7;
  return msg;
}

struct ButtonSim {
  ConfigData config;
  OneButton button;
  uint32_t now = 1000;
  std::vector<ButtonEvent> events;

  ButtonSim() {
    ConfigStore store;
    store.begin();
    config = store.data();
    hostMillis = now;
    hostButton = HIGH;
    button.begin();
  }

  void holdRaw(uint32_t ms, bool pressed) {
    hostButton = pressed ? LOW : HIGH;
    const uint32_t end = now + ms;
    while (static_cast<int32_t>(end - now) > 0) {
      now += 5;
      hostMillis = now;
      button.update(now, config);
      while (true) {
        const ButtonEvent ev = button.takeEvent();
        if (ev == ButtonEvent::None) break;
        events.push_back(ev);
      }
    }
  }

  void tap(uint32_t downMs = 80, uint32_t upMs = 90) {
    holdRaw(downMs, true);
    holdRaw(upMs, false);
  }
};

}  // namespace

int main() {
  std::printf("Running power_and_rollback_tests...\n");

  test("ObdClient rejects 0x7E9 gearbox zero-voltage PID 0x42 and out-of-range frames", [] {
    Preferences::storage.clear();
    h2g_twai_stub::reset();
    ConfigStore store;
    CHECK(store.begin());
    TelemetryData tel;
    CanMonitor monitor;
    ObdClient client(tel, store.data(), monitor);
    CHECK(client.begin());

    uint32_t now = 100;
    // Pump until ObdClient requests PID 0x42, feeding real Haval H2 masks:
    // 0x7E8 has NO 0x42; 0x7E9 advertises 0x42 (0x40 0x80 0x00 0x00).
    bool sawPid42Request = false;
    for (int step = 0; step < 400 && !sawPid42Request; ++step) {
      now += 25;
      hostMillis = now;
      client.loop(now);
      for (const twai_message_t& tx : h2g_twai_stub::transmitted()) {
        if (tx.data[1] == 0x01 && tx.data[2] == 0x00) {
          h2g_twai_stub::queueRx(canFrame(0x7E9, 0x06, 0x41, 0x00, 0x98, 0x18, 0x00, 0x01));
        } else if (tx.data[1] == 0x01 && tx.data[2] == 0x20) {
          h2g_twai_stub::queueRx(canFrame(0x7E8, 0x06, 0x41, 0x20, 0x80, 0x05, 0x80, 0x01));
          h2g_twai_stub::queueRx(canFrame(0x7E9, 0x06, 0x41, 0x20, 0x80, 0x01, 0x80, 0x01));
        } else if (tx.data[1] == 0x01 && tx.data[2] == 0x40) {
          h2g_twai_stub::queueRx(canFrame(0x7E8, 0x06, 0x41, 0x40, 0x0A, 0x10, 0x00, 0x00));
          h2g_twai_stub::queueRx(canFrame(0x7E9, 0x06, 0x41, 0x40, 0x40, 0x80, 0x00, 0x00));
        } else if (tx.data[1] == 0x01 && tx.data[2] == 0x42) {
          sawPid42Request = true;
          // Gearbox answers 0x0000 (0.000 V) at key-on with engine off!
          h2g_twai_stub::queueRx(canFrame(0x7E9, 0x04, 0x41, 0x42, 0x00, 0x00));
        }
      }
      h2g_twai_stub::clearTransmitted();
    }
    CHECK(sawPid42Request);
    now += 10;
    hostMillis = now;
    client.loop(now);
    CHECK(!tel.ecuVoltage.valid(now));

    // Boundary below 6.000 V (5999 mV = 0x176F) is also rejected.
    for (int step = 0; step < 300; ++step) {
      now += 25;
      hostMillis = now;
      client.loop(now);
      for (const twai_message_t& tx : h2g_twai_stub::transmitted()) {
        if (tx.data[1] == 0x01 && tx.data[2] == 0x42) {
          h2g_twai_stub::queueRx(canFrame(0x7E9, 0x04, 0x41, 0x42, 0x17, 0x6F));
          step = 300;
        }
      }
      h2g_twai_stub::clearTransmitted();
    }
    now += 10;
    hostMillis = now;
    client.loop(now);
    CHECK(!tel.ecuVoltage.valid(now));

    // Out-of-range high garbage (20001 mV = 0x4E21 and 65535 mV = 0xFFFF) rejected too.
    for (uint16_t raw : {static_cast<uint16_t>(20001U), static_cast<uint16_t>(0xFFFFU)}) {
      for (int step = 0; step < 300; ++step) {
        now += 25;
        hostMillis = now;
        client.loop(now);
        for (const twai_message_t& tx : h2g_twai_stub::transmitted()) {
          if (tx.data[1] == 0x01 && tx.data[2] == 0x42) {
            h2g_twai_stub::queueRx(canFrame(
                0x7E9, 0x04, 0x41, 0x42,
                static_cast<uint8_t>(raw >> 8), static_cast<uint8_t>(raw & 0xFF)));
            step = 300;
          }
        }
        h2g_twai_stub::clearTransmitted();
      }
      now += 10;
      hostMillis = now;
      client.loop(now);
      CHECK(!tel.ecuVoltage.valid(now));
    }

    // Valid key-on battery voltage 11.200 V (0x2BC0) IS accepted cleanly!
    for (int step = 0; step < 300; ++step) {
      now += 25;
      hostMillis = now;
      client.loop(now);
      for (const twai_message_t& tx : h2g_twai_stub::transmitted()) {
        if (tx.data[1] == 0x01 && tx.data[2] == 0x42) {
          h2g_twai_stub::queueRx(canFrame(0x7E9, 0x04, 0x41, 0x42, 0x2B, 0xC0));
          step = 300;
        }
      }
      h2g_twai_stub::clearTransmitted();
    }
    now += 10;
    hostMillis = now;
    client.loop(now);
    CHECK(tel.ecuVoltage.valid(now));
    CHECK(std::fabs(tel.ecuVoltage.value - 11.2f) < 0.002f);
  });

  test("PowerManager never deep-sleeps on 0 V, key-on 11.1 V, running engine, or <30 s crank", [] {
    PowerManager pm;
    TelemetryData tel;

    // 1. 0.00 V or 5.99 V garbage for 10 full minutes -> NEVER sleeps.
    for (uint32_t t = 100; t <= 600000; t += 250) {
      tel.ecuVoltage.set(0.0f, t);
      CHECK(!pm.shouldEnterLowVoltageSleep(t, tel));
    }
    for (uint32_t t = 600250; t <= 700000; t += 250) {
      tel.ecuVoltage.set(5.99f, t);
      CHECK(!pm.shouldEnterLowVoltageSleep(t, tel));
    }

    // 2. Normal ignition-ON / engine-OFF battery + TCU load (10.25 V - 11.49 V)
    //    for 10 full minutes -> NEVER sleeps (previously slept every 3.0 s!).
    for (uint32_t t = 1000; t <= 600000; t += 250) {
      tel.rpm.set(0.0f, t);
      tel.ecuVoltage.set(11.1f, t);
      CHECK(!pm.shouldEnterLowVoltageSleep(t, tel));
      tel.ecuVoltage.set(10.25f, t);
      CHECK(!pm.shouldEnterLowVoltageSleep(t, tel));
    }

    // 3. Cold starter cranking dip to 9.4 V for 29.5 s, followed by engine
    //    starting -> NEVER sleeps and resets timer cleanly.
    pm.reset();
    for (uint32_t dt = 0; dt <= 29500; dt += 250) {
      const uint32_t t = 10000 + dt;
      tel.rpm.set(180.0f, t);
      tel.ecuVoltage.set(9.4f, t);
      CHECK(!pm.shouldEnterLowVoltageSleep(t, tel));
    }
    tel.rpm.set(850.0f, 39800);
    tel.ecuVoltage.set(9.4f, 39800);  // Even with faulty regulator while running
    CHECK(!pm.shouldEnterLowVoltageSleep(45000, tel));

    // 4. Genuine depleted battery (9.8 V for >= 30.0 s with engine stopped) DOES
    //    enter emergency deep sleep cleanly.
    pm.reset();
    tel.rpm.set(0.0f, 100000);
    tel.ecuVoltage.set(9.8f, 100000);
    CHECK(!pm.shouldEnterLowVoltageSleep(100000, tel));
    tel.ecuVoltage.set(9.8f, 129999);
    CHECK(!pm.shouldEnterLowVoltageSleep(129999, tel));
    tel.ecuVoltage.set(9.8f, 130000);
    CHECK(pm.shouldEnterLowVoltageSleep(130000, tel));
  });

  test("Boot hold active at OneButton::begin() never emits phantom clicks or exit hold", [] {
    ConfigStore store;
    store.begin();
    const ConfigData config = store.data();
    OneButton btn;
    hostMillis = 3005;
    hostButton = LOW;  // Still held after heldAtBoot(3000)
    btn.begin();
    btn.setMultiClickMode(1);

    // Hold for an extra 6 full seconds and release — zero events emitted!
    uint32_t now = 3005;
    for (; now <= 9200; now += 10) {
      hostMillis = now;
      btn.update(now, config);
      CHECK(btn.takeEvent() == ButtonEvent::None);
    }
    hostButton = HIGH;
    for (; now <= 10000; now += 10) {
      hostMillis = now;
      btn.update(now, config);
      CHECK(btn.takeEvent() == ButtonEvent::None);
    }
  });

  test("Service button rollback works for fast 2x/3x clicks, slow bezel double-tap, and LongPress/ServiceHold reboot", [] {
    // Path A: Fast double click (< 400 ms) -> OpenSelector -> LongPress ->
    //         SelectOtherSlot -> LongPress ("ДЛИННОЕ - ПЕРЕЗАПУСК") -> Reboot!
    {
      ButtonSim sim;
      sim.button.setMultiClickMode(1);
      ServiceRollbackController ctl;
      ctl.reset();

      sim.tap(80, 120);
      sim.tap(80, 500);
      CHECK(sim.events.size() == 1 && sim.events[0] == ButtonEvent::DoublePress);
      CHECK(ctl.handleEvent(sim.now, sim.events[0]) == ServiceRollbackAction::OpenSelector);
      ctl.onSelectorOpened(true);
      sim.events.clear();

      // Step 2: 2.0 s LongPress selects the inactive slot
      sim.holdRaw(2000, true);
      sim.holdRaw(200, false);
      CHECK(sim.events.size() == 1 && sim.events[0] == ButtonEvent::LongPress);
      CHECK(ctl.handleEvent(sim.now, sim.events[0]) == ServiceRollbackAction::SelectOtherSlot);
      ctl.onSlotSelectionResult(true);
      sim.events.clear();

      // Step 3: 2.0 s LongPress (matching on-screen "ДЛИННОЕ - ПЕРЕЗАПУСК"!) reboots!
      sim.holdRaw(2000, true);
      sim.holdRaw(200, false);
      CHECK(sim.events.size() == 1 && sim.events[0] == ButtonEvent::LongPress);
      CHECK(ctl.handleEvent(sim.now, sim.events[0]) == ServiceRollbackAction::RebootToSelectedSlot);
    }

    // Path B: Natural car bezel double-tap (750 ms gap > 400 ms -> two ShortPress
    //         events!) opens selector cleanly, ShortPress cancels back to Service,
    //         re-opening + ServiceHold (5.0 s) on Step 3 ALSO reboots!
    {
      ButtonSim sim;
      sim.button.setMultiClickMode(1);
      ServiceRollbackController ctl;
      ctl.reset();

      sim.tap(90, 750);  // Emits first ShortPress at +400 ms
      CHECK(sim.events.size() == 1 && sim.events[0] == ButtonEvent::ShortPress);
      CHECK(ctl.handleEvent(sim.now, sim.events[0]) == ServiceRollbackAction::None);
      sim.events.clear();

      sim.tap(90, 500);  // Second tap 750 ms after first -> OpenSelector!
      CHECK(sim.events.size() == 1 && sim.events[0] == ButtonEvent::ShortPress);
      CHECK(ctl.handleEvent(sim.now, sim.events[0]) == ServiceRollbackAction::OpenSelector);
      ctl.onSelectorOpened(true);
      sim.events.clear();

      // ShortPress while selector is open cancels back to main Service screen
      sim.tap(90, 500);
      CHECK(sim.events.size() == 1 && sim.events[0] == ButtonEvent::ShortPress);
      CHECK(ctl.handleEvent(sim.now, sim.events[0]) == ServiceRollbackAction::CancelToService);
      CHECK(!ctl.selectorOpen());
      sim.events.clear();

      // Triple rapid click also opens selector (never silently dropped as 3 shorts)
      sim.tap(70, 90);
      sim.tap(70, 90);
      sim.tap(70, 500);
      CHECK(sim.events.size() == 1 && sim.events[0] == ButtonEvent::DoublePress);
      CHECK(ctl.handleEvent(sim.now, sim.events[0]) == ServiceRollbackAction::OpenSelector);
      ctl.onSelectorOpened(true);
      sim.events.clear();

      sim.holdRaw(2000, true);
      sim.holdRaw(200, false);
      CHECK(ctl.handleEvent(sim.now, sim.events[0]) == ServiceRollbackAction::SelectOtherSlot);
      ctl.onSlotSelectionResult(true);
      sim.events.clear();

      // Holding for 5.2 s (ServiceHold) when armed also reboots into other slot
      sim.holdRaw(5200, true);
      CHECK(sim.events.size() == 1 && sim.events[0] == ButtonEvent::ServiceHold);
      CHECK(ctl.handleEvent(sim.now, sim.events[0]) == ServiceRollbackAction::RebootToSelectedSlot);
    }

    // Path C: Isolated single taps > 1500 ms apart NEVER accidentally open selector
    {
      ServiceRollbackController ctl;
      ctl.reset();
      CHECK(ctl.handleEvent(1000, ButtonEvent::ShortPress) == ServiceRollbackAction::None);
      CHECK(ctl.handleEvent(2600, ButtonEvent::ShortPress) == ServiceRollbackAction::None);
      CHECK(!ctl.selectorOpen());

      // UINT32_MAX rollover safety on two taps 600 ms apart across wraparound
      const uint32_t nearMax = std::numeric_limits<uint32_t>::max() - 200U;
      CHECK(ctl.handleEvent(nearMax, ButtonEvent::ShortPress) == ServiceRollbackAction::None);
      CHECK(ctl.handleEvent(nearMax + 600U, ButtonEvent::ShortPress) ==
            ServiceRollbackAction::OpenSelector);
    }
  });

  if (gFailures != 0) {
    std::fprintf(stderr, "%d test check(s) failed.\n", gFailures);
    return 1;
  }
  std::printf("All %d power_and_rollback test suites passed.\n", gTestsRun);
  return 0;
}
