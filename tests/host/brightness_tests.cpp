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
  c.displaySpeedOffsetKph = -4.5f;
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

// A schema-6 record is the current layout without screenOffDelaySec: same prefix, checksum
// four bytes earlier, 212 bytes in total. Values here are deliberately different
// from the factory ones, because the point of the migration is that nothing the
// owner tuned is thrown away.
std::vector<uint8_t> v6Record() {
  ConfigData c = ConfigStore::defaults();
  c.schemaVersion = 6;
  c.brightnessDay = 92;
  c.brightnessNight = 15;
  c.lpgEnabled = true;
  c.lpgCorrection = 1.2f;
  c.displaySpeedOffsetKph = 5.0f;
  c.obdTimeoutMs = 200;
  c.maxRequestsPerSecond = 50;
  c.fuelSource = FuelSource::SpeedDensity;
  c.centerValue = MainCenterValue::CurrentConsumption;
  c.lockLongWhenMoving = false;
  strlcpy(c.apPassword, "keep-this-password", sizeof(c.apPassword));
  std::vector<uint8_t> result(212);
  std::memcpy(result.data(), &c, 208);
  const uint32_t hash = fnv(result.data(), 208);
  std::memcpy(result.data() + 208, &hash, 4);
  return result;
}

void testSchema6RecordKeepsEverySetting() {
  Preferences::storage.clear();
  Preferences::storage["h2gauge/config"] = v6Record();
  ConfigStore store;
  CHECK(store.begin());
  const ConfigData& c = store.data();
  CHECK(c.schemaVersion == H2G_CONFIG_SCHEMA);
  CHECK(c.brightnessDay == 92 && c.brightnessNight == 15);
  CHECK(c.lpgEnabled && near(c.lpgCorrection, 1.2f));
  CHECK(near(c.displaySpeedOffsetKph, 5.0f));
  CHECK(c.obdTimeoutMs == 200 && c.maxRequestsPerSecond == 50);
  CHECK(c.fuelSource == FuelSource::SpeedDensity);
  CHECK(c.centerValue == MainCenterValue::CurrentConsumption);
  CHECK(!c.lockLongWhenMoving);
  CHECK(std::strcmp(c.apPassword, "keep-this-password") == 0);
  // The field the record predates takes its factory value, and the record is
  // rewritten at the current size so the migration runs once.
  CHECK(c.screenOffDelaySec == ConfigStore::defaults().screenOffDelaySec);
  CHECK(Preferences::storage["h2gauge/config"].size() == sizeof(ConfigData));
}

// A schema-7 record is the same length as a schema-8 one: the new enumeration
// landed in padding that already existed between screenOffDelaySec and the
// checksum. The stored version is therefore the only thing telling begin() which
// layout it is holding, and these two cases are what that switch rests on.
std::vector<uint8_t> v7Record(bool poisonPadding) {
  ConfigData c = ConfigStore::defaults();
  c.schemaVersion = 7;
  c.brightnessDay = 64;
  c.screenOffDelaySec = 240;
  c.centerValue = MainCenterValue::Speed;
  c.setMainDigitSize(MainDigitSize::Small);   // lives in the padding byte
  // 216, spelled out rather than sizeof(ConfigData): this is the length the
  // schema-7 and schema-8 builds wrote, and current builds are longer.
  std::vector<uint8_t> result(216);
  std::memcpy(result.data(), &c, 210);
  // 210..211 is the padding the schema-7 build never wrote. Poison it, because a
  // value that merely happens to be Large would prove nothing.
  if (poisonPadding) {
    result[210] = 0xAB;
    result[211] = 0xCD;
  } else {
    result[210] = 0;
    result[211] = 0;
  }
  const uint32_t hash = fnv(result.data(), 212);
  std::memcpy(result.data() + 212, &hash, 4);
  return result;
}

