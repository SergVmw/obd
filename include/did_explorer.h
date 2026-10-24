#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/twai.h"

// Self-contained, read-only UDS identifier scan for the service portal.
//
// Why it exists: the Haval's Bosch ME17.8.8 tells anything useful about the
// engine - the pre-throttle pressure the boost gauge is really asking about,
// the target boost, the wastegate duty, the gearbox temperature - through
// manufacturer identifiers that Mode 01 does not expose. Finding out which
// identifier carries which value needs a request/answer exchange against the
// vehicle, and the owner asked for that exchange to happen on this device,
// with no laptop, no ELM327 and no second tester on the bus.
//
// It is deliberately the narrowest thing that can answer the question:
//
//   * reads only. Service 0x22 (ReadDataByIdentifier) and 0x21
//     (ReadDataByLocalIdentifier) as the user selects, plus an OPTIONAL
//     session request (0x10 03) with the tester-present it requires, which no
//     scanner omits and which writes nothing.
//   * one module, chosen from the functional responders this bus has: the
//     engine at 0x7E0/0x7E8 and the transmission at 0x7E1/0x7E9. The request
//     identifier must be one of 0x7E0..0x7E7 and the answer identifier is
//     required to be exactly request + 8, so a typo cannot spray identifiers
//     across the bus.
//   * a bounded range. 512 identifiers per scan, 30 ms apart, on top of the
//     ordinary Mode 01 profile - roughly 20 requests a second, inside the same
//     maxRequestsPerSecond budget the poller already respects.
//   * a bounded answer. 20 bytes kept per identifier, 48 identifiers kept:
//     enough for a VIN-shaped answer and for a page of results, small enough to
//     live in the object rather than the heap.
class DidExplorer {
 public:
  static constexpr size_t kMaxResults = 48;
  static constexpr size_t kMaxPayload = 20;
  // A control snapshot re-reads the identifiers the scan already found, so the
  // list it walks is short: the point of the snapshot is to compare a handful
  // of candidate sensors across three engine states, not to scan again.
  static constexpr size_t kMaxSnapshotTargets = 24;
  // Answers from a target run, waiting for the portal to collect them. Four
  // deep: the portal's loop drains this every few milliseconds and a single
  // identifier cannot answer faster than the pacing gap, so the ring only has
  // to absorb one loop's worth of arrivals rather than a backlog.
  static constexpr size_t kAnswerRing = 4;
  static constexpr uint16_t kMaxRange = 512;
  static constexpr uint32_t kRequestTimeoutMs = 150;
  static constexpr uint32_t kGapMs = 30;
  // Tester present: S3 is five seconds of silence before an ECU drops an
  // extended session, and two is the interval every scanner uses.
  static constexpr uint32_t kKeepAliveMs = 2000;

  enum class State : uint8_t { Idle, Running, Finished, Stopped, Refused };

  // A range scan walks an address range looking for anything that answers. A
  // target run re-reads a list of identifiers that a previous scan already
  // found, which is what a control snapshot is: the same identifiers under
  // three different engine states, each beside the manifold pressure and the
  // engine speed the gauge was reading at that instant.
  enum class Mode : uint8_t { RangeScan, Targets };

  // What the outstanding request is, when one is outstanding.
  enum class Pending : uint8_t { None, Session, KeepAlive, Scan };

  struct Config {
    uint32_t requestId = 0x7E0;
    uint32_t responseId = 0x7E8;
    uint8_t service = 0x22;
    uint16_t firstDid = 0xF180;
    uint16_t lastDid = 0xF1FF;
    bool extendedSession = false;
  };

  struct Result {
    uint16_t did = 0;
    uint8_t service = 0x22;
    uint8_t length = 0;
    bool truncated = false;
    uint8_t payload[kMaxPayload] = {0};
  };

  // One answer taken during a target run, handed to the portal for a snapshot.
  // `refused` and `silent` are both outcomes worth recording: an identifier
  // that answered during the scan and does not answer under load is telling the
  // owner something about the module, not about the wire.
  struct Answer {
    uint16_t did = 0;
    uint8_t service = 0x22;
    uint8_t length = 0;
    uint8_t nrc = 0;
    bool refused = false;
    bool silent = false;
    uint8_t payload[kMaxPayload] = {0};
  };

  struct Counters {
    uint32_t scanned = 0;    // identifiers the scan moved past
    uint32_t positive = 0;   // answered with data
    uint32_t negative = 0;   // answered with a negative response code
    uint32_t timeout = 0;    // no answer at all
    uint32_t dropped = 0;    // answer arrived that the request could not own
    uint8_t lastNrc = 0;     // the most recent negative response code
  };

