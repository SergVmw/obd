#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "Preferences.h"
#include "persistence_snapshot.h"
#include "storage_crc32.h"

uint32_t hostMillis = 0;
uint16_t hostAdc = 0;
int hostButton = 1;
std::map<std::string, std::vector<uint8_t>> Preferences::storage;
bool Preferences::failWrite = false;
bool Preferences::failBegin = false;
size_t Preferences::writes = 0;

namespace {
TripState sampleTrip() {
  TripState trip{};
  trip.rawPetrolLiters = 12.345;
  trip.rawLpgLiters = 6.789;
  trip.petrolDistanceKm = 123.5;
  trip.lpgDistanceKm = 54.25;
  trip.totalDistanceKm = 177.75;
  trip.petrolSeconds = 4567;
  trip.lpgSeconds = 1234;
  return trip;
}

PetrolCalibrationState sampleCalibration() {
  PetrolCalibrationState state{};
  state.active = 1;
  state.rawPetrolLiters = 34.5;
  state.distanceKm = 456.75;
  state.petrolSeconds = 9876;
  state.calibrationCount = 3;
  state.lastActualLiters = 42.1f;
  state.lastCalculatedLiters = 40.2f;
  state.lastOldCorrection = 1.01f;
  state.lastNewCorrection = 1.057f;
  return state;
}

void testCrc32() {
  const char text[] = "123456789";
  assert(storageCrc32(text, 9) == 0xCBF43926UL);
  uint32_t state = storageCrc32Update(kStorageCrc32Initial, text, 4);
  state = storageCrc32Update(state, text + 4, 5);
  assert(storageCrc32Finish(state) == 0xCBF43926UL);
}

void testSnapshotRoundTrip() {
  const TripState trip = sampleTrip();
  const PetrolCalibrationState calibration = sampleCalibration();
  uint8_t record[PersistenceSnapshot::kRecordBytes]{};
  assert(PersistenceSnapshot::encode(77, trip, calibration, record,
                                     sizeof(record)));
  assert(PersistenceSnapshot::valid(record, sizeof(record)));
  assert(PersistenceSnapshot::sequence(record, sizeof(record)) == 77);
  assert(PersistenceSnapshot::payloadEquals(record, sizeof(record), trip,
                                             calibration));

  PersistenceSnapshot::Decoded decoded;
  assert(PersistenceSnapshot::decode(record, sizeof(record), decoded));
  assert(decoded.sequence == 77);
  assert(TripStore::valid(decoded.trip));
  assert(PetrolCalibrationStore::valid(decoded.calibration));
  assert(std::fabs(decoded.trip.rawPetrolLiters - 12.345) < 1e-12);
  assert(std::fabs(decoded.calibration.distanceKm - 456.75) < 1e-12);
}

void testCanonicalChecksumsAndDirtyComparison() {
  TripState trip = sampleTrip();
  PetrolCalibrationState calibration = sampleCalibration();
  trip.magic = 0;
  trip.checksum = 0xDEADBEEF;
  calibration.magic = 0;
  calibration.schemaVersion = 99;
  calibration.checksum = 0x12345678;

  uint8_t record[PersistenceSnapshot::kRecordBytes]{};
  assert(PersistenceSnapshot::encode(2, trip, calibration, record,
                                     sizeof(record)));
  assert(PersistenceSnapshot::payloadEquals(record, sizeof(record), trip,
                                             calibration));
  trip.totalDistanceKm += 0.001;
  assert(!PersistenceSnapshot::payloadEquals(record, sizeof(record), trip,
                                              calibration));
}

void testTornAndCorruptRecordsRejected() {
  uint8_t record[PersistenceSnapshot::kRecordBytes]{};
  assert(PersistenceSnapshot::encode(9, sampleTrip(), sampleCalibration(),
                                     record, sizeof(record)));
  for (size_t offset : {size_t{0}, size_t{8}, size_t{24},
                        PersistenceSnapshot::kCrcOffset}) {
    uint8_t corrupted[PersistenceSnapshot::kRecordBytes];
    memcpy(corrupted, record, sizeof(corrupted));
    corrupted[offset] ^= 0x40;
    assert(!PersistenceSnapshot::valid(corrupted, sizeof(corrupted)));
  }
  assert(!PersistenceSnapshot::valid(record, sizeof(record) - 1));
  assert(!PersistenceSnapshot::encode(0, sampleTrip(), sampleCalibration(),
                                      record, sizeof(record)));
}

void testSequenceOrderingAndWrap() {
  assert(PersistenceSnapshot::sequenceNewer(2, 1));
  assert(!PersistenceSnapshot::sequenceNewer(1, 2));
  assert(PersistenceSnapshot::sequenceNewer(1, 0xFFFFFFFFUL));
  assert(!PersistenceSnapshot::sequenceNewer(0, 100));
}
}  // namespace

int main() {
  static_assert(PersistenceSnapshot::kRecordBytes == 140,
                "record format regression");
  testCrc32();
  testSnapshotRoundTrip();
  testCanonicalChecksumsAndDirtyComparison();
  testTornAndCorruptRecordsRejected();
  testSequenceOrderingAndWrap();
  std::cout << "persistence tests passed\n";
  return 0;
}