void testSchema7RecordKeepsSettingsAndTakesFactoryDigitSize() {
  Preferences::storage.clear();
  Preferences::storage["h2gauge/config"] = v7Record(false);
  ConfigStore store;
  CHECK(store.begin());
  const ConfigData& c = store.data();
  CHECK(c.schemaVersion == H2G_CONFIG_SCHEMA);
  CHECK(c.brightnessDay == 64);
  CHECK(c.screenOffDelaySec == 240);
  CHECK(c.centerValue == MainCenterValue::Speed);
  // The record's own value sat in padding. The firmware supplies its own, because
  // reading back padding is how an out-of-range enumeration would reach drawMain.
  CHECK(c.mainDigit() == MainDigitSize::Large);
  CHECK(Preferences::storage["h2gauge/config"].size() == sizeof(ConfigData));
}

void testPaddingGarbageNeverBecomesTheDigitSize() {
  Preferences::storage.clear();
  Preferences::storage["h2gauge/config"] = v7Record(true);
  ConfigStore store;
  CHECK(store.begin());
  // 0xAB is not a valid MainDigitSize. Either the factory value or a refusal to
  // restore at all is acceptable; a stored 0xAB is not.
  CHECK(store.data().mainDigit() != static_cast<MainDigitSize>(0xAB));
  CHECK(static_cast<uint8_t>(store.data().mainDigit()) <=
        static_cast<uint8_t>(MainDigitSize::Small));
}

// A schema-8 record is the same length as a schema-7 one and differs in what
// the last four bytes cover: a real checksum over mainDigitSize rather than a
// checksum over padding. It is the layout the 0.4.20 to 0.4.24 builds wrote, so
// it is the one an upgrade in the car actually meets.
std::vector<uint8_t> v8Record() {
  ConfigData c = ConfigStore::defaults();
  c.schemaVersion = 8;
  c.brightnessDay = 71;
  c.lpgEnabled = true;
  c.lpgCorrection = 1.2f;
  c.setMainDigitSize(MainDigitSize::Small);
  c.screenOffDelaySec = 120;
  strlcpy(c.apPassword, "schema-eight-password", sizeof(c.apPassword));
  std::vector<uint8_t> result(216);
  std::memcpy(result.data(), &c, 212);
  const uint32_t hash = fnv(result.data(), 212);
  std::memcpy(result.data() + 212, &hash, 4);
  return result;
}

void testSchema8RecordKeepsSettingsAndTakesFactorySwitchOnDelay() {
  Preferences::storage.clear();
  Preferences::storage["h2gauge/config"] = v8Record();
  ConfigStore store;
  CHECK(store.begin());
  const ConfigData& c = store.data();
  CHECK(c.schemaVersion == H2G_CONFIG_SCHEMA);
  // Everything the record held, the digit size included: it was padding in
  // schema 7 and is a real field in schema 8, so this is the case that proves
  // the two 216-byte layouts are told apart by the stored version and not by
  // their length.
  CHECK(c.brightnessDay == 71);
  CHECK(c.lpgEnabled && near(c.lpgCorrection, 1.2f));
  CHECK(c.mainDigit() == MainDigitSize::Small);
  CHECK(c.screenOffDelaySec == 120);
  CHECK(std::strcmp(c.apPassword, "schema-eight-password") == 0);
  // The one field the record predates takes the value that changes no
  // behaviour at all. An owner who needs a switch-on delay sets one; an owner
  // who does not must not have an upgrade start moving fuel between tanks.
  CHECK(c.lpgOnDelayMs == 0);
  CHECK(Preferences::storage["h2gauge/config"].size() == sizeof(ConfigData));
}