  bool start(const Config& config, uint32_t nowMs);
  // Re-reads `count` identifiers (at most kMaxSnapshotTargets) in the order
  // given. The link part of the configuration is validated exactly as for a
  // range scan; the range itself is not used.
  bool startTargets(const Config& config, const uint16_t* dids, size_t count,
                    uint32_t nowMs);
  void stop(uint32_t nowMs);
  void clear();

  // Drains one answer captured by a target run. The portal calls this until it
  // returns false, stamping each entry with the manifold pressure and engine
  // speed current at that moment.
  bool takeAnswer(Answer& out);

  bool running() const { return state_ == State::Running; }
  bool waiting() const { return pending_ != Pending::None; }
  State state() const { return state_; }
  const char* stateName() const;
  const Config& config() const { return config_; }
  const Counters& counters() const { return counters_; }
  const Result* results() const { return results_; }
  size_t resultCount() const { return resultCount_; }
  bool resultsTruncated() const { return resultsTruncated_; }
  uint16_t currentDid() const { return activeDid(); }
  Mode mode() const { return mode_; }
  size_t targetCount() const { return targetCount_; }
  size_t targetIndex() const { return targetIndex_; }
  uint32_t totalDids() const;
  uint32_t doneDids() const;
  uint32_t elapsedMs(uint32_t nowMs) const;
  bool sessionOpened() const { return sessionOpened_; }
  bool sessionRefused() const { return sessionRefused_; }

  // True when nothing can be sent yet, without touching any state. The client
  // asks this before it builds a frame, so a paced request cannot be built and
  // then dropped on the floor while the request slot is busy.
  bool due(uint32_t nowMs) const;

  // Builds one request. Returns false when nothing is due.
  bool buildRequest(uint32_t nowMs, twai_message_t& out);

  // Consumes an answer that belongs to the outstanding request. Returns false
  // for every frame that is not ours, including a Mode 01 answer from the same
  // module - the explorer sits in the same receive path as the poller and must
  // never swallow its traffic.
  bool handleFrame(const twai_message_t& in, uint32_t nowMs,
                   twai_message_t& flowControl, bool& needsFlowControl);

  // Called by the client when the request slot expires.
  void requestTimedOut(uint32_t nowMs);
  // Called when the frame could not be transmitted at all.
  void transportFailed(uint32_t nowMs);

  static bool validConfig(const Config& config);
  // The part of the configuration that says where to send and what to ask.
  // Split out because a target run has no range to validate but must satisfy
  // exactly the same addressing rules.
  static bool validLink(const Config& config);

 private:
  static bool knownRequestId(uint32_t requestId);
  uint16_t activeDid() const;
  bool exhausted() const;
  void pushAnswer(const uint8_t* uds, size_t length, int8_t nrc, bool silent);
  void finishRequest();
  void advance();
  void completeScan(uint32_t nowMs);
  void recordResult(uint16_t did, uint8_t service, const uint8_t* data,
                    size_t length);
  // Whether a decoded UDS payload answers the outstanding request.
  bool payloadMatches(const uint8_t* uds, size_t length) const;
  void acceptPayload(const uint8_t* uds, size_t length, uint32_t nowMs);

  Config config_{};
  State state_ = State::Idle;
  Mode mode_ = Mode::RangeScan;
  Pending pending_ = Pending::None;

  uint16_t cursor_ = 0;
  uint16_t targets_[kMaxSnapshotTargets]{};
  size_t targetCount_ = 0;
  size_t targetIndex_ = 0;
  uint32_t startedAt_ = 0;
  uint32_t lastSentAt_ = 0;
  uint32_t lastKeepAliveAt_ = 0;
  uint32_t finishedAt_ = 0;
  bool sessionOpened_ = false;
  bool sessionRefused_ = false;

  Counters counters_{};
  Result results_[kMaxResults]{};
  size_t resultCount_ = 0;
  bool resultsTruncated_ = false;

  Answer answers_[kAnswerRing]{};
  size_t answerHead_ = 0;
  size_t answerCount_ = 0;

  // ISO-TP assembly for the answer in flight.
  uint8_t buffer_[64]{};
  size_t expectedLength_ = 0;
  size_t receivedLength_ = 0;
  uint8_t nextSequence_ = 1;
  bool assembling_ = false;
};
