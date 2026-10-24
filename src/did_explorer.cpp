#include "did_explorer.h"

#include <cstring>

namespace {
constexpr uint8_t kServiceReadByIdentifier = 0x22;
constexpr uint8_t kServiceReadByLocalIdentifier = 0x21;
constexpr uint8_t kPositiveReadByIdentifier = 0x62;   // 0x22 + 0x40
constexpr uint8_t kPositiveReadByLocalId = 0x61;      // 0x21 + 0x40
constexpr uint8_t kNegativeResponse = 0x7F;
constexpr uint8_t kServiceDiagnosticSessionControl = 0x10;
constexpr uint8_t kPositiveSessionControl = 0x50;
constexpr uint8_t kServiceTesterPresent = 0x3E;
constexpr uint8_t kPositiveTesterPresent = 0x7E;
constexpr uint8_t kExtendedSession = 0x03;
}  // namespace

bool DidExplorer::knownRequestId(uint32_t requestId) {
  // The responders this vehicle's diagnostic gateway exposes. Anything else
  // would be an identifier guessed at, and a scan that sprays frames at a
  // guessed identifier is exactly the kind of traffic a body controller can
  // decide is a fault.
  return requestId >= 0x7E0 && requestId <= 0x7E7;
}

bool DidExplorer::validLink(const Config& config) {
  if (!knownRequestId(config.requestId)) return false;
  if (config.responseId != config.requestId + 8) return false;
  if (config.service != kServiceReadByIdentifier &&
      config.service != kServiceReadByLocalIdentifier) {
    return false;
  }
  return true;
}

bool DidExplorer::validConfig(const Config& config) {
  if (!validLink(config)) return false;
  if (config.lastDid < config.firstDid) return false;
  if (static_cast<uint32_t>(config.lastDid) - config.firstDid >= kMaxRange) {
    return false;
  }
  return true;
}

// The identifier the outstanding request is about. A range scan walks the
// range; a target run walks the list a previous scan produced.
uint16_t DidExplorer::activeDid() const {
  if (mode_ == Mode::Targets) {
    return targetIndex_ < targetCount_ ? targets_[targetIndex_] : 0;
  }
  return cursor_;
}

bool DidExplorer::exhausted() const {
  if (mode_ == Mode::Targets) return targetIndex_ >= targetCount_;
  return cursor_ > config_.lastDid;
}

uint32_t DidExplorer::totalDids() const {
  if (state_ == State::Idle) return 0;
  if (mode_ == Mode::Targets) return static_cast<uint32_t>(targetCount_);
  return static_cast<uint32_t>(config_.lastDid) - config_.firstDid + 1;
}

uint32_t DidExplorer::doneDids() const {
  if (state_ == State::Idle) return 0;
  if (mode_ == Mode::Targets) return static_cast<uint32_t>(targetIndex_);
  const uint32_t first = config_.firstDid;
  if (cursor_ < first) return 0;
  return static_cast<uint32_t>(cursor_) - first;
}

uint32_t DidExplorer::elapsedMs(uint32_t nowMs) const {
  if (state_ == State::Idle) return 0;
  const uint32_t end = (state_ == State::Running) ? nowMs : finishedAt_;
  return end - startedAt_;
}

const char* DidExplorer::stateName() const {
  switch (state_) {
    case State::Idle:
      return "idle";
    case State::Running:
      return "running";
    case State::Finished:
      return "finished";
    case State::Stopped:
      return "stopped";
    case State::Refused:
      return "refused";
  }
  return "idle";
}

bool DidExplorer::start(const Config& config, uint32_t nowMs) {
  stop(nowMs);
  if (!validConfig(config)) {
    state_ = State::Refused;
    return false;
  }
  config_ = config;
  state_ = State::Running;
  mode_ = Mode::RangeScan;
  pending_ = Pending::None;
  cursor_ = config.firstDid;
  targetCount_ = 0;
  targetIndex_ = 0;
  answerHead_ = 0;
  answerCount_ = 0;
  startedAt_ = nowMs;
  finishedAt_ = 0;
  // -kGapMs so the first request is due immediately rather than one pacing
  // interval after the button.
  lastSentAt_ = nowMs - kGapMs;
  lastKeepAliveAt_ = nowMs;
  sessionOpened_ = false;
  sessionRefused_ = false;
  counters_ = Counters{};
  resultCount_ = 0;
  resultsTruncated_ = false;
  for (size_t i = 0; i < kMaxResults; ++i) results_[i] = Result{};
  std::memset(buffer_, 0, sizeof(buffer_));
  expectedLength_ = 0;
  receivedLength_ = 0;
  nextSequence_ = 1;
  assembling_ = false;
  return true;
}

