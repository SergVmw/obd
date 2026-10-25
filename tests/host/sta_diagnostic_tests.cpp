// Host tests for the Wi-Fi station failure report.
//
// The device can print one word on the panel - "WIFI: ПОИСК" - whatever went
// wrong underneath: 5 GHz, a hidden SSID, an invisible trailing space in the
// name, WPA3 where the stack speaks WPA2, or an access point that refuses the
// association. The only thing that separates them is the reason number
// esp_wifi reports, so this mapping is the whole diagnosis. It is tested here,
// on the host, because a wrong or missing token would put the owner back in
// front of a spinner.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sta_diagnostic.h"

namespace {

int testsRun = 0;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                   #expression);                                              \
      std::abort();                                                           \
    }                                                                         \
  } while (0)

void test(const char* name, void (*body)()) {
  body();
  ++testsRun;
  std::printf("[ok] %s\n", name);
}

#define TEST_CASE(name)                                                       \
  static void name();                                                         \
  struct name##Registration {                                                 \
    name##Registration() { test(#name, name); }                               \
  } name##Instance;                                                           \
  static void name()

TEST_CASE(everyCodeHasATokenAndAHint) {
  // The set esp_wifi actually produces in the field, plus the ones a bad
  // hotspot throws. Every one of them must come back with something to show.
  const int codes[] = {2,  3,  4,           6,   8,   9,   15,
                       200, 201, 202,       203, 204, 205, 210,
                       211, 212};
  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
    const char* token = StaDiagnostic::reasonCode(codes[i]);
    const char* hint = StaDiagnostic::reasonHint(codes[i]);
    CHECK(token != nullptr && token[0] != '\0');
    CHECK(hint != nullptr && hint[0] != '\0');
    // The unknown bucket must stay reserved for codes nobody mapped; if a
    // mapped code fell into it, the number would be the only thing left and
    // the portal would say "причина не расшифрована" about a known cause.
    CHECK(std::strcmp(token, "unknown") != 0);
    CHECK(std::strcmp(hint, StaDiagnostic::reasonHint(-999)) != 0);
  }
}

TEST_CASE(unknownCodeKeepsItsNumber) {
  // A reason the table does not know must still be reported as a number and
  // still be distinguishable from "no failure yet" (-1). Losing it would send
  // the next person back to guessing with no evidence at all.
  CHECK(std::strcmp(StaDiagnostic::reasonCode(9999), "unknown") == 0);
  CHECK(std::strcmp(StaDiagnostic::reasonCode(-1), "unknown") == 0);
  CHECK(StaDiagnostic::reasonHint(9999)[0] != '\0');
  // -1 is the "never failed" marker used by the status struct, and it must not
  // look like a real hint.
  CHECK(std::strcmp(StaDiagnostic::reasonHint(-1),
                    StaDiagnostic::reasonHint(201)) != 0);
}

TEST_CASE(theTwoFamiliesAreToldApart) {
  // The one distinction the display cannot make: "the frame never arrived"
  // against "the frame arrived and was refused". Band, hidden SSID and a wrong
  // name are in the first family; password and security mode are in the
  // second. A hint from the wrong family sends the owner to the wrong setting,
  // which is exactly the round trip this exists to prevent.
  const int neverArrived[] = {201, 210, 211, 212};
  const int refused[] = {2, 15, 202, 203, 204, 205};
  for (size_t i = 0; i < sizeof(neverArrived) / sizeof(neverArrived[0]); ++i) {
    for (size_t j = 0; j < sizeof(refused) / sizeof(refused[0]); ++j) {
      CHECK(std::strcmp(StaDiagnostic::reasonHint(neverArrived[i]),
                        StaDiagnostic::reasonHint(refused[j])) != 0);
    }
  }
  // And within the families the tokens must not collapse either: 201 and 210
  // lead to different actions (check the band against check the security).
  CHECK(std::strcmp(StaDiagnostic::reasonCode(201),
                    StaDiagnostic::reasonCode(210)) != 0);
  CHECK(std::strcmp(StaDiagnostic::reasonCode(202),
                    StaDiagnostic::reasonCode(204)) != 0);
}

TEST_CASE(countersDescribeTheShapeOfTheFailure) {
  StaDiagnostic::Tracker tracker;
  CHECK(tracker.attempts == 0 && tracker.disconnects == 0);
  CHECK(!tracker.hasReason);
  CHECK(tracker.lastReason == -1);

  tracker.noteAttempt();
  tracker.noteDisconnect(201);
  tracker.noteAttempt();
  tracker.noteDisconnect(201);
  CHECK(tracker.attempts == 2);
  CHECK(tracker.disconnects == 2);
  CHECK(tracker.hasReason);
  CHECK(tracker.lastReason == 201);

  // A later, different failure is the one that matters: the last reason is
  // what the portal shows, and it has to move.
  tracker.noteAttempt();
  tracker.noteDisconnect(202);
  CHECK(tracker.lastReason == 202);
  CHECK(tracker.attempts == 3);
  CHECK(tracker.disconnects == 3);
}

TEST_CASE(attemptsWithoutDisconnectsIsAValidState) {
  // Mid-attempt, nothing has failed yet. The portal shows "попыток 1, отказов
  // 0" for up to ten seconds, and that must not be read as a reason of zero.
  StaDiagnostic::Tracker tracker;
  tracker.noteAttempt();
  CHECK(tracker.attempts == 1);
  CHECK(tracker.disconnects == 0);
  CHECK(!tracker.hasReason);
  CHECK(tracker.lastReason == -1);
}

}  // namespace

int main() {
  std::printf("%d station diagnostic tests passed\n", testsRun);
  return 0;
}
