// Host tests for the read-only identifier scan.
//
// The scan is the one piece of 0.4.37 that puts requests on the vehicle's bus
// that the vehicle did not ask for, so it is tested against the transport rules
// rather than only against its own happy path: which frames it emits, which
// answers it is allowed to claim, and what it does with the ones it must not.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "did_explorer.h"

namespace {

DidExplorer::Config baseConfig() {
  DidExplorer::Config c;
  c.requestId = 0x7E0;
  c.responseId = 0x7E8;
  c.service = 0x22;
  c.firstDid = 0xF180;
  c.lastDid = 0xF181;
  c.extendedSession = false;
  return c;
}

twai_message_t frame(uint32_t id, std::initializer_list<uint8_t> bytes) {
  twai_message_t m{};
  m.identifier = id;
  m.data_length_code = static_cast<uint8_t>(bytes.size());
  size_t i = 0;
  for (uint8_t b : bytes) {
    if (i < 8) m.data[i++] = b;
  }
  return m;
}

struct Rig {
  DidExplorer explorer;
  uint32_t now = 1000;

  bool build(twai_message_t& out) {
    out = {};
    return explorer.buildRequest(now, out);
  }
  // Sends one request and answers it with the supplied bytes.
  bool expectRequest(std::initializer_list<uint8_t> expected, bool full = false) {
    twai_message_t out{};
    if (!build(out)) return false;
    assert(out.identifier == 0x7E0);
    assert(out.data_length_code == 8);
    size_t i = 0;
    for (uint8_t b : expected) {
      assert(out.data[i++] == b);
    }
    if (full) {
      for (; i < 8; ++i) assert(out.data[i] == 0);
    }
    return true;
  }
  bool answer(std::initializer_list<uint8_t> payload, uint32_t id = 0x7E8) {
    twai_message_t f = frame(id, payload);
    twai_message_t fc{};
    bool needFc = false;
    const bool taken = explorer.handleFrame(f, now, fc, needFc);
    assert(!needFc);
    return taken;
  }
};

}  // namespace

