#include "app_config.h"
#include "brightness_manager.h"
#include "input_manager.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <vector>

uint32_t hostMillis = 0;
uint16_t hostAdc = 1800;
int hostButton = HIGH;
std::map<std::string, std::vector<uint8_t>> Preferences::storage;
bool Preferences::failWrite = false;
bool Preferences::failBegin = false;
size_t Preferences::writes = 0;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

namespace {
int passed = 0;
void test(const char* name, const std::function<void()>& body) {
  Preferences::storage.clear();
  Preferences::failWrite = Preferences::failBegin = false;
  Preferences::writes = 0;
  hostMillis = 0;
  hostAdc = 1800;
  hostButton = HIGH;
  body();
  std::printf("PASS %s\n", name);
  ++passed;
}

bool near(float a, float b, float tolerance = 0.1f) {
  return std::fabs(a - b) <= tolerance;
}

struct Rig {
  BrightnessSettings settings;
  BrightnessLogic logic;
  uint32_t now;
  Rig(uint32_t start = 0) : now(start) {
    settings.mode = BrightnessMode::Auto;
    settings.autoCalibrate = false;
    logic.begin(now, settings, 82, 26);
  }
  void first(uint16_t adc) { logic.sample(now, adc, settings, 82, 26); }
  void run(uint32_t duration, uint16_t adc) {
    for (uint32_t elapsed = 0; elapsed < duration; elapsed += 50) {
      const float previous = logic.currentPercent();
      now += 50;
      logic.sample(now, adc, settings, 82, 26);
      CHECK(std::isfinite(logic.currentPercent()));
      CHECK(logic.currentPercent() >= 26 && logic.currentPercent() <= 82);
      CHECK(std::fabs(logic.currentPercent() - previous) <= 1.251f);
    }
  }
};

struct ButtonRig {
  OneButton button;
  ConfigData config = ConfigStore::defaults();
  std::vector<ButtonEvent> events;
  ButtonRig(uint32_t now = 0) {
    hostMillis = now;
    hostButton = HIGH;
    config.brightness.mode = BrightnessMode::Manual;
    button.begin();
  }
  void run(uint32_t ms, bool down) {
    for (uint32_t t = 0; t < ms; t += 10) {
      hostMillis += 10;
      hostButton = down ? LOW : HIGH;
      button.update(hostMillis, config);
      const ButtonEvent e = button.takeEvent();
      if (e != ButtonEvent::None) events.push_back(e);
    }
  }
  void click() { run(100, true); run(100, false); }
};

uint32_t fnv(const uint8_t* b, size_t len) {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < len; ++i) { h ^= b[i]; h *= 16777619UL; }
  return h;
}

std::vector<uint8_t> v5Record(bool inverted = false) {
  ConfigData c = ConfigStore::defaults();
  c.schemaVersion = 5;
  c.brightnessDay = inverted ? 20 : 73;
  c.brightnessNight = inverted ? 50 : 19;
  c.petrolCorrection = 1.18f;
  c.lpgCorrection = 1.31f;
  c.speedCorrectionKph = -4.5f;
  c.lpgEnabled = true;
  c.colorText = 0x1234;
  c.centerValue = MainCenterValue::AverageConsumption;
  c.language = UiLanguage::English;
  c.startPage = 2;
  c.dfcoCorrectionEnabled = false;
  strlcpy(c.apPassword, "preserve-this-password", sizeof(c.apPassword));
  std::vector<uint8_t> result(200);
  std::memcpy(result.data(), &c, 196);
  const uint32_t hash = fnv(result.data(), 196);
  std::memcpy(result.data() + 196, &hash, 4);
  return result;
}

void managerRun(BrightnessManager& m, const ConfigData& c, uint32_t ms, uint16_t adc) {
  hostAdc = adc;
  for (uint32_t t = 0; t < ms; t += 50) {
    hostMillis += 50;
    m.update(hostMillis, c);
  }
}
}  // namespace