// A schema-9 record is the same length as a schema-10 one: the two main-screen
// cells landed in padding that already existed before the checksum, so the
// stored version is the only thing telling begin() which layout those bytes
// hold. It is the layout 0.4.25 to 0.4.27 wrote, so it is the one an upgrade in
// the car meets.
//
// The padding byte is a parameter because the point of the migration is that
// the record's own padding is never read back as a setting.
std::vector<uint8_t> v9Record(uint8_t startPage = 0, uint8_t padding = 0xA5) {
  ConfigData c = ConfigStore::defaults();
  c.schemaVersion = 9;
  c.brightnessDay = 66;
  c.lpgEnabled = true;
  c.lpgCorrection = 1.2f;
  c.lpgOnDelayMs = 2500;
  c.setDisplayStartPage(startPage);
  strlcpy(c.apPassword, "schema-nine-password", sizeof(c.apPassword));
  std::vector<uint8_t> result(220);
  std::memcpy(result.data(), &c, 216);
  // 214..215 is the padding the schema-9 build never wrote. Poison it, because
  // a migration that copied the whole prefix would read this as a choice of
  // what the main screen shows.
  result[214] = padding;
  result[215] = padding;
  const uint32_t hash = fnv(result.data(), 216);
  std::memcpy(result.data() + 216, &hash, 4);
  return result;
}

void testSchema9RecordKeepsSettingsAndTakesFactoryCells() {
  Preferences::storage.clear();
  Preferences::storage["h2gauge/config"] = v9Record(3);
  ConfigStore store;
  CHECK(store.begin());
  const ConfigData& c = store.data();
  CHECK(c.schemaVersion == H2G_CONFIG_SCHEMA);
  // Everything the record held, including the switch-on delay that sits in the
  // two bytes immediately before the appended ones: the migration copies up to
  // the first new field and not a byte less.
  CHECK(c.brightnessDay == 66);
  CHECK(c.lpgEnabled && near(c.lpgCorrection, 1.2f));
  CHECK(c.lpgOnDelayMs == 2500);
  CHECK(std::strcmp(c.apPassword, "schema-nine-password") == 0);
  // What the record predates takes the pair the screen has always drawn, so an
  // upgrade looks exactly as it did before it was asked. The poisoned padding
  // proves these come from defaults() and not from the record.
  CHECK(c.mainCellLeft() == MainCellValue::CurrentConsumption);
  CHECK(c.mainCellRight() == MainCellValue::AverageConsumption);
  // The diagnostics screen moved to the end of the cycle, so the start page the
  // owner chose follows the screen and not the number: 3 was diagnostics.
  CHECK(c.displayStartPage() == 4);
  CHECK(Preferences::storage["h2gauge/config"].size() == sizeof(ConfigData));
}

// Both directions of the swap, and a page that did not move at all. A remap
// that only worked one way would quietly send an owner to the wrong screen.
void testStartPageFollowsTheScreenItNamed() {
  for (uint8_t stored : {(uint8_t)3, (uint8_t)4}) {
    Preferences::storage.clear();
    Preferences::storage["h2gauge/config"] = v9Record(stored);
    ConfigStore store;
    CHECK(store.begin());
    CHECK(store.data().displayStartPage() == (stored == 3 ? 4 : 3));
  }
  Preferences::storage.clear();
  Preferences::storage["h2gauge/config"] = v9Record(2);
  ConfigStore temperatures;
  CHECK(temperatures.begin());
  CHECK(temperatures.data().displayStartPage() == 2);
}

// A record already at the current schema was written by code that knows the new
// order, so the remap must not touch it: running it twice would swap the pages
// back and forth on every boot.
void testStartPageAndCellsSurviveASaveAtTheCurrentSchema() {
  Preferences::storage.clear();
  ConfigStore store;
  CHECK(store.begin());
  store.data().setDisplayStartPage(4);
  store.data().setMainCellLeft(MainCellValue::Rpm);
  store.data().setMainCellRight(MainCellValue::IdleShare);
  CHECK(store.save());
  ConfigStore reloaded;
  CHECK(reloaded.begin());
  CHECK(reloaded.data().displayStartPage() == 4);
  CHECK(reloaded.data().mainCellLeft() == MainCellValue::Rpm);
  CHECK(reloaded.data().mainCellRight() == MainCellValue::IdleShare);
}

