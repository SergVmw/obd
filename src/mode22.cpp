#include "mode22.h"

#include <string.h>

namespace {
constexpr size_t kMaxConfiguredDids = 8;
uint32_t sLastSentAt[kMaxConfiguredDids]{};
}

bool Mode22Transport::buildDueRequest(uint32_t now,
                                      twai_message_t& message) {
  if (waiting_ || definitions_ == nullptr || count_ == 0) return false;
  const size_t usableCount = count_ < kMaxConfiguredDids
                                 ? count_
                                 : kMaxConfiguredDids;

  for (size_t checked = 0; checked < usableCount; ++checked) {
    const size_t index = (cursor_ + checked) % usableCount;
    const auto& definition = definitions_[index];
    if (!definition.enabled || definition.decode == nullptr ||
        now - sLastSentAt[index] < definition.pollIntervalMs) {
      continue;
    }

    message = {};
    message.identifier = definition.requestId;
    message.extd = definition.extendedId;
    message.rtr = 0;
    message.data_length_code = 8;
    message.data[0] = 0x03;
    message.data[1] = 0x22;
    message.data[2] = static_cast<uint8_t>(definition.did >> 8);
    message.data[3] = static_cast<uint8_t>(definition.did & 0xFF);

    sLastSentAt[index] = now;
    cursor_ = (index + 1) % usableCount;
    pending_ = &definition;
    waiting_ = true;
    expectedLength_ = 0;
    receivedLength_ = 0;
    nextSequence_ = 1;
    return true;
  }
  return false;
}

const Mode22DidDefinition* Mode22Transport::findResponse(
    const twai_message_t& message, uint16_t did) const {
  if (definitions_ == nullptr) return nullptr;
  for (size_t i = 0; i < count_; ++i) {
    const auto& definition = definitions_[i];
    if (definition.enabled && definition.did == did &&
        definition.responseId == message.identifier &&
        definition.extendedId == static_cast<bool>(message.extd)) {
      return &definition;
    }
  }
  return nullptr;
}

void Mode22Transport::complete(const Mode22DidDefinition& definition,
                               uint32_t now) {
  if (definition.decode != nullptr) {
    definition.decode(buffer_, receivedLength_, now, telemetry_);
  }
  reset();
}

bool Mode22Transport::handleFrame(const twai_message_t& message, uint32_t now,
                                  twai_message_t& flowControl,
                                  bool& needsFlowControl) {
  needsFlowControl = false;
  if (message.rtr || message.data_length_code < 2) return false;

  const uint8_t pciType = message.data[0] >> 4;
  if (pciType == 0x0) {
    const size_t udsLength = message.data[0] & 0x0F;
    if (udsLength < 3 || message.data_length_code < udsLength + 1 ||
        message.data[1] != 0x62) {
      return false;
    }
    const uint16_t did = (static_cast<uint16_t>(message.data[2]) << 8) |
                         message.data[3];
    const Mode22DidDefinition* definition = findResponse(message, did);
    if (definition == nullptr) return false;

    receivedLength_ = udsLength - 3;
    if (receivedLength_ > sizeof(buffer_)) return false;
    if (receivedLength_ > 0) {
      memcpy(buffer_, &message.data[4], receivedLength_);
    }
    complete(*definition, now);
    return true;
  }

  if (pciType == 0x1) {
    if (message.data_length_code < 5 || message.data[2] != 0x62) return false;
    const size_t udsLength =
        (static_cast<size_t>(message.data[0] & 0x0F) << 8) | message.data[1];
    const uint16_t did = (static_cast<uint16_t>(message.data[3]) << 8) |
                         message.data[4];
    const Mode22DidDefinition* definition = findResponse(message, did);
    if (definition == nullptr || udsLength < 3 ||
        udsLength - 3 > sizeof(buffer_)) {
      return false;
    }

    pending_ = definition;
    waiting_ = true;
    expectedLength_ = udsLength - 3;
    receivedLength_ = 0;
    for (uint8_t i = 5;
         i < message.data_length_code && receivedLength_ < expectedLength_;
         ++i) {
      buffer_[receivedLength_++] = message.data[i];
    }
    nextSequence_ = 1;

    flowControl = {};
    flowControl.identifier = definition->requestId;
    flowControl.extd = definition->extendedId;
    flowControl.rtr = 0;
    flowControl.data_length_code = 8;
    flowControl.data[0] = 0x30;  // Continue To Send, unlimited block, no delay.
    needsFlowControl = true;
    return true;
  }

  if (pciType == 0x2 && pending_ != nullptr && waiting_ &&
      message.identifier == pending_->responseId &&
      message.extd == pending_->extendedId) {
    const uint8_t sequence = message.data[0] & 0x0F;
    if (sequence != nextSequence_) {
      reset();
      return true;
    }
    nextSequence_ = (nextSequence_ + 1) & 0x0F;
    for (uint8_t i = 1;
         i < message.data_length_code && receivedLength_ < expectedLength_;
         ++i) {
      buffer_[receivedLength_++] = message.data[i];
    }
    if (receivedLength_ >= expectedLength_) complete(*pending_, now);
    return true;
  }

  return false;
}

void Mode22Transport::reset() {
  pending_ = nullptr;
  waiting_ = false;
  expectedLength_ = 0;
  receivedLength_ = 0;
  nextSequence_ = 1;
}
