#pragma once

#include <Arduino.h>
#include "driver/twai.h"
#include "telemetry.h"

// Vehicle-specific Mode 22 entries belong in src/mode22_config.cpp only after
// their request ID, response ID, DID and formula have been verified on a Haval.
using Mode22DecodeFn = void (*)(const uint8_t* payload, size_t length,
                                uint32_t now, TelemetryData& telemetry);

struct Mode22DidDefinition {
  const char* name;
  uint32_t requestId;
  uint32_t responseId;
  uint16_t did;
  uint32_t pollIntervalMs;
  bool extendedId;
  bool enabled;
  Mode22DecodeFn decode;
};

extern const Mode22DidDefinition* const kMode22DidList;
extern const size_t kMode22DidCount;

// Small ISO-TP transport for verified Mode 22 DIDs. The shipping DID list is
// deliberately empty, so this code emits no manufacturer-specific requests.
class Mode22Transport {
 public:
  Mode22Transport(const Mode22DidDefinition* definitions, size_t count,
                  TelemetryData& telemetry)
      : definitions_(definitions), count_(count), telemetry_(telemetry) {}

  // Builds one due physical request. Returns false when no verified DID is due.
  bool buildDueRequest(uint32_t now, twai_message_t& message);

  // Consumes matching single-frame or multi-frame responses. For an ISO-TP
  // first frame, flowControl is populated and needsFlowControl is set.
  bool handleFrame(const twai_message_t& message, uint32_t now,
                   twai_message_t& flowControl, bool& needsFlowControl);

  void reset();
  bool waiting() const { return waiting_; }
  void timeout() { reset(); }

 private:
  const Mode22DidDefinition* findResponse(const twai_message_t& message,
                                          uint16_t did) const;
  void complete(const Mode22DidDefinition& definition, uint32_t now);

  const Mode22DidDefinition* definitions_;
  size_t count_;
  TelemetryData& telemetry_;
  size_t cursor_ = 0;
  const Mode22DidDefinition* pending_ = nullptr;
  bool waiting_ = false;
  uint8_t buffer_[48]{};
  size_t expectedLength_ = 0;
  size_t receivedLength_ = 0;
  uint8_t nextSequence_ = 1;
};
