#include "ota_diagnostics.h"

#include <esp_system.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

uint32_t hostMillis = 0;
uint16_t hostAdc = 0;
int hostButton = HIGH;
std::map<std::string, std::vector<uint8_t>> Preferences::storage;
bool Preferences::failWrite = false;
bool Preferences::failBegin = false;
size_t Preferences::writes = 0;

namespace {
const esp_partition_t kApp0{"app0", 0x10000, 0x400000};
const esp_partition_t kApp1{"app1", 0x410000, 0x400000};
const esp_partition_t kOther{"factory", 0x810000, 0x100000};
}

const esp_partition_t* hostRunningPartition = &kApp0;
const esp_partition_t* hostBootPartition = &kApp0;
esp_ota_img_states_t hostImageState = ESP_OTA_IMG_UNDEFINED;
esp_err_t hostStateResult = ESP_OK;
esp_err_t hostMarkValidResult = ESP_OK;
unsigned hostMarkValidCalls = 0;
esp_reset_reason_t hostResetReason = ESP_RST_POWERON;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

namespace {
int passed = 0;

void reset() {
  Preferences::storage.clear();
  Preferences::failWrite = Preferences::failBegin = false;
  Preferences::writes = 0;
  hostRunningPartition = hostBootPartition = &kApp0;
  hostImageState = ESP_OTA_IMG_UNDEFINED;
  hostStateResult = ESP_OK;
  hostMarkValidResult = ESP_OK;
  hostMarkValidCalls = 0;
  hostResetReason = ESP_RST_POWERON;
}

void test(const char* name, const std::function<void()>& body) {
  reset();
  body();
  std::printf("PASS %s\n", name);
  ++passed;
}

void write32(std::vector<uint8_t>& record, size_t offset, uint32_t value) {
  record[offset] = static_cast<uint8_t>(value);
  record[offset + 1] = static_cast<uint8_t>(value >> 8);
  record[offset + 2] = static_cast<uint8_t>(value >> 16);
  record[offset + 3] = static_cast<uint8_t>(value >> 24);
}

uint32_t hashRecord(const std::vector<uint8_t>& record) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < 60; ++i) {
    hash ^= record[i];
    hash *= 16777619UL;
  }
  return hash;
}

void createAttempt(OtaDiagnostics& diagnostics) {
  CHECK(diagnostics.begin());
  CHECK(diagnostics.recordReceiving(&kApp0, &kApp1, 1079000));
  CHECK(diagnostics.attempt() == 1);
  CHECK(std::strcmp(diagnostics.phaseName(), "receiving") == 0);
  CHECK(diagnostics.receivedSize() == 0);
  CHECK(diagnostics.recordProgress(786432, "esp-idf-desc"));
  CHECK(diagnostics.receivedSize() == 786432);
  CHECK(diagnostics.recordVerifying(&kApp0, &kApp1, 1079000, "esp-idf-desc"));
  CHECK(diagnostics.attempt() == 1);
  CHECK(diagnostics.receivedSize() == 1079000);
  CHECK(std::strcmp(diagnostics.resultName(), "pending") == 0);
  CHECK(std::strcmp(diagnostics.phaseName(), "verifying") == 0);
}
}  // namespace

