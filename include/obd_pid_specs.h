#pragma once

#include <stdint.h>

// Response sizes for the Mode 01 PIDs this project decodes.
//
// SAE J1979 / ISO 15031-5 fixes how many data bytes each parameter carries, and
// the number is a property of the parameter, not of the vehicle reporting it:
// engine speed is always two bytes, manifold pressure always one, control module
// voltage always two. A block that answers with fewer bytes than the parameter
// needs is not answering in a shortened form of the same value - the bytes are
// simply not there, and the decoder would have to invent them.
//
// That is what used to happen. The length check lived inside the branch that
// decides which block is the engine, so it ran while the engine ECU was still
// being identified and stopped running the moment identification settled:
// 0x0C is not one of the four engine-only parameters, so engineIdentityDecides()
// returns false for it as soon as any block holds the identity, which is the
// state the vehicle is in for the whole of every drive. In that state a
// two-byte parameter answered with one byte was decoded with the missing byte
// read as zero, and the request was then closed as successfully answered, so it
// was never repeated. Engine speed came out up to 64 rpm low and the poller
// believed it had been served.
//
// Lengths are data bytes only - A, B, C, D as the standard names them,
// excluding the service echo and the PID echo. An ISO-TP Single Frame adds two
// bytes on top.
//
// Two numbers, not one, because for a single parameter the standard and this
// decoder disagree about what is enough. See requiredDataBytes() below.
//
// Everything here is a single return statement: the firmware is built as C++11,
// where the body of a constexpr function is allowed to be nothing else.
namespace ObdPidSpecs {

// Data bytes J1979 defines for the parameter.
//
// Returns 0 for a PID the table does not cover, which means "no normative
// length is known", not "no data bytes".
constexpr uint8_t normativeDataBytes(uint8_t pid) {
  // Support masks (00, 20, 40) and monitor status (01): four bytes A..D.
  return (pid == 0x00 || pid == 0x01 || pid == 0x20 || pid == 0x40) ? 4
         // Fuel system status (03, A = system 1, B = system 2), engine speed
         // (0C), mass air flow (10), O2 sensor 1 (14), distance since codes
         // cleared (31), control module voltage (42), commanded equivalence
         // ratio (44), engine fuel rate (5E), O2 sensor 2 (15: A voltage,
         // B trim).
         : (pid == 0x03 || pid == 0x0C || pid == 0x10 || pid == 0x14 ||
            pid == 0x15 || pid == 0x31 || pid == 0x42 || pid == 0x44 ||
            pid == 0x5E)
             ? 2
             // Calculated load (04), coolant (05), short term trim (06), long
             // term trim (07), manifold pressure (0B), speed (0D), timing
             // advance (0E), intake air (0F), throttle (11), commanded purge
             // (2E), barometric pressure (33).
             : (pid == 0x04 || pid == 0x05 || pid == 0x06 || pid == 0x07 ||
                pid == 0x0B || pid == 0x0D || pid == 0x0E || pid == 0x0F ||
                pid == 0x11 || pid == 0x2E || pid == 0x33)
                 ? 1
                 : 0;
}

// Data bytes this decoder needs before it can read the parameter.
//
// One exception, and it is the whole reason there are two functions. J1979
// defines PID 03 as two bytes: A for fuel system 1, B for fuel system 2. Byte B
// describes a second fuel system, which a vehicle need not have, and blocks
// without one answer a single byte. This decoder reads A only.
//
// So a one-byte answer to PID 03 is not short - it is the whole parameter this
// vehicle has - and requiring two would not be strictness, it would be a
// blackout with a side effect out of proportion to the bytes: 0x03 is one of the
// four parameters the engine identity is claimed on, so refusing its answers
// can stop the identity from ever being taken, and the engine identity is what
// the DTC scan and the clear are bound to. A rule about response lengths has no
// business deciding which block raises the check-engine lamp.
constexpr uint8_t requiredDataBytes(uint8_t pid) {
  return pid == 0x03 ? 1 : normativeDataBytes(pid);
}

constexpr bool isListed(uint8_t pid) { return normativeDataBytes(pid) != 0; }

// Smallest ISO-TP Single Frame payload this decoder will accept: the service
// echo, the PID echo and the data bytes it needs.
//
// An unlisted PID falls back to the structural floor of three rather than being
// rejected. Failing closed on a PID the table has never heard of would turn
// every future addition to the poll profile into a silent blackout the first
// time a block answered it, and the loss would be invisible: nothing counts an
// answer that was dropped before it was decoded.
constexpr uint8_t minPayloadLength(uint8_t pid) {
  return requiredDataBytes(pid) == 0
             ? 3
             : static_cast<uint8_t>(requiredDataBytes(pid) + 2);
}

// Minimum, not exact. A block that returns more bytes than the parameter needs
// is still carrying the normative ones in the normative positions - a frame
// answering engine speed and vehicle speed together is a longer answer, not a
// corrupt one - so rejecting it would throw away readable data to satisfy a
// rule that exists to catch the opposite defect.
constexpr bool payloadSufficient(uint8_t pid, uint8_t payloadLength) {
  return payloadLength >= minPayloadLength(pid);
}

// An answer longer than the standard defines for the parameter. Accepted and
// decoded, but counted: a block that over-delivers on one parameter may
// over-deliver on another where the extra bytes are not padding behind a
// complete value. This is what decides, from the vehicle rather than from the
// workshop, whether exact equality would be safe here.
constexpr bool isOverLong(uint8_t pid, uint8_t payloadLength) {
  return normativeDataBytes(pid) != 0 &&
         payloadLength > normativeDataBytes(pid) + 2u;
}

}  // namespace ObdPidSpecs
