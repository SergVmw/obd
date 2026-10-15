#include <cassert>
#include <cmath>
#include <cstddef>
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
  // Every data field gets a distinct non-zero value. A field left at zero
  // cannot be distinguished from a field the encoder forgot to copy, which is
  // exactly how the standing-fuel counters were lost on every save: the
  // stored record read back clean, because zero was also what the source held.
  TripState trip{};
  trip.rawPetrolLiters = 12.345;
  trip.rawLpgLiters = 6.789;
  trip.idlePetrolLiters = 1.25;
  trip.idleLpgLiters = 0.5;
  trip.petrolDistanceKm = 123.5;
  trip.lpgDistanceKm = 54.25;
  trip.totalDistanceKm = 177.75;
  trip.petrolSeconds = 4567;
  trip.lpgSeconds = 1234;
  trip.petrolIdleSeconds = 600;
  trip.lpgIdleSeconds = 120;
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

// The contract that was missing: whatever canonicalTrip() drops must fail
// here. Sealing the source produces the canonical form of the same data, so a
// complete encoder writes a record identical to it, byte for byte. A field the
// encoder leaves out is zero in the record and non-zero in the sealed source,
// and the difference shows up without this test naming the field. That is what
// makes a counter added later unable to vanish silently the way the standing
// litres did.
void testEveryTripFieldSurvivesEncoding() {
  const TripState trip = sampleTrip();
  TripState expected = trip;
  TripStore::seal(expected);

  uint8_t record[PersistenceSnapshot::kRecordBytes]{};
  assert(PersistenceSnapshot::encode(5, trip, sampleCalibration(), record,
                                     sizeof(record)));
  PersistenceSnapshot::Decoded decoded;
  assert(PersistenceSnapshot::decode(record, sizeof(record), decoded));

  assert(std::memcmp(&expected, &decoded.trip, sizeof(TripState)) == 0);
  // Spelled out for the two counters that were lost, so the intent survives
  // even if someone later weakens the byte comparison above.
  assert(std::fabs(decoded.trip.idlePetrolLiters - 1.25) < 1e-12);
  assert(std::fabs(decoded.trip.idleLpgLiters - 0.5) < 1e-12);
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

// Leave something recognisable on the stack where the encoder will work, so the
// next encode has something else to find below its own frame.
__attribute__((noinline)) void dirtyTheStack() {
  volatile uint8_t scratch[4096];
  for (size_t i = 0; i < sizeof(scratch); ++i) {
    scratch[i] = static_cast<uint8_t>(i * 7U + 3U);
  }
}

// Two encodes of the same data have to produce the same bytes. They did not:
// sizeof() covers the padding that follows `checksum`, that padding was copied
// into the record uninitialised, and the bytes depended on whatever the stack
// happened to hold between the two calls. That is what stopped payloadEquals
// from recognising an unchanged trip, and the journal then appended a record
// every twenty seconds to a state that had not moved. The host suite only sees
// it on a compiler whose aggregate initialisation leaves that padding dirty, so
// the property is asserted directly rather than through the round trip.
void testEncodingDoesNotDependOnStackGarbage() {
  const TripState trip = sampleTrip();
  const PetrolCalibrationState calibration = sampleCalibration();
  uint8_t first[PersistenceSnapshot::kRecordBytes];
  uint8_t second[PersistenceSnapshot::kRecordBytes];

  assert(PersistenceSnapshot::encode(11, trip, calibration, first,
                                     sizeof(first)));
  dirtyTheStack();
  assert(PersistenceSnapshot::encode(11, trip, calibration, second,
                                     sizeof(second)));

  assert(std::memcmp(first, second, sizeof(first)) == 0);
  assert(PersistenceSnapshot::payloadEquals(first, sizeof(first), trip,
                                            calibration));

  // Spelled out so the intent survives even if the comparison above is
  // weakened: the record must not contain stack bytes at all.
  const size_t tripTail = PersistenceSnapshot::kTripOffset +
                          offsetof(TripState, checksum) + sizeof(uint32_t);
  for (size_t i = tripTail;
       i < PersistenceSnapshot::kTripOffset + sizeof(TripState); ++i) {
    assert(first[i] == 0);
  }
  const size_t calibrationTail = PersistenceSnapshot::kCalibrationOffset +
                                 offsetof(PetrolCalibrationState, checksum) +
                                 sizeof(uint32_t);
  for (size_t i = calibrationTail;
       i < PersistenceSnapshot::kCalibrationOffset +
               sizeof(PetrolCalibrationState); ++i) {
    assert(first[i] == 0);
  }
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
  // Bumped 140 -> 156 for the standing litre counters, then 156 -> 164 for the
  // standing seconds. Records written by an older schema are rejected by the
  // magic and CRC check, so the trip restarts once on this upgrade.
  static_assert(PersistenceSnapshot::kRecordBytes == 164,
                "record format regression");
  testCrc32();
  testSnapshotRoundTrip();
  testEveryTripFieldSurvivesEncoding();
  testCanonicalChecksumsAndDirtyComparison();
  testEncodingDoesNotDependOnStackGarbage();
  testTornAndCorruptRecordsRejected();
  testSequenceOrderingAndWrap();
  std::cout << "persistence tests passed\n";
  return 0;
}