int main() {
  test("empty NVS reports no OTA record", [] {
    OtaDiagnostics diagnostics;
    CHECK(diagnostics.begin());
    CHECK(!diagnostics.hasRecord());
    CHECK(std::strcmp(diagnostics.resultName(), "none") == 0);
    CHECK(std::strcmp(diagnostics.phaseName(), "none") == 0);
  });

  test("transport reset is persisted as interrupted upload with progress", [] {
    OtaDiagnostics beforeReset;
    CHECK(beforeReset.begin());
    CHECK(beforeReset.recordReceiving(&kApp0, &kApp1, 1079616));
    CHECK(beforeReset.recordProgress(1048576, "0.3.7"));

    hostResetReason = ESP_RST_TASK_WDT;
    OtaDiagnostics afterReset;
    CHECK(afterReset.begin());
    CHECK(std::strcmp(afterReset.resultName(), "interrupted_upload") == 0);
    CHECK(std::strcmp(afterReset.phaseName(), "receiving") == 0);
    CHECK(afterReset.imageSize() == 1079616);
    CHECK(afterReset.receivedSize() == 1048576);
    CHECK(afterReset.resetReason() == ESP_RST_TASK_WDT);
    CHECK(afterReset.attempt() == 1);
  });

  test("reset inside esp_ota_end is persisted as interrupted verification", [] {
    OtaDiagnostics beforeReset;
    createAttempt(beforeReset);

    hostResetReason = ESP_RST_TASK_WDT;
    OtaDiagnostics afterReset;
    CHECK(afterReset.begin());
    CHECK(std::strcmp(afterReset.resultName(), "interrupted_finalize") == 0);
    CHECK(std::strcmp(afterReset.phaseName(), "verifying") == 0);
    CHECK(afterReset.resetReason() == ESP_RST_TASK_WDT);
    CHECK(afterReset.sourceAddress() == kApp0.address);
    CHECK(afterReset.targetAddress() == kApp1.address);
    CHECK(afterReset.imageSize() == 1079000);
    CHECK(afterReset.attempt() == 1);

    // A later power cycle must not overwrite the reset evidence associated
    // with the failed OTA transaction.
    hostResetReason = ESP_RST_POWERON;
    OtaDiagnostics afterPowerCycle;
    CHECK(afterPowerCycle.begin());
    CHECK(afterPowerCycle.resetReason() == ESP_RST_TASK_WDT);
  });

  test("reset after image verification identifies boot selection phase", [] {
    OtaDiagnostics beforeReset;
    createAttempt(beforeReset);
    CHECK(beforeReset.recordImageVerified());
    CHECK(std::strcmp(beforeReset.phaseName(), "image_verified") == 0);

    OtaDiagnostics afterReset;
    CHECK(afterReset.begin());
    CHECK(std::strcmp(afterReset.resultName(), "interrupted_finalize") == 0);
    CHECK(std::strcmp(afterReset.phaseName(), "image_verified") == 0);
    CHECK(afterReset.attempt() == 1);
  });

  test("selected target returning to source is classified as rollback", [] {
    OtaDiagnostics beforeReset;
    createAttempt(beforeReset);
    CHECK(beforeReset.recordImageVerified());
    CHECK(beforeReset.recordPending(&kApp0, &kApp1, 1079000, "esp-idf-desc"));
    CHECK(beforeReset.attempt() == 1);
    CHECK(std::strcmp(beforeReset.phaseName(), "boot_selected") == 0);

    OtaDiagnostics afterReset;
    CHECK(afterReset.begin());
    CHECK(std::strcmp(afterReset.resultName(), "rolled_back") == 0);
    CHECK(std::strcmp(afterReset.phaseName(), "boot_selected") == 0);
    CHECK(afterReset.attempt() == 1);
  });

  test("target boot is applied and pending image is confirmed", [] {
    OtaDiagnostics beforeReset;
    createAttempt(beforeReset);
    CHECK(beforeReset.recordImageVerified());
    CHECK(beforeReset.recordPending(&kApp0, &kApp1, 1079000, "esp-idf-desc"));

    hostRunningPartition = hostBootPartition = &kApp1;
    hostImageState = ESP_OTA_IMG_PENDING_VERIFY;
    hostResetReason = ESP_RST_SW;
    OtaDiagnostics afterReset;
    CHECK(afterReset.begin());
    CHECK(hostMarkValidCalls == 1);
    CHECK(std::strcmp(afterReset.resultName(), "applied") == 0);
    CHECK(afterReset.resetReason() == ESP_RST_SW);
    CHECK(afterReset.attempt() == 1);
  });

  test("unexpected running partition remains explicit", [] {
    OtaDiagnostics beforeReset;
    createAttempt(beforeReset);
    CHECK(beforeReset.recordImageVerified());
    CHECK(beforeReset.recordPending(&kApp0, &kApp1, 1079000, "esp-idf-desc"));

    hostRunningPartition = hostBootPartition = &kOther;
    OtaDiagnostics afterReset;
    CHECK(afterReset.begin());
    CHECK(std::strcmp(afterReset.resultName(), "unexpected_slot") == 0);
  });

  test("returned image-finalization error remains durable", [] {
    OtaDiagnostics diagnostics;
    createAttempt(diagnostics);
    CHECK(diagnostics.recordFinalizeFailed(-42));
    CHECK(std::strcmp(diagnostics.resultName(), "finalize_failed") == 0);
    CHECK(std::strcmp(diagnostics.phaseName(), "verifying") == 0);
    CHECK(diagnostics.errorCode() == -42);

    OtaDiagnostics afterReset;
    CHECK(afterReset.begin());
    CHECK(std::strcmp(afterReset.resultName(), "finalize_failed") == 0);
    CHECK(afterReset.errorCode() == -42);
  });

  test("returned boot-selection error is not mislabeled as rollback", [] {
    OtaDiagnostics diagnostics;
    createAttempt(diagnostics);
    CHECK(diagnostics.recordImageVerified());
    CHECK(diagnostics.recordBootSelectionFailed(-43));
    CHECK(std::strcmp(diagnostics.resultName(), "boot_selection_failed") == 0);
    CHECK(std::strcmp(diagnostics.phaseName(), "image_verified") == 0);
    CHECK(diagnostics.errorCode() == -43);

    OtaDiagnostics afterReset;
    CHECK(afterReset.begin());
    CHECK(std::strcmp(afterReset.resultName(), "boot_selection_failed") == 0);
    CHECK(afterReset.errorCode() == -43);
  });

  test("version 2 boot-selected record migrates to version 3 semantics", [] {
    std::vector<uint8_t> record(64, 0);
    write32(record, 0, 0x544F3248UL);
    record[4] = 2;  // record version
    record[5] = 1;  // pending
    record[6] = 3;  // v2 boot_selected; v3 inserts receiving before it
    record[7] = 0xFF;
    write32(record, 8, 7);
    write32(record, 12, kApp0.address);
    write32(record, 16, kApp1.address);
    write32(record, 20, 1079000);
    const char descriptor[] = "legacy-v2";
    std::memcpy(record.data() + 24, descriptor, sizeof(descriptor));
    write32(record, 60, hashRecord(record));
    Preferences::storage["h2ota/last"] = record;

    OtaDiagnostics diagnostics;
    CHECK(diagnostics.begin());
    CHECK(std::strcmp(diagnostics.resultName(), "rolled_back") == 0);
    CHECK(std::strcmp(diagnostics.phaseName(), "boot_selected") == 0);
    CHECK(diagnostics.receivedSize() == 1079000);
    CHECK(diagnostics.attempt() == 7);
    CHECK(Preferences::storage["h2ota/last"][4] == 3);
  });

  test("NVS failure is surfaced instead of reporting a durable phase", [] {
    OtaDiagnostics diagnostics;
    CHECK(diagnostics.begin());
    Preferences::failWrite = true;
    CHECK(!diagnostics.recordVerifying(&kApp0, &kApp1, 1079000, "desc"));
    CHECK(!diagnostics.storageHealthy());
  });

  std::printf("%d OTA diagnostic groups passed\n", passed);
  return 0;
}
