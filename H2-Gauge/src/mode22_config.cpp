#include "mode22.h"

// Production policy: no manufacturer-specific DID is enabled until its CAN IDs,
// payload layout, units and validity conditions have been captured and verified
// on the target Haval. Keeping a null list guarantees that no Mode 22 request is
// emitted by an unconfigured release.
const Mode22DidDefinition* const kMode22DidList = nullptr;
const size_t kMode22DidCount = 0;

// Future verified configuration pattern:
// namespace {
// void decodeExample(const uint8_t* payload, size_t length, uint32_t now,
//                    TelemetryData& telemetry) { ... }
// const Mode22DidDefinition kVerifiedDids[] = {
//   {"verified-name", requestId, responseId, did, intervalMs,
//    extendedId, true, decodeExample},
// };
// }
// const Mode22DidDefinition* const kMode22DidList = kVerifiedDids;
// const size_t kMode22DidCount = sizeof(kVerifiedDids) / sizeof(kVerifiedDids[0]);
