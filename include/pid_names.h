#pragma once

#include <stdint.h>

// Names for the Mode 01 parameters a support mask can encode.
//
// A mask is four bytes per group, one bit per parameter, most significant bit
// first per SAE J1979: group 0x00 covers PID 0x01..0x20, 0x20 covers 0x21..0x40,
// and 0x40 covers 0x41..0x60. Shipping four hex bytes to a reader would leave
// the work undone; the useful form is the list of numbers, and the useful form
// of that is with names.
//
// Names for parameters the firmware already polls deliberately match the
// comments in the poll table, so a listing and the code that produces the value
// never call the same thing different things. Entries outside J1979 are named
// only where the meaning is not a guess; everything else comes back as "Vendor
// specific" rather than an invented description.
namespace PidNames {

// Returns a static string; never null. Empty for PID 0x00, which is a request
// rather than a parameter.
const char* name(uint8_t pid);

}  // namespace PidNames