int main() {
  test("settings constraints and safe default", [] {
    auto c = ConfigStore::defaults();
    CHECK(c.brightness.mode == BrightnessMode::AlwaysDay);
    CHECK(BrightnessLogic::validSettings(c.brightness, 82, 26));
    CHECK(!BrightnessLogic::validSettings(c.brightness, 20, 26));
    c.brightness.dayAdc = c.brightness.nightAdc + 199;
    CHECK(!BrightnessLogic::validSettings(c.brightness, 82, 26));
    ++c.brightness.dayAdc;
    CHECK(BrightnessLogic::validSettings(c.brightness, 82, 26));
    c.brightness.dimDelayMs = 30001;
    CHECK(!BrightnessLogic::validSettings(c.brightness, 82, 26));
    c.brightness.dimDelayMs = 30000;
    c.brightness.mode = static_cast<BrightnessMode>(4);
    CHECK(!BrightnessLogic::validSettings(c.brightness, 82, 26));
  });
  test("fixed day/night and remembered manual level ignore ADC", [] {
    for (const auto mode : {BrightnessMode::AlwaysDay, BrightnessMode::AlwaysNight,
                            BrightnessMode::Manual}) {
      for (const bool night : {false, true}) {
        BrightnessSettings s;
        s.mode = mode; s.manualNight = night;
        BrightnessLogic logic;
        logic.begin(0, s, 82, 26);
        const float expected = mode == BrightnessMode::AlwaysNight ||
            (mode == BrightnessMode::Manual && night) ? 26 : 82;
        for (uint32_t now = 0; now < 12000; now += 50) {
          logic.sample(now, now < 6000 ? 0 : 4095, s, 82, 26);
          CHECK(near(logic.currentPercent(), expected));
          CHECK(!logic.calibration().hasSamples);
        }
      }
    }
  });
  test("Auto startup uses the first ADC instead of flashing daylight level", [] {
    BrightnessSettings s; s.mode = BrightnessMode::Auto; s.autoCalibrate = false;
    for (const uint16_t adc : {200, 1800, 3800}) {
      BrightnessLogic logic;
      logic.begin(0, s, 82, 26, LightCalibration{}, adc);
      CHECK(logic.hasSample());
      const float expected = adc == 200 ? 26 : adc == 1800 ? 54 : 82;
      CHECK(near(logic.currentPercent(), expected));
      CHECK(logic.delayRemainingMs(0) == 0);
    }
    auto c = ConfigStore::defaults(); c.brightness.mode = BrightnessMode::Auto;
    hostAdc = 200; BrightnessManager manager; CHECK(manager.begin(0, c));
    CHECK(near(manager.state().currentPercent(), 26));
  });
  test("linear interpolation and exact endpoints", [] {
    for (const uint16_t adc : {0, 600, 1200, 1800, 2400, 3000, 4095}) {
      Rig r; r.settings.dimDelayMs = r.settings.brightenDelayMs = 0;
      r.first(adc); r.run(12000, adc);
      const float fraction = std::max(0.0f, std::min(1.0f, (adc - 600) / 2400.0f));
      CHECK(near(r.logic.targetPercent(), 26 + fraction * 56));
      CHECK(near(r.logic.currentPercent(), 26 + fraction * 56));
      CHECK(r.logic.pwmDuty() >= 66 && r.logic.pwmDuty() <= 210);
    }
  });
  test("dimming delay and bounded PWM ramp", [] {
    Rig r; r.first(200); r.run(2950, 200);
    CHECK(near(r.logic.targetPercent(), 82));
    CHECK(r.logic.delayRemainingMs(r.now) == 50);
    r.run(50, 200);
    CHECK(near(r.logic.targetPercent(), 26));
    CHECK(r.logic.currentPercent() > 80);
    r.run(3000, 200);
    CHECK(near(r.logic.currentPercent(), 26));
  });
  test("brightening has its own delay", [] {
    Rig r; r.first(200); r.run(10000, 200);
    r.run(750, 3800);
    CHECK(near(r.logic.targetPercent(), 26));
    r.run(9000, 3800);
    CHECK(near(r.logic.currentPercent(), 82));
  });
  test("median rejects spikes, ADC/output deadbands suppress noise", [] {
    Rig r; r.first(1800); r.run(10000, 1800);
    const float baseline = r.logic.currentPercent();
    for (int i = 0; i < 10; ++i) {
      r.run(50, 4095); r.run(250, 1800);
      r.run(50, 0); r.run(250, 1800);
    }
    CHECK(near(r.logic.currentPercent(), baseline));
    for (int i = 0; i < 200; ++i) r.run(50, i % 2 ? 1780 : 1820);
    CHECK(near(r.logic.currentPercent(), baseline));
  });
  test("brief shadow does not dim; canceled delays do not accumulate", [] {
    Rig r; r.first(3500); r.run(5000, 3500);
    for (int i = 0; i < 8; ++i) {
      r.run(300, 200); r.run(2000, 3500);
      CHECK(near(r.logic.currentPercent(), 82));
    }
  });
  test("sample gap restarts qualification without catch-up", [] {
    Rig r; r.first(3500); r.run(5000, 3500);
    r.now += 60000;
    r.logic.sample(r.now, 200, r.settings, 82, 26);
    CHECK(near(r.logic.currentPercent(), 82));
    CHECK(r.logic.delayRemainingMs(r.now) == 3000);
    r.run(6000, 200); CHECK(near(r.logic.currentPercent(), 26));
  });
  test("millis rollover preserves filter, delay and ramp", [] {
    Rig r(std::numeric_limits<uint32_t>::max() - 1200);
    r.first(200); r.run(2950, 200);
    CHECK(near(r.logic.currentPercent(), 82));
    r.run(3100, 200); CHECK(near(r.logic.currentPercent(), 26));
    r.run(8000, 3800); CHECK(near(r.logic.currentPercent(), 82));
  });
  test("constant/narrow light never becomes a calibrated full range", [] {
    Rig r; r.settings.autoCalibrate = true;
    r.first(1800); r.run(60000, 1800);
    CHECK(r.logic.calibration().hasSamples);
    CHECK(!r.logic.calibrationReady());
    CHECK(r.logic.nightThreshold() == 600 && r.logic.dayThreshold() == 3000);
    r.run(15000, 2100); CHECK(!r.logic.calibrationReady());
  });
  test("stable extrema train thresholds; range only expands", [] {
    Rig r; r.settings.autoCalibrate = true;
    r.first(500); r.run(10000, 500); r.run(20000, 3500);
    CHECK(r.logic.calibrationReady());
    CHECK(r.logic.learnedThresholdsActive());
    const auto range = r.logic.calibration();
    const uint16_t margin = (range.maxAdc - range.minAdc) * 15UL / 100;
    CHECK(r.logic.nightThreshold() == range.minAdc + margin);
    CHECK(r.logic.dayThreshold() == range.maxAdc - margin);
    r.run(60000, 1800);
    CHECK(r.logic.calibration().minAdc == range.minAdc);
    CHECK(r.logic.calibration().maxAdc == range.maxAdc);
    r.settings.autoCalibrate = false; r.run(1000, 1800);
    CHECK(r.logic.nightThreshold() == 600 && r.logic.dayThreshold() == 3000);
    CHECK(r.logic.calibrationReady());
  });
  test("ADC rails drive endpoint levels but cannot train calibration", [] {
    Rig r; r.settings.autoCalibrate = true;
    r.first(0); r.run(15000, 0); CHECK(near(r.logic.currentPercent(), 26));
    r.run(15000, 4095); CHECK(near(r.logic.currentPercent(), 82));
    CHECK(!r.logic.calibration().hasSamples);
  });
  test("reset restores base thresholds without instant brightness jump", [] {
    Rig r; r.settings.autoCalibrate = true;
    r.first(500); r.run(10000, 500); r.run(20000, 3500);
    CHECK(r.logic.calibrationReady());
    const float percent = r.logic.currentPercent();
    r.logic.resetCalibration(r.now);
    CHECK(!r.logic.calibrationReady());
    CHECK(r.logic.nightThreshold() == 600 && r.logic.dayThreshold() == 3000);
    CHECK(near(r.logic.currentPercent(), percent));
  });
  test("manual live changes fade without auto delays", [] {
    Rig r; r.settings.mode = BrightnessMode::Manual; r.first(2000);
    r.settings.manualNight = true; r.run(50, 2000);
    CHECK(near(r.logic.targetPercent(), 26)); CHECK(r.logic.currentPercent() < 82);
    r.run(3000, 2000); CHECK(near(r.logic.currentPercent(), 26));
    r.settings.manualNight = false; r.run(3000, 2000);
    CHECK(near(r.logic.currentPercent(), 82));
  });
  test("four quick presses emit only QuadPress", [] {
    ButtonRig r;
    for (int i = 0; i < 4; ++i) r.click();
    r.run(1000, false);
    CHECK(r.events.size() == 1 && r.events[0] == ButtonEvent::QuadPress);
  });
  test("one, two and three clicks keep all page actions after timeout", [] {
    for (int n = 1; n <= 3; ++n) {
      ButtonRig r;
      for (int i = 0; i < n; ++i) r.click();
      CHECK(r.events.empty());
      r.run(600, false);
      CHECK(r.events.size() == static_cast<size_t>(n));
      for (const auto e : r.events) CHECK(e == ButtonEvent::ShortPress);
    }
  });
  test("four presses outside manual are normal navigation, not override", [] {
    ButtonRig r; r.config.brightness.mode = BrightnessMode::Auto;
    for (int i = 0; i < 4; ++i) r.click();
    CHECK(r.events.size() == 4);
    for (const auto e : r.events) CHECK(e == ButtonEvent::ShortPress);
  });
  test("long and service holds cancel pending clicks, never trigger quadruple", [] {
    ButtonRig r; r.click(); r.run(2000, true); r.run(600, false);
    CHECK(r.events.size() == 1 && r.events[0] == ButtonEvent::LongPress);
    r.events.clear(); r.click(); r.run(7000, true); r.run(600, false);
    CHECK(r.events.size() == 1 && r.events[0] == ButtonEvent::ServiceHold);
  });
  test("debounce and rollover on button; last press may cross gap while debouncing", [] {
    ButtonRig r(std::numeric_limits<uint32_t>::max() - 500);
    for (int i = 0; i < 10; ++i) { r.run(20, true); r.run(20, false); }
    CHECK(r.events.empty());
    for (int i = 0; i < 3; ++i) r.click();
    r.run(310, false); // raw next press starts 380 ms after debounced release
    r.click(); r.run(600, false);
    CHECK(r.events.size() == 1 && r.events[0] == ButtonEvent::QuadPress);
  });
  test("schema-5 migration preserves settings, trip and fuel calibration namespaces", [] {
    Preferences::storage["h2gauge/config"] = v5Record();
    Preferences::storage["h2trip/trip"] = {1, 2, 3};
    Preferences::storage["h2petcal/state"] = {4, 5, 6};
    ConfigStore store; CHECK(store.begin());
    const auto& c = store.data();
    CHECK(c.schemaVersion == 6 && c.brightnessDay == 73 && c.brightnessNight == 19);
    CHECK(near(c.petrolCorrection, 1.18f) && near(c.lpgCorrection, 1.31f));
    CHECK(near(c.speedCorrectionKph, -4.5f) && c.lpgEnabled);
    CHECK(c.colorText == 0x1234 && c.startPage == 2 && !c.dfcoCorrectionEnabled);
    CHECK(c.language == UiLanguage::English);
    CHECK(c.centerValue == MainCenterValue::AverageConsumption);
    CHECK(std::strcmp(c.apPassword, "preserve-this-password") == 0);
    CHECK(c.brightness.mode == BrightnessMode::AlwaysDay);
    CHECK(Preferences::storage["h2gauge/config"].size() == sizeof(ConfigData));
    CHECK((Preferences::storage["h2trip/trip"] == std::vector<uint8_t>{1,2,3}));
    CHECK((Preferences::storage["h2petcal/state"] == std::vector<uint8_t>{4,5,6}));
  });
  test("legacy inverted levels normalize only night; corrupt records fall back", [] {
    Preferences::storage["h2gauge/config"] = v5Record(true);
    ConfigStore first; CHECK(first.begin());
    CHECK(first.data().brightnessNight == 20 && first.data().brightnessDay == 20);
    CHECK(near(first.data().petrolCorrection, 1.18f));
    auto bad = v5Record(); bad[35] ^= 1;
    Preferences::storage["h2gauge/config"] = bad;
    ConfigStore second; CHECK(second.begin());
    CHECK(second.data().brightnessDay == 82 && near(second.data().petrolCorrection, 1));
  });
  test("schema-6/manual choice round trip, invalid writes do not replace stored config", [] {
    ConfigStore store; CHECK(store.begin());
    store.data().brightness.mode = BrightnessMode::Manual;
    store.data().brightness.manualNight = true;
    store.data().brightness.dimDelayMs = 4321;
    CHECK(store.save());
    const auto record = Preferences::storage["h2gauge/config"];
    ConfigStore reloaded; CHECK(reloaded.begin());
    CHECK(reloaded.data().brightness.manualNight);
    CHECK(reloaded.data().brightness.dimDelayMs == 4321);
    CHECK(BrightnessLogic::initialPercent(reloaded.data().brightness, 82, 26) == 26);
    reloaded.data().brightness.dayAdc = 1;
    CHECK(!reloaded.save()); CHECK(Preferences::storage["h2gauge/config"] == record);
    Preferences::failWrite = true;
    store.data().brightness.manualNight = false;
    CHECK(!store.save()); CHECK(Preferences::storage["h2gauge/config"] == record);
  });
  test("calibration storage is explicit 16-byte LE, wear-limited and recoverable", [] {
    auto c = ConfigStore::defaults(); c.brightness.mode = BrightnessMode::Auto;
    hostAdc = 500; BrightnessManager m; CHECK(m.begin(hostMillis, c));
    managerRun(m, c, 10000, 500); managerRun(m, c, 20000, 3500);
    CHECK(m.state().calibrationReady()); CHECK(Preferences::writes == 0);
    managerRun(m, c, 570000, 1800);
    CHECK(Preferences::writes == 1);
    const auto b = Preferences::storage["h2light/range"];
    CHECK(b.size() == 16 && b[0] == 'H' && b[1] == '2' && b[4] == 1);
    CHECK(m.checkpoint()); CHECK(Preferences::writes == 1);
    managerRun(m, c, 600000, 1800); CHECK(Preferences::writes == 1);
    BrightnessManager restored; CHECK(restored.begin(hostMillis, c));
    CHECK(restored.state().calibrationReady());
    CHECK(restored.state().calibration().minAdc == m.state().calibration().minAdc);
    Preferences::failWrite = true;
    CHECK(!restored.resetCalibration(hostMillis));
    CHECK(restored.state().calibrationReady());
    CHECK(!restored.storageHealthy());
    Preferences::failWrite = false;
    CHECK(restored.resetCalibration(hostMillis));
    CHECK(!restored.state().calibrationReady());
    BrightnessManager empty; CHECK(empty.begin(hostMillis, c));
    CHECK(!empty.state().calibration().hasSamples);
  });
  test("damaged/unknown calibration record and unavailable NVS remain usable", [] {
    auto c = ConfigStore::defaults();
    Preferences::storage["h2light/range"] = std::vector<uint8_t>(16, 0xA5);
    BrightnessManager m; CHECK(m.begin(hostMillis, c));
    CHECK(!m.state().calibration().hasSamples);
    CHECK(near(m.state().currentPercent(), 82));
    Preferences::failBegin = true;
    BrightnessManager missing; CHECK(!missing.begin(hostMillis, c));
    CHECK(!missing.storageHealthy()); CHECK(!missing.checkpoint());
    managerRun(missing, c, 1000, 100); CHECK(near(missing.state().currentPercent(), 82));
  });
  std::printf("\n%d host regression groups passed. Hardware behavior is not simulated.\n", passed);
}
