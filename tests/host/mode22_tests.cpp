#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

#include "mode22.h"

namespace {
void decodeNoop(const uint8_t*, size_t, uint32_t, TelemetryData&) {}

Mode22DidDefinition makeDefinition(size_t index, uint32_t pollIntervalMs = 0) {
  return {"test-did", static_cast<uint32_t>(0x7E0 + index),
          static_cast<uint32_t>(0x7E8 + index),
          static_cast<uint16_t>(0x1000 + index), pollIntervalMs, false, true,
          decodeNoop};
}
}  // namespace

int main() {
  static_assert(kMaxConfiguredDids == 8,
                "update host coverage when Mode 22 capacity changes");

  // Poll timestamps belong to each transport: one instance must not suppress
  // an identical DID request from another instance.
  {
    const Mode22DidDefinition definition = makeDefinition(0, 500);
    TelemetryData firstTelemetry;
    TelemetryData secondTelemetry;
    Mode22Transport first(&definition, 1, firstTelemetry);
    Mode22Transport second(&definition, 1, secondTelemetry);
    twai_message_t request{};

    assert(first.buildDueRequest(1000, request));
    assert(request.identifier == definition.requestId);
    first.reset();
    assert(second.buildDueRequest(1000, request));
    assert(request.identifier == definition.requestId);
    second.reset();

    assert(first.buildDueRequest(1500, request));
    first.reset();
  }

  // Every configured entry up to the declared capacity is schedulable. There
  // is no shorter usableCount that strands entries after a hidden limit.
  {
    std::array<Mode22DidDefinition, kMaxConfiguredDids> definitions{};
    for (size_t i = 0; i < definitions.size(); ++i) {
      definitions[i] = makeDefinition(i);
    }
    TelemetryData telemetry;
    Mode22Transport transport(definitions.data(), definitions.size(), telemetry);
    twai_message_t request{};
    for (size_t i = 0; i < definitions.size(); ++i) {
      assert(transport.buildDueRequest(1000, request));
      assert(request.identifier == definitions[i].requestId);
      assert(request.data[1] == 0x22);
      assert(request.data[2] == static_cast<uint8_t>(definitions[i].did >> 8));
      assert(request.data[3] == static_cast<uint8_t>(definitions[i].did));
      transport.reset();
    }
  }

  // A runtime list that exceeds inline storage is rejected as a whole, rather
  // than being partially polled or indexing beyond the per-instance array.
  {
    std::array<Mode22DidDefinition, kMaxConfiguredDids + 1> definitions{};
    for (size_t i = 0; i < definitions.size(); ++i) {
      definitions[i] = makeDefinition(i);
    }
    TelemetryData telemetry;
    Mode22Transport transport(definitions.data(), definitions.size(), telemetry);
    twai_message_t request{};
    assert(!transport.buildDueRequest(1000, request));

    twai_message_t response{};
    response.identifier = definitions.back().responseId;
    response.data_length_code = 4;
    response.data[0] = 0x03;
    response.data[1] = 0x62;
    response.data[2] = static_cast<uint8_t>(definitions.back().did >> 8);
    response.data[3] = static_cast<uint8_t>(definitions.back().did);
    twai_message_t flowControl{};
    bool needsFlowControl = false;
    assert(!transport.handleFrame(response, 1000, flowControl,
                                  needsFlowControl));
  }

  std::cout << "Mode 22 tests passed\n";
  return 0;
}