// A record at the current schema with a sound checksum and a cell choice the
// enumeration does not know. The portal clamps what it writes, but it is not
// the only thing that can put a number here, and valid() is the last gate
// before a record is trusted: without the bound, an unknown value is accepted
// and the screen draws boost in its place, which reads as a setting rather than
// as corruption.
std::vector<uint8_t> v10Record(uint8_t rightCell) {
  ConfigData c = ConfigStore::defaults();
  c.schemaVersion = H2G_CONFIG_SCHEMA;
  c.brightnessDay = 57;
  c.lpgCorrection = 1.25f;
  c.setMainCellLeft(MainCellValue::Rpm);
  c.cellRightValue = rightCell;
  std::vector<uint8_t> result(sizeof(ConfigData));
  std::memcpy(result.data(), &c, sizeof(c));
  const uint32_t hash = fnv(result.data(), 216);
  std::memcpy(result.data() + 216, &hash, 4);
  return result;
}

void testUnknownCellValueIsRefusedAtTheCurrentSchema() {
  Preferences::storage.clear();
  Preferences::storage["h2gauge/config"] = v10Record(11);
  ConfigStore refused;
  CHECK(refused.begin());
  // The whole record is refused rather than drawn half-way: a config that fails
  // valid() is never trusted field by field.
  CHECK(refused.data().brightnessDay == ConfigStore::defaults().brightnessDay);
  CHECK(refused.data().mainCellLeft() == MainCellValue::CurrentConsumption);
  CHECK(refused.data().mainCellRight() == MainCellValue::AverageConsumption);

  // Control: the last value the enumeration does know is accepted, so the bound
  // is the enumeration and not something narrower.
  Preferences::storage.clear();
  Preferences::storage["h2gauge/config"] = v10Record(10);
  ConfigStore accepted;
  CHECK(accepted.begin());
  CHECK(accepted.data().brightnessDay == 57);
  CHECK(near(accepted.data().lpgCorrection, 1.25f));
  CHECK(accepted.data().mainCellLeft() == MainCellValue::Rpm);
  CHECK(accepted.data().mainCellRight() == MainCellValue::IdleShare);
}

void testSchema9RecordWithBrokenChecksumFallsBackToFactory() {
  Preferences::storage.clear();
  std::vector<uint8_t> record = v9Record();
  record[216] ^= 0xFF;
  Preferences::storage["h2gauge/config"] = record;
  ConfigStore store;
  CHECK(store.begin());
  const ConfigData& c = store.data();
  CHECK(c.schemaVersion == H2G_CONFIG_SCHEMA);
  CHECK(c.brightnessDay == ConfigStore::defaults().brightnessDay);
  CHECK(!c.lpgEnabled);
}

void testSchema8RecordWithBrokenChecksumFallsBackToFactory() {
  Preferences::storage.clear();
  auto record = v8Record();
  record[40] ^= 0xFF;
  Preferences::storage["h2gauge/config"] = record;
  ConfigStore store;
  CHECK(store.begin());
  CHECK(store.data().brightnessDay == ConfigStore::defaults().brightnessDay);
  CHECK(store.data().lpgOnDelayMs == 0);
}

void managerRun(BrightnessManager& m, const ConfigData& c, uint32_t ms, uint16_t adc) {
  hostAdc = adc;
  for (uint32_t t = 0; t < ms; t += 50) {
    hostMillis += 50;
    m.update(hostMillis, c);
  }
}
}  // namespace

// ---------------------------------------------------------------------------
// LPG valve input. Two timers in series: the contact stops bouncing, then the
// engine catches up with its own plumbing. The switch-on delay is the newer of
// the two and the reason these are written down - it decides which tank the
// trip charges for the seconds when the car is between fuels, and a delay that
// leaked into the debounce or into the switch-off would be invisible until
// somebody compared a full tank against the number on the screen.
// ---------------------------------------------------------------------------