bool DidExplorer::startTargets(const Config& config, const uint16_t* dids,
                              size_t count, uint32_t nowMs) {
  stop(nowMs);
  // An empty list or one longer than the object can hold is a caller error, and
  // the honest answer is to refuse rather than to read part of what was asked
  // for and report a shorter snapshot as if it were complete.
  if (dids == nullptr || count == 0 || count > kMaxSnapshotTargets ||
      !validLink(config)) {
    state_ = State::Refused;
    return false;
  }
  if (!start(config, nowMs)) return false;
  for (size_t i = 0; i < count; ++i) targets_[i] = dids[i];
  targetCount_ = count;
  targetIndex_ = 0;
  cursor_ = config.firstDid;
  mode_ = Mode::Targets;
  return true;
}

void DidExplorer::stop(uint32_t nowMs) {
  if (state_ == State::Running) {
    state_ = State::Stopped;
    finishedAt_ = nowMs;
  }
  pending_ = Pending::None;
  assembling_ = false;
}

void DidExplorer::clear() {
  state_ = State::Idle;
  pending_ = Pending::None;
  config_ = Config{};
  cursor_ = 0;
  mode_ = Mode::RangeScan;
  targetCount_ = 0;
  targetIndex_ = 0;
  answerHead_ = 0;
  answerCount_ = 0;
  startedAt_ = 0;
  finishedAt_ = 0;
  lastSentAt_ = 0;
  lastKeepAliveAt_ = 0;
  sessionOpened_ = false;
  sessionRefused_ = false;
  counters_ = Counters{};
  resultCount_ = 0;
  resultsTruncated_ = false;
  for (size_t i = 0; i < kMaxResults; ++i) results_[i] = Result{};
  std::memset(buffer_, 0, sizeof(buffer_));
  expectedLength_ = 0;
  receivedLength_ = 0;
  assembling_ = false;
}

bool DidExplorer::due(uint32_t nowMs) const {
  if (state_ != State::Running) return false;
  if (pending_ != Pending::None) return false;
  return nowMs - lastSentAt_ >= kGapMs;
}

bool DidExplorer::buildRequest(uint32_t nowMs, twai_message_t& out) {
  if (!due(nowMs)) return false;

  out = {};
  out.identifier = config_.requestId;
  out.extd = 0;
  out.rtr = 0;
  out.data_length_code = 8;

  if (config_.extendedSession && !sessionOpened_ && !sessionRefused_) {
    out.data[0] = 0x02;
    out.data[1] = kServiceDiagnosticSessionControl;
    out.data[2] = kExtendedSession;
    pending_ = Pending::Session;
    lastSentAt_ = nowMs;
    return true;
  }

  if (config_.extendedSession && sessionOpened_ &&
      nowMs - lastKeepAliveAt_ >= kKeepAliveMs) {
    out.data[0] = 0x02;
    out.data[1] = kServiceTesterPresent;
    out.data[2] = 0x00;
    pending_ = Pending::KeepAlive;
    lastSentAt_ = nowMs;
    lastKeepAliveAt_ = nowMs;
    return true;
  }

  if (exhausted()) {
    completeScan(nowMs);
    return false;
  }

  const uint16_t did = activeDid();
  if (config_.service == kServiceReadByIdentifier) {
    out.data[0] = 0x03;
    out.data[1] = kServiceReadByIdentifier;
    out.data[2] = static_cast<uint8_t>(did >> 8);
    out.data[3] = static_cast<uint8_t>(did & 0xFF);
  } else {
    // A local identifier is one byte. The range is masked here rather than
    // clamped so 0x0105 and 0x0205 do not silently become the same request.
    out.data[0] = 0x02;
    out.data[1] = kServiceReadByLocalIdentifier;
    out.data[2] = static_cast<uint8_t>(did & 0xFF);
  }
  pending_ = Pending::Scan;
  lastSentAt_ = nowMs;
  assembling_ = false;
  expectedLength_ = 0;
  receivedLength_ = 0;
  nextSequence_ = 1;
  return true;
}