int main() {
  // ---- configuration bounds -------------------------------------------------
  {
    DidExplorer explorer;
    DidExplorer::Config c = baseConfig();
    assert(DidExplorer::validConfig(c));
    c.responseId = 0x7E9;
    assert(!DidExplorer::validConfig(c));
    c = baseConfig();
    c.requestId = 0x7F0;
    c.responseId = 0x7F8;
    assert(!DidExplorer::validConfig(c));
    c = baseConfig();
    c.service = 0x2E;  // WriteDataByIdentifier: not this feature.
    assert(!DidExplorer::validConfig(c));
    c = baseConfig();
    c.lastDid = c.firstDid - 1;
    assert(!DidExplorer::validConfig(c));
    c = baseConfig();
    c.lastDid = static_cast<uint16_t>(c.firstDid + DidExplorer::kMaxRange);
    assert(!DidExplorer::validConfig(c));
    twai_message_t out{};
    assert(!explorer.start(c, 0));
    assert(explorer.state() == DidExplorer::State::Refused);
    assert(!explorer.buildRequest(0, out));
  }

  // ---- the request format ---------------------------------------------------
  {
    Rig rig;
    assert(rig.explorer.start(baseConfig(), rig.now));
    assert(rig.expectRequest({0x03, 0x22, 0xF1, 0x80}, true));
    assert(rig.explorer.waiting());

    // A second build before the answer must not emit a second request: one
    // request slot is what keeps this from interleaving with Mode 01.
    twai_message_t out{};
    assert(!rig.build(out));

    // Positive answer for F180 with two bytes of data.
    assert(rig.answer({0x05, 0x62, 0xF1, 0x80, 0x41, 0x42}));
    assert(!rig.explorer.waiting());
    assert(rig.explorer.resultCount() == 1);
    const DidExplorer::Result& r = rig.explorer.results()[0];
    assert(r.did == 0xF180 && r.service == 0x22 && r.length == 2);
    assert(r.payload[0] == 0x41 && r.payload[1] == 0x42);
    assert(rig.explorer.counters().positive == 1);
    assert(rig.explorer.counters().scanned == 1);
    assert(rig.explorer.currentDid() == 0xF181);
  }

  // ---- pacing ---------------------------------------------------------------
  {
    Rig rig;
    assert(rig.explorer.start(baseConfig(), rig.now));
    twai_message_t out{};
    assert(rig.build(out));
    rig.explorer.requestTimedOut(rig.now);
    // 30 ms later is too soon; the floor exists so a scan cannot turn into a
    // burst that competes with the poller for the bus.
    rig.now += DidExplorer::kGapMs - 1;
    assert(!rig.build(out));
    rig.now += 1;
    assert(rig.build(out));
  }

  // ---- negative answer and timeout both advance ------------------------------
  {
    Rig rig;
    assert(rig.explorer.start(baseConfig(), rig.now));
    twai_message_t out{};
    assert(rig.build(out));
    assert(rig.answer({0x03, 0x7F, 0x22, 0x31}));  // requestOutOfRange
    assert(rig.explorer.counters().negative == 1);
    assert(rig.explorer.counters().lastNrc == 0x31);
    assert(rig.explorer.currentDid() == 0xF181);

    rig.now += DidExplorer::kGapMs;
    assert(rig.build(out));
    rig.explorer.requestTimedOut(rig.now);
    assert(rig.explorer.counters().timeout == 1);
    assert(rig.explorer.counters().scanned == 2);
    // F181 was the last identifier, so the scan is over.
    assert(rig.explorer.state() == DidExplorer::State::Finished);
  }

  // ---- multi-frame answer ---------------------------------------------------
  {
    DidExplorer::Config c = baseConfig();
    c.firstDid = 0xF190;
    c.lastDid = 0xF190;
    Rig rig;
    assert(rig.explorer.start(c, rig.now));
    twai_message_t out{};
    assert(rig.build(out));

    // First frame: 0x0B bytes of UDS payload, the head of a VIN-shaped answer.
    twai_message_t ff = frame(0x7E8, {0x10, 0x0B, 0x62, 0xF1, 0x90, 0x4C,
                                      0x47, 0x57});
    twai_message_t fc{};
    bool needFc = false;
    assert(rig.explorer.handleFrame(ff, rig.now, fc, needFc));
    assert(needFc);
    assert(fc.identifier == 0x7E0);
    assert(fc.data_length_code == 8);
    assert(fc.data[0] == 0x30);
    assert(rig.explorer.waiting());

    // Consecutive frame with the remaining five bytes.
    twai_message_t cf = frame(0x7E8, {0x21, 0x45, 0x45, 0x34, 0x41, 0x34});
    assert(rig.explorer.handleFrame(cf, rig.now, fc, needFc));
    assert(!needFc);
    assert(!rig.explorer.waiting());
    assert(rig.explorer.resultCount() == 1);
    const DidExplorer::Result& r = rig.explorer.results()[0];
    assert(r.did == 0xF190);
    assert(r.length == 8);  // 11 payload bytes minus service and identifier
    assert(r.payload[0] == 0x4C && r.payload[1] == 0x47 && r.payload[7] == 0x34);
    assert(rig.explorer.state() == DidExplorer::State::Finished);
  }

  // ---- sequence mismatch drops the answer instead of splicing it ------------
  {
    DidExplorer::Config c = baseConfig();
    c.firstDid = 0xF190;
    c.lastDid = 0xF190;
    Rig rig;
    assert(rig.explorer.start(c, rig.now));
    twai_message_t out{};
    assert(rig.build(out));
    twai_message_t fc{};
    bool needFc = false;
    twai_message_t ff = frame(0x7E8, {0x10, 0x0B, 0x62, 0xF1, 0x90, 0x41,
                                      0x41, 0x41});
    assert(rig.explorer.handleFrame(ff, rig.now, fc, needFc));
    // Sequence 2 where 1 was expected.
    twai_message_t cf = frame(0x7E8, {0x22, 0x42, 0x42, 0x42, 0x42, 0x42});
    assert(rig.explorer.handleFrame(cf, rig.now, fc, needFc));
    assert(rig.explorer.resultCount() == 0);
    assert(rig.explorer.counters().dropped == 1);
    assert(!rig.explorer.waiting());
  }

  // ---- frames that are not ours ---------------------------------------------
  {
    Rig rig;
    assert(rig.explorer.start(baseConfig(), rig.now));
    twai_message_t out{};
    assert(rig.build(out));

    // A Mode 01 answer from the same module. The poller owns 0x41; the explorer
    // must hand this frame back, or the gauge would start missing readings
    // whenever a scan is running.
    twai_message_t fc{};
    bool needFc = false;
    assert(!rig.explorer.handleFrame(
        frame(0x7E8, {0x04, 0x41, 0x0C, 0x1F, 0x40}), rig.now, fc, needFc));
    // Another module's 0x62.
    assert(!rig.explorer.handleFrame(
        frame(0x7E9, {0x05, 0x62, 0xF1, 0x80, 0x00, 0x00}), rig.now, fc,
        needFc));
    // The right frame for a different identifier.
    assert(!rig.explorer.handleFrame(
        frame(0x7E8, {0x05, 0x62, 0xF1, 0x87, 0x01, 0x02}), rig.now, fc,
        needFc));
    // Extended identifiers are not part of this bus.
    twai_message_t ext = frame(0x18DAF110, {0x05, 0x62, 0xF1, 0x80, 0x01, 0x02});
    ext.extd = 1;
    assert(!rig.explorer.handleFrame(ext, rig.now, fc, needFc));
    // Nor is the same identifier arriving as a 29-bit frame: 0x7E8 as an 11-bit
    // identifier and 0x000007E8 as a 29-bit one are different PGNs on this bus.
    twai_message_t extSame = frame(0x7E8, {0x05, 0x62, 0xF1, 0x80, 0x01, 0x02});
    extSame.extd = 1;
    assert(!rig.explorer.handleFrame(extSame, rig.now, fc, needFc));
    // A remote frame carries no data and must never be read as an answer.
    twai_message_t rtr = frame(0x7E8, {0x05, 0x62, 0xF1, 0x80, 0x01, 0x02});
    rtr.rtr = 1;
    assert(!rig.explorer.handleFrame(rtr, rig.now, fc, needFc));
    assert(rig.explorer.waiting());
    assert(rig.explorer.resultCount() == 0);

    // And when nothing is outstanding, even an exact-looking answer is ignored.
    assert(rig.answer({0x05, 0x62, 0xF1, 0x80, 0x01, 0x02}));
    assert(!rig.explorer.waiting());
    assert(!rig.explorer.handleFrame(
        frame(0x7E8, {0x05, 0x62, 0xF1, 0x81, 0x01, 0x02}), rig.now, fc,
        needFc));
  }

  // ---- extended session and tester present ----------------------------------
  {
    DidExplorer::Config c = baseConfig();
    c.extendedSession = true;
    Rig rig;
    assert(rig.explorer.start(c, rig.now));
    // 0x10 03 first, not a read.
    assert(rig.expectRequest({0x02, 0x10, 0x03}, true));
    assert(rig.answer({0x06, 0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}));
    assert(rig.explorer.sessionOpened());
    assert(!rig.explorer.sessionRefused());

    rig.now += DidExplorer::kGapMs;
    assert(rig.expectRequest({0x03, 0x22, 0xF1, 0x80}, true));
    assert(rig.answer({0x05, 0x62, 0xF1, 0x80, 0x01, 0x02}));

    // Two seconds of scanning is two seconds without a tester present, which is
    // where an ECU on the S3 timer drops the session. The keep-alive must go out
    // before the next identifier can.
    rig.now += DidExplorer::kKeepAliveMs;
    assert(rig.expectRequest({0x02, 0x3E, 0x00}, true));
    assert(rig.answer({0x02, 0x7E, 0x00}));
    rig.now += DidExplorer::kGapMs;
    assert(rig.expectRequest({0x03, 0x22, 0xF1, 0x81}, true));
  }

  // ---- a refused session still scans ----------------------------------------
  {
    DidExplorer::Config c = baseConfig();
    c.extendedSession = true;
    Rig rig;
    assert(rig.explorer.start(c, rig.now));
    twai_message_t out{};
    assert(rig.build(out));
    assert(rig.answer({0x03, 0x7F, 0x10, 0x12}));  // subFunctionNotSupported
    assert(rig.explorer.sessionRefused());
    rig.now += DidExplorer::kGapMs;
    assert(rig.expectRequest({0x03, 0x22, 0xF1, 0x80}, true));
    rig.explorer.requestTimedOut(rig.now);
    rig.now += DidExplorer::kGapMs;
    assert(rig.expectRequest({0x03, 0x22, 0xF1, 0x81}, true));
  }

  // ---- a session request that is never answered also falls back -------------
  {
    DidExplorer::Config c = baseConfig();
    c.extendedSession = true;
    Rig rig;
    assert(rig.explorer.start(c, rig.now));
    twai_message_t out{};
    assert(rig.build(out));
    assert(out.data[1] == 0x10);
    rig.explorer.requestTimedOut(rig.now);
    assert(rig.explorer.sessionRefused());
    assert(rig.explorer.counters().timeout == 0);  // not a scan result
    rig.now += DidExplorer::kGapMs;
    assert(rig.expectRequest({0x03, 0x22, 0xF1, 0x80}, true));
  }

  // ---- a keep-alive answer is not counted as a scan result ------------------
  {
    DidExplorer::Config c = baseConfig();
    c.extendedSession = true;
    Rig rig;
    assert(rig.explorer.start(c, rig.now));
    twai_message_t out{};
    assert(rig.build(out));
    assert(rig.answer({0x06, 0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}));
    assert(rig.explorer.counters().scanned == 0);
    rig.now += DidExplorer::kKeepAliveMs;
    assert(rig.expectRequest({0x02, 0x3E, 0x00}, true));
    assert(rig.answer({0x02, 0x7E, 0x00}));
    assert(rig.explorer.counters().scanned == 0);
    assert(rig.explorer.resultCount() == 0);
    assert(rig.explorer.currentDid() == 0xF180);
  }

  // ---- local identifiers are one byte ---------------------------------------
  {
    DidExplorer::Config c = baseConfig();
    c.service = 0x21;
    c.firstDid = 0x00;
    c.lastDid = 0x01;
    Rig rig;
    assert(rig.explorer.start(c, rig.now));
    assert(rig.expectRequest({0x02, 0x21, 0x00}, true));
    assert(rig.answer({0x04, 0x61, 0x00, 0xAA, 0xBB}));
    assert(rig.explorer.resultCount() == 1);
    assert(rig.explorer.results()[0].did == 0x0000);
    assert(rig.explorer.results()[0].length == 2);
    assert(rig.explorer.results()[0].payload[0] == 0xAA);
  }

  // ---- long answers are truncated, not overflowed ---------------------------
  {
    DidExplorer::Config c = baseConfig();
    c.firstDid = 0xF1A0;
    c.lastDid = 0xF1A0;
    Rig rig;
    assert(rig.explorer.start(c, rig.now));
    twai_message_t out{};
    assert(rig.build(out));

    // 0x20 payload bytes: 3 header + 25 data, more than kMaxPayload keeps.
    twai_message_t ff = frame(0x7E8, {0x10, 0x20, 0x62, 0xF1, 0xA0, 0x01,
                                      0x02, 0x03});
    twai_message_t fc{};
    bool needFc = false;
    assert(rig.explorer.handleFrame(ff, rig.now, fc, needFc));
    assert(needFc);

    uint8_t seq = 1;
    bool done = false;
    while (!done) {
      twai_message_t cf{};
      cf.identifier = 0x7E8;
      cf.data_length_code = 8;
      cf.data[0] = static_cast<uint8_t>(0x20 | (seq & 0x0F));
      for (int i = 1; i < 8; ++i) cf.data[i] = static_cast<uint8_t>(seq * 8 + i);
      const bool more = rig.explorer.waiting();
      assert(rig.explorer.handleFrame(cf, rig.now, fc, needFc));
      done = !more || !rig.explorer.waiting();
      ++seq;
      if (seq > 16) break;
    }
    assert(rig.explorer.resultCount() == 1);
    const DidExplorer::Result& r = rig.explorer.results()[0];
    assert(r.truncated);
    assert(r.length == DidExplorer::kMaxPayload);
    assert(rig.explorer.counters().positive == 1);
  }


  // ---- target runs, which is what a control snapshot uses -------------------
  {
    const uint16_t targets[] = {0xF1A0, 0x1234, 0xF1A0};
    DidExplorer::Config c = baseConfig();
    DidExplorer explorer;
    assert(explorer.startTargets(c, targets, 3, 500));
    assert(explorer.mode() == DidExplorer::Mode::Targets);
    assert(explorer.running());
    assert(explorer.totalDids() == 3);
    assert(explorer.currentDid() == 0xF1A0);

    twai_message_t out{};
    twai_message_t fc{};
    bool needFc = false;

    // First target answers with data.
    assert(explorer.buildRequest(500, out));
    assert(out.data[2] == 0xF1 && out.data[3] == 0xA0);
    assert(explorer.handleFrame(
        frame(0x7E8, {0x05, 0x62, 0xF1, 0xA0, 0x11, 0x22}), 520, fc, needFc));

    // Second target answers with a refusal.
    assert(explorer.buildRequest(560, out));
    assert(out.data[2] == 0x12 && out.data[3] == 0x34);
    assert(explorer.handleFrame(frame(0x7E8, {0x03, 0x7F, 0x22, 0x31}), 580,
                                fc, needFc));

    // Third target stays silent, so the client times it out.
    assert(explorer.buildRequest(620, out));
    assert(out.data[2] == 0xF1 && out.data[3] == 0xA0);
    explorer.requestTimedOut(800);
    assert(explorer.state() == DidExplorer::State::Finished);

    DidExplorer::Answer answer{};
    assert(explorer.takeAnswer(answer));
    assert(answer.did == 0xF1A0 && answer.length == 2 && !answer.refused &&
           !answer.silent);
    assert(answer.payload[0] == 0x11 && answer.payload[1] == 0x22);
    assert(explorer.takeAnswer(answer));
    assert(answer.did == 0x1234 && answer.refused && answer.nrc == 0x31);
    assert(explorer.takeAnswer(answer));
    assert(answer.did == 0xF1A0 && answer.silent && !answer.refused);
    assert(!explorer.takeAnswer(answer));
  }

  // ---- a target run still ignores an answer for a different identifier ------
  {
    const uint16_t targets[] = {0xF1A0};
    DidExplorer::Config c = baseConfig();
    DidExplorer explorer;
    assert(explorer.startTargets(c, targets, 1, 0));
    twai_message_t out{};
    twai_message_t fc{};
    bool needFc = false;
    assert(explorer.buildRequest(0, out));
    // The range this configuration names is irrelevant to a target run, so an
    // answer to a range address must not be taken.
    assert(!explorer.handleFrame(
        frame(0x7E8, {0x05, 0x62, 0xF1, 0x80, 0x01, 0x02}), 10, fc, needFc));
    assert(explorer.waiting());
    DidExplorer::Answer answer{};
    assert(!explorer.takeAnswer(answer));
  }

  // ---- the answer ring keeps the newest entries when it overflows -----------
  {
    const uint16_t targets[] = {0x0001, 0x0002, 0x0003, 0x0004, 0x0005};
    DidExplorer::Config c = baseConfig();
    DidExplorer explorer;
    assert(explorer.startTargets(c, targets, 5, 0));
    twai_message_t out{};
    twai_message_t fc{};
    bool needFc = false;
    uint32_t now = 0;
    for (size_t i = 0; i < 5; ++i) {
      assert(explorer.buildRequest(now, out));
      const uint8_t didHi = static_cast<uint8_t>(targets[i] >> 8);
      const uint8_t didLo = static_cast<uint8_t>(targets[i] & 0xFF);
      assert(explorer.handleFrame(
          frame(0x7E8, {0x04, 0x62, didHi, didLo, 0x00}), now + 10, fc,
          needFc));
      now += 100;
    }
    // Only kAnswerRing entries survive, and they are the last ones asked.
    DidExplorer::Answer answer{};
    size_t kept = 0;
    uint16_t first = 0;
    while (explorer.takeAnswer(answer)) {
      if (kept == 0) first = answer.did;
      ++kept;
    }
    assert(kept == DidExplorer::kAnswerRing);
    assert(first == 0x0002);
  }

  // ---- a target run refuses a list it cannot honour -------------------------
  {
    DidExplorer::Config c = baseConfig();
    DidExplorer explorer;
    assert(!explorer.startTargets(c, nullptr, 1, 0));
    assert(explorer.state() == DidExplorer::State::Refused);
    const uint16_t one[] = {0x0001};
    assert(!explorer.startTargets(c, one, 0, 0));
    assert(explorer.state() == DidExplorer::State::Refused);
    uint16_t tooMany[DidExplorer::kMaxSnapshotTargets + 1] = {0};
    assert(!explorer.startTargets(c, tooMany, DidExplorer::kMaxSnapshotTargets + 1,
                                 0));
    assert(explorer.state() == DidExplorer::State::Refused);
    // A wrong responder is refused for the same reason a range scan refuses it.
    DidExplorer::Config bad = c;
    bad.responseId = 0x7E9;
    assert(!explorer.startTargets(bad, one, 1, 0));
    // And a good list still works afterwards, so a refusal leaves no residue.
    assert(explorer.startTargets(c, one, 1, 0));
    assert(explorer.running());
  }

  // ---- the result list is bounded -------------------------------------------
  {
    DidExplorer::Config c = baseConfig();
    c.firstDid = 0x0100;
    c.lastDid = static_cast<uint16_t>(0x0100 + DidExplorer::kMaxResults +
                                      DidExplorer::kMaxResults - 1);
    Rig rig;
    assert(rig.explorer.start(c, rig.now));
    twai_message_t out{};
    for (size_t i = 0; i < DidExplorer::kMaxResults + 4; ++i) {
      assert(rig.build(out));
      const uint16_t did = static_cast<uint16_t>(0x0100 + i);
      assert(rig.answer({0x05, 0x62, static_cast<uint8_t>(did >> 8),
                         static_cast<uint8_t>(did & 0xFF), 0x00, 0x00}));
      rig.now += DidExplorer::kGapMs;
    }
    assert(rig.explorer.resultCount() == DidExplorer::kMaxResults);
    assert(rig.explorer.resultsTruncated());
    assert(rig.explorer.counters().positive == DidExplorer::kMaxResults + 4);
  }

  // ---- stop ends it, and nothing is emitted afterwards ----------------------
  {
    Rig rig;
    assert(rig.explorer.start(baseConfig(), rig.now));
    twai_message_t out{};
    assert(rig.build(out));
    rig.explorer.stop(rig.now);
    assert(rig.explorer.state() == DidExplorer::State::Stopped);
    assert(!rig.explorer.waiting());
    rig.now += 1000;
    assert(!rig.build(out));
    // An answer that arrives after the stop is not claimed either.
    twai_message_t fc{};
    bool needFc = false;
    assert(!rig.explorer.handleFrame(
        frame(0x7E8, {0x05, 0x62, 0xF1, 0x80, 0x01, 0x02}), rig.now, fc,
        needFc));
  }

  std::printf("PASS read-only identifier scan: request format, ISO-TP assembly, "
              "negative answers, timeouts, foreign frames, session handling, "
              "bounded results, target runs for control snapshots\n");
  return 0;
}