struct LpgRig {
  LpgValveInput input;
  ConfigData config = ConfigStore::defaults();
  LpgRig(uint32_t start = 10000) {
    hostMillis = start;
    hostButton = HIGH;   // active low, so pin high is petrol
    config.lpgEnabled = true;
    config.lpgActiveLow = true;
    config.lpgDebounceMs = 800;
    config.lpgOffDelayMs = 1000;
    config.lpgOnDelayMs = 10000;
    input.begin();
  }
  // Steps the way the main loop does. Returns the timestamp of the update at
  // which the decision first became true, or 0 if it never did inside the
  // window - which is the form that lets the expected instant be written down
  // instead of implied by a sequence of runs.
  uint32_t timeToActive(int level, uint32_t limit) {
    hostButton = level;
    for (uint32_t t = 0; t <= limit; t += 50) {
      hostMillis += 50;
      if (input.update(hostMillis, config)) return hostMillis;
    }
    return 0;
  }
  uint32_t timeToInactive(int level, uint32_t limit) {
    hostButton = level;
    for (uint32_t t = 0; t <= limit; t += 50) {
      hostMillis += 50;
      if (!input.update(hostMillis, config)) return hostMillis;
    }
    return 0;
  }
};

void testOnDelayHoldsTheDecisionUntilTheInjectorsFollow() {
  LpgRig r;
  const uint32_t t0 = hostMillis;
  // Gas. The contact settles 800 ms after the level last moved, and the
  // injectors follow 10 s after that - not 10 s after the valve.
  const uint32_t flip = r.timeToActive(LOW, 40000);
  CHECK(flip == t0 + 50 + r.config.lpgDebounceMs + r.config.lpgOnDelayMs);
}

void testZeroOnDelaySwitchesAsSoonAsTheContactSettles() {
  LpgRig r;
  r.config.lpgOnDelayMs = 0;
  const uint32_t t0 = hostMillis;
  // What every release before this one did, and what a system that really does
  // switch at once still needs: the debounce alone gates the decision.
  const uint32_t flip = r.timeToActive(LOW, 40000);
  CHECK(flip == t0 + 50 + r.config.lpgDebounceMs);
}

void testChatterRestartsTheSettleSoTheDelayRunsFromTheLastEdge() {
  LpgRig r;
  // A second of contact bounce on a wire that runs the length of the engine
  // bay. The level never sits still long enough to settle.
  for (uint32_t t = 0; t < 1000; t += 50) {
    hostMillis += 50;
    hostButton = (t / 50) % 2 ? HIGH : LOW;
    r.input.update(hostMillis, r.config);
  }
  CHECK(!r.input.active());
  const uint32_t t0 = hostMillis;
  const uint32_t flip = r.timeToActive(LOW, 40000);
  // Measured from the last edge, not the first: a delay that started at the
  // first one would let a bouncing contact shorten the wait.
  CHECK(flip == t0 + 50 + r.config.lpgDebounceMs + r.config.lpgOnDelayMs);
}

void testSwitchingOffWaitsTheOffDelayAndNotTheOnOne() {
  LpgRig r;
  CHECK(r.timeToActive(LOW, 40000) != 0);
  const uint32_t t0 = hostMillis;
  // Petrol again. The switch-on delay must not appear on this side: the two
  // are delays on the same settled level, and adding them would hold the
  // counter on gas for ten seconds the engine spent burning petrol.
  const uint32_t flip = r.timeToInactive(HIGH, 40000);
  CHECK(flip == t0 + 50 + r.config.lpgDebounceMs + r.config.lpgOffDelayMs);
}