bool DidExplorer::payloadMatches(const uint8_t* uds, size_t length) const {
  if (length < 1) return false;
  if (pending_ == Pending::Session) {
    return uds[0] == kPositiveSessionControl ||
           (uds[0] == kNegativeResponse && length >= 2 &&
            uds[1] == kServiceDiagnosticSessionControl);
  }
  if (pending_ == Pending::KeepAlive) {
    return uds[0] == kPositiveTesterPresent ||
           (uds[0] == kNegativeResponse && length >= 2 &&
            uds[1] == kServiceTesterPresent);
  }
  if (pending_ != Pending::Scan) return false;
  if (uds[0] == kNegativeResponse) {
    return length >= 2 && uds[1] == config_.service;
  }
  const uint16_t did = activeDid();
  if (config_.service == kServiceReadByIdentifier) {
    return uds[0] == kPositiveReadByIdentifier && length >= 3 &&
           uds[1] == static_cast<uint8_t>(did >> 8) &&
           uds[2] == static_cast<uint8_t>(did & 0xFF);
  }
  return uds[0] == kPositiveReadByLocalId && length >= 2 &&
         uds[1] == static_cast<uint8_t>(did & 0xFF);
}

void DidExplorer::recordResult(uint16_t did, uint8_t service,
                               const uint8_t* data, size_t length) {
  if (resultCount_ >= kMaxResults) {
    resultsTruncated_ = true;
    return;
  }
  Result& result = results_[resultCount_++];
  result.did = did;
  result.service = service;
  result.truncated = length > kMaxPayload;
  const size_t keep = result.truncated ? kMaxPayload : length;
  result.length = static_cast<uint8_t>(keep);
  std::memcpy(result.payload, data, keep);
}

void DidExplorer::acceptPayload(const uint8_t* uds, size_t length,
                                uint32_t nowMs) {
  if (pending_ == Pending::Session) {
    if (uds[0] == kPositiveSessionControl) {
      sessionOpened_ = true;
    } else {
      sessionRefused_ = true;
    }
    finishRequest();
    return;
  }
  if (pending_ == Pending::KeepAlive) {
    finishRequest();
    return;
  }
  if (pending_ == Pending::Scan) {
    if (uds[0] == kNegativeResponse) {
      ++counters_.negative;
      counters_.lastNrc = length >= 3 ? uds[2] : 0;
      if (mode_ == Mode::Targets) {
        pushAnswer(uds, length, static_cast<int8_t>(counters_.lastNrc), false);
      }
    } else if (config_.service == kServiceReadByIdentifier) {
      ++counters_.positive;
      if (mode_ == Mode::Targets) {
        pushAnswer(uds + 3, length - 3, -1, false);
      } else {
        recordResult(activeDid(), config_.service, uds + 3, length - 3);
      }
    } else {
      ++counters_.positive;
      if (mode_ == Mode::Targets) {
        pushAnswer(uds + 2, length - 2, -1, false);
      } else {
        recordResult(activeDid(), config_.service, uds + 2, length - 2);
      }
    }
    advance();
    finishRequest();
    if (state_ == State::Running && exhausted()) {
      completeScan(nowMs);
    }
    return;
  }
}

void DidExplorer::finishRequest() {
  pending_ = Pending::None;
  assembling_ = false;
  expectedLength_ = 0;
  receivedLength_ = 0;
  nextSequence_ = 1;
}

void DidExplorer::advance() {
  ++counters_.scanned;
  if (mode_ == Mode::Targets) {
    ++targetIndex_;
    return;
  }
  // One past the last identifier is a valid cursor: buildRequest() reads it as
  // "the range is exhausted" and finishes the scan without a second flag that
  // could disagree with the cursor.
  ++cursor_;
}

void DidExplorer::pushAnswer(const uint8_t* uds, size_t length, int8_t nrc,
                             bool silent) {
  if (answerCount_ >= kAnswerRing) {
    // The portal is gone or wedged. Dropping the oldest keeps the newest, which
    // is the reading a snapshot is actually after.
    answerHead_ = (answerHead_ + 1) % kAnswerRing;
    --answerCount_;
  }
  const size_t index = (answerHead_ + answerCount_) % kAnswerRing;
  Answer& slot = answers_[index];
  slot = Answer{};
  slot.did = activeDid();
  slot.service = config_.service;
  slot.refused = nrc >= 0;
  slot.silent = silent;
  slot.nrc = nrc >= 0 ? static_cast<uint8_t>(nrc) : 0;
  const size_t keep = length > kMaxPayload ? kMaxPayload : length;
  slot.length = static_cast<uint8_t>(keep);
  if (!silent && keep != 0) std::memcpy(slot.payload, uds, keep);
  ++answerCount_;
}