void testDisablingLpgDropsTheDecisionAtOnce() {
  LpgRig r;
  CHECK(r.timeToActive(LOW, 40000) != 0);
  r.config.lpgEnabled = false;
  hostMillis += 50;
  CHECK(!r.input.update(hostMillis, r.config));
  // And it stays dropped while the pin is still calling for gas.
  hostMillis += 5000;
  CHECK(!r.input.update(hostMillis, r.config));
}

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
  test("Day and Night are the only brightness settings", [] {
    // The RAM-only override is gone: it could not be persisted, so it vanished
    // on every reboot, and while it was active the saved sliders looked dead.
    // What must hold now is that a reboot changes nothing and the saved values
    // are what the logic returns.
    ConfigData config = ConfigStore::defaults();
    const uint8_t savedDay = config.brightnessDay;
    const uint8_t savedNight = config.brightnessNight;
    BrightnessLogic logic;
    logic.begin(0, config.brightness, savedDay, savedNight);
    CHECK(near(logic.currentPercent(), savedDay));

    for (uint32_t now = 50; now <= 2000; now += 50) {
      logic.sample(now, 0, config.brightness, savedDay, savedNight);
      CHECK(near(logic.currentPercent(), savedDay));
    }
    CHECK(config.brightnessDay == savedDay && config.brightnessNight == savedNight);

    BrightnessLogic afterReboot;
    afterReboot.begin(0, config.brightness, savedDay, savedNight);
    CHECK(near(afterReboot.currentPercent(), savedDay));
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
  // The class was never the problem: the gesture is recognised or not
  // depending on what loop() hands it, and that call site is in main.cpp where
  // no compiled test can see it. These three pin the contract the call site
  // relies on, so the wiring can be checked without an ESP32 in the loop.
  test("the dashboard screen follows brightness mode, not a forced setting", [] {
    // -1 is "ask the brightness mode": four presses switch day and night in
    // manual, and are four page turns in every other mode.
    ButtonRig manual;
    manual.button.setMultiClickMode(-1);
    manual.config.brightness.mode = BrightnessMode::Manual;
    for (int i = 0; i < 4; ++i) manual.click();
    manual.run(1000, false);
    CHECK(manual.events.size() == 1 &&
          manual.events[0] == ButtonEvent::QuadPress);

    ButtonRig automatic;
    automatic.button.setMultiClickMode(-1);
    automatic.config.brightness.mode = BrightnessMode::Auto;
    for (int i = 0; i < 4; ++i) automatic.click();
    CHECK(automatic.events.size() == 4);
    for (const auto e : automatic.events) CHECK(e == ButtonEvent::ShortPress);
  });

  test("a series forced off never reaches four presses in any brightness mode",
       [] {
    // What 0 means, and what the dashboard screen asked for from 0.4.24 to
    // 0.4.26: the quad-press handler in loop() existed and could not be
    // reached by pressing the button, manual brightness or not.
    ButtonRig r;
    r.button.setMultiClickMode(0);
    for (int i = 0; i < 4; ++i) r.click();
    CHECK(r.events.size() == 4);
    for (const auto e : r.events) CHECK(e == ButtonEvent::ShortPress);
  });

  test("service mode keeps the two-press selector in automatic brightness", [] {
    ButtonRig r;
    r.button.setMultiClickMode(1);
    r.config.brightness.mode = BrightnessMode::Auto;
    r.click();
    r.click();
    r.run(1000, false);
    CHECK(r.events.size() == 1 && r.events[0] == ButtonEvent::DoublePress);
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
    // The record was written under schema 5, so every field the schema-5 prefix
    // covers must still read back exactly, and only the fields appended since
    // then fall back to their factory values.
    CHECK(c.schemaVersion == H2G_CONFIG_SCHEMA && c.brightnessDay == 73 &&
          c.brightnessNight == 19);
    CHECK(near(c.petrolCorrection, 1.18f) && near(c.lpgCorrection, 1.31f));
    CHECK(near(c.displaySpeedOffsetKph, -4.5f) && c.lpgEnabled);
    CHECK(c.colorText == 0x1234 && c.startPage == 2 && !c.dfcoCorrectionEnabled);
    CHECK(c.language == UiLanguage::English);
    CHECK(c.centerValue == MainCenterValue::AverageConsumption);
    CHECK(std::strcmp(c.apPassword, "preserve-this-password") == 0);
    CHECK(c.brightness.mode == BrightnessMode::AlwaysDay);
    CHECK(Preferences::storage["h2gauge/config"].size() == sizeof(ConfigData));
    CHECK((Preferences::storage["h2trip/trip"] == std::vector<uint8_t>{1,2,3}));
    CHECK((Preferences::storage["h2petcal/state"] == std::vector<uint8_t>{4,5,6}));
  });
  test("a schema-6 record keeps every setting the owner tuned", [] {
    testSchema6RecordKeepsEverySetting();
  });
  test("a schema-7 record keeps its settings and takes the factory digit size", [] {
    testSchema7RecordKeepsSettingsAndTakesFactoryDigitSize();
  });
  test("padding garbage in a schema-7 record never becomes the digit size", [] {
    testPaddingGarbageNeverBecomesTheDigitSize();
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
  test("a schema-8 record keeps its settings and takes the factory switch-on delay", [] {
    testSchema8RecordKeepsSettingsAndTakesFactorySwitchOnDelay();
  });
  test("a schema-8 record whose checksum does not match falls back to factory", [] {
    testSchema8RecordWithBrokenChecksumFallsBackToFactory();
  });
  test("a schema-9 record keeps its settings and takes the factory main-screen cells", [] {
    testSchema9RecordKeepsSettingsAndTakesFactoryCells();
  });
  test("the start page follows the screen it named, not the number it held", [] {
    testStartPageFollowsTheScreenItNamed();
  });
  test("the start page and the cell choices survive a save at the current schema", [] {
    testStartPageAndCellsSurviveASaveAtTheCurrentSchema();
  });
  test("a schema-9 record whose checksum does not match falls back to factory", [] {
    testSchema9RecordWithBrokenChecksumFallsBackToFactory();
  });
  test("a cell value outside the enumeration is refused by valid()", [] {
    testUnknownCellValueIsRefusedAtTheCurrentSchema();
  });
  test("the switch-on delay survives a save and reload, and the range is enforced", [] {
    ConfigStore store; CHECK(store.begin());
    store.data().lpgEnabled = true;
    store.data().lpgOnDelayMs = 10000;
    CHECK(store.save());
    ConfigStore reloaded; CHECK(reloaded.begin());
    CHECK(reloaded.data().lpgOnDelayMs == 10000);
    // The form offers at most 30000, and a value outside the range must be
    // refused rather than stored: an unclamped delay would silently hold the
    // counter on petrol for as long as the number happened to say.
    reloaded.data().lpgOnDelayMs = 30001;
    CHECK(!reloaded.save());
  });
  test("the switch-on delay holds the decision until the injectors follow", [] {
    testOnDelayHoldsTheDecisionUntilTheInjectorsFollow();
  });
  test("a zero switch-on delay switches as soon as the contact settles", [] {
    testZeroOnDelaySwitchesAsSoonAsTheContactSettles();
  });
  test("contact chatter restarts the settle, so the delay runs from the last edge", [] {
    testChatterRestartsTheSettleSoTheDelayRunsFromTheLastEdge();
  });
  test("switching off waits the off delay and never the on one", [] {
    testSwitchingOffWaitsTheOffDelayAndNotTheOnOne();
  });
  test("disabling LPG drops the decision at once and keeps it dropped", [] {
    testDisablingLpgDropsTheDecisionAtOnce();
  });
  std::printf("\n%d host regression groups passed. Hardware behavior is not simulated.\n", passed);
}