bool DidExplorer::takeAnswer(Answer& out) {
  if (answerCount_ == 0) return false;
  out = answers_[answerHead_];
  answerHead_ = (answerHead_ + 1) % kAnswerRing;
  --answerCount_;
  return true;
}

void DidExplorer::completeScan(uint32_t nowMs) {
  state_ = State::Finished;
  finishedAt_ = nowMs;
  pending_ = Pending::None;
  assembling_ = false;
}

void DidExplorer::requestTimedOut(uint32_t nowMs) {
  if (state_ != State::Running) {
    pending_ = Pending::None;
    assembling_ = false;
    return;
  }
  if (pending_ == Pending::Scan) {
    ++counters_.timeout;
    if (mode_ == Mode::Targets) pushAnswer(nullptr, 0, -1, true);
    advance();
  } else if (pending_ == Pending::Session) {
    // A module that will not open an extended session is still worth scanning
    // in the default one: that is where half the identification identifiers
    // live anyway.
    sessionRefused_ = true;
  }
  pending_ = Pending::None;
  assembling_ = false;
  expectedLength_ = 0;
  receivedLength_ = 0;
  nextSequence_ = 1;
  lastSentAt_ = nowMs;
  if (exhausted()) completeScan(nowMs);
}

void DidExplorer::transportFailed(uint32_t nowMs) {
  ++counters_.dropped;
  requestTimedOut(nowMs);
}

bool DidExplorer::handleFrame(const twai_message_t& in, uint32_t nowMs,
                              twai_message_t& flowControl,
                              bool& needsFlowControl) {
  needsFlowControl = false;
  if (state_ != State::Running || pending_ == Pending::None) return false;
  if (in.extd || in.rtr) return false;
  if (in.identifier != config_.responseId) return false;
  if (in.data_length_code < 2) return false;

  const uint8_t pci = in.data[0] >> 4;

  if (pci == 0x0) {
    const size_t udsLength = in.data[0] & 0x0F;
    if (udsLength < 1 || in.data_length_code < udsLength + 1) return false;
    const uint8_t* uds = &in.data[1];
    if (assembling_) {
      // A single frame while a multi-frame answer is being assembled means the
      // first frame was abandoned by the block. Drop the partial answer rather
      // than splicing two different messages together.
      ++counters_.dropped;
      finishRequest();
      return true;
    }
    if (!payloadMatches(uds, udsLength)) return false;
    acceptPayload(uds, udsLength, nowMs);
    return true;
  }

  if (pci == 0x1) {
    if (in.data_length_code < 8) return false;
    const size_t udsLength =
        (static_cast<size_t>(in.data[0] & 0x0F) << 8) | in.data[1];
    if (udsLength < 1 || udsLength > sizeof(buffer_)) return false;
    const uint8_t* uds = &in.data[2];
    const size_t firstLength = in.data_length_code - 2U;
    const size_t available = udsLength < firstLength ? udsLength : firstLength;
    if (!payloadMatches(uds, available)) return false;

    std::memset(buffer_, 0, sizeof(buffer_));
    std::memcpy(buffer_, uds, available);
    receivedLength_ = available;
    expectedLength_ = udsLength;
    nextSequence_ = 1;
    assembling_ = true;
    if (receivedLength_ >= expectedLength_) {
      acceptPayload(buffer_, expectedLength_, nowMs);
      return true;
    }

    flowControl = {};
    flowControl.identifier = config_.requestId;
    flowControl.extd = 0;
    flowControl.rtr = 0;
    flowControl.data_length_code = 8;
    flowControl.data[0] = 0x30;  // Continue To Send, unlimited block, no delay.
    needsFlowControl = true;
    return true;
  }

  if (pci == 0x2 && assembling_) {
    const uint8_t sequence = in.data[0] & 0x0F;
    if (sequence != nextSequence_) {
      ++counters_.dropped;
      finishRequest();
      return true;
    }
    nextSequence_ = static_cast<uint8_t>((nextSequence_ + 1) & 0x0F);
    for (uint8_t i = 1;
         i < in.data_length_code && receivedLength_ < expectedLength_; ++i) {
      buffer_[receivedLength_++] = in.data[i];
    }
    if (receivedLength_ >= expectedLength_) {
      acceptPayload(buffer_, expectedLength_, nowMs);
    }
    return true;
  }

  return false;
}
