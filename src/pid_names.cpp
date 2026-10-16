#include "pid_names.h"

namespace PidNames {
namespace {

// Indexed by PID. Only 0x01..0x60 are reachable: the three mask groups cover
// exactly that span. nullptr means "not a named J1979 parameter".
//
// These names are transcribed from the SAE J1979 Service 01 table. They were
// wrong across roughly 0x1D..0x4D before, where entries described oxygen sensor
// heaters and OBD monitor blocks that live at different PIDs entirely, and the
// two first entries were off by a PID in the direction that matters most: 0x04
// read "Measured air flow (MAF)" and 0x03 read "Calculated engine load", while
// the standard puts fuel system status at 0x03 and engine load at 0x04. A
// reader who trusted the label drew the wrong conclusion about the vehicle.
// tools/check_pid_names.py pins every entry against the standard so the table
// cannot drift again without a decision.
const char* const kNames[0x61] = {
    /* 0x00 */ nullptr,
    /* 0x01 */ "Monitor status since DTCs cleared",
    /* 0x02 */ "DTC that caused freeze frame",
    /* 0x03 */ "Fuel system status",
    /* 0x04 */ "Calculated engine load",
    /* 0x05 */ "Engine coolant temperature",
    /* 0x06 */ "Short term fuel trim, bank 1",
    /* 0x07 */ "Long term fuel trim, bank 1",
    /* 0x08 */ "Short term fuel trim, bank 2",
    /* 0x09 */ "Long term fuel trim, bank 2",
    /* 0x0A */ "Fuel pressure",
    /* 0x0B */ "Intake manifold pressure",
    /* 0x0C */ "Engine speed",
    /* 0x0D */ "Vehicle speed",
    /* 0x0E */ "Timing advance",
    /* 0x0F */ "Intake air temperature",
    /* 0x10 */ "MAF air flow rate",
    /* 0x11 */ "Throttle position",
    /* 0x12 */ "Commanded secondary air status",
    /* 0x13 */ "Oxygen sensors present, 2 banks",
    /* 0x14 */ "O2 sensor 1 voltage and trim",
    /* 0x15 */ "O2 sensor 2 voltage and trim",
    /* 0x16 */ "O2 sensor 3 voltage and trim",
    /* 0x17 */ "O2 sensor 4 voltage and trim",
    /* 0x18 */ "O2 sensor 5 voltage and trim",
    /* 0x19 */ "O2 sensor 6 voltage and trim",
    /* 0x1A */ "O2 sensor 7 voltage and trim",
    /* 0x1B */ "O2 sensor 8 voltage and trim",
    /* 0x1C */ "OBD standards this vehicle conforms to",
    /* 0x1D */ "Oxygen sensors present, 4 banks",
    /* 0x1E */ "Auxiliary input status",
    /* 0x1F */ "Run time since engine start",
    /* 0x20 */ "PIDs supported [$21-$40]",
    /* 0x21 */ "Distance traveled with MIL on",
    /* 0x22 */ "Fuel rail pressure, relative to manifold vacuum",
    /* 0x23 */ "Fuel rail gauge pressure",
    /* 0x24 */ "O2 sensor 1 equivalence ratio and voltage",
    /* 0x25 */ "O2 sensor 2 equivalence ratio and voltage",
    /* 0x26 */ "O2 sensor 3 equivalence ratio and voltage",
    /* 0x27 */ "O2 sensor 4 equivalence ratio and voltage",
    /* 0x28 */ "O2 sensor 5 equivalence ratio and voltage",
    /* 0x29 */ "O2 sensor 6 equivalence ratio and voltage",
    /* 0x2A */ "O2 sensor 7 equivalence ratio and voltage",
    /* 0x2B */ "O2 sensor 8 equivalence ratio and voltage",
    /* 0x2C */ "Commanded EGR",
    /* 0x2D */ "EGR error",
    /* 0x2E */ "Commanded evaporative purge",
    /* 0x2F */ "Fuel tank level input",
    /* 0x30 */ "Warm-ups since codes cleared",
    /* 0x31 */ "Distance traveled since codes cleared",
    /* 0x32 */ "Evaporative system vapor pressure",
    /* 0x33 */ "Absolute barometric pressure",
    /* 0x34 */ "O2 sensor 1 equivalence ratio, current",
    /* 0x35 */ "O2 sensor 2 equivalence ratio, current",
    /* 0x36 */ "O2 sensor 3 equivalence ratio, current",
    /* 0x37 */ "O2 sensor 4 equivalence ratio, current",
    /* 0x38 */ "O2 sensor 5 equivalence ratio, current",
    /* 0x39 */ "O2 sensor 6 equivalence ratio, current",
    /* 0x3A */ "O2 sensor 7 equivalence ratio, current",
    /* 0x3B */ "O2 sensor 8 equivalence ratio, current",
    /* 0x3C */ "Catalyst temperature, bank 1 sensor 1",
    /* 0x3D */ "Catalyst temperature, bank 2 sensor 1",
    /* 0x3E */ "Catalyst temperature, bank 1 sensor 2",
    /* 0x3F */ "Catalyst temperature, bank 2 sensor 2",
    /* 0x40 */ "PIDs supported [$41-$60]",
    /* 0x41 */ "Monitor status this drive cycle",
    /* 0x42 */ "Control module voltage",
    /* 0x43 */ "Absolute load value",
    /* 0x44 */ "Commanded air-fuel equivalence ratio",
    /* 0x45 */ "Relative throttle position",
    /* 0x46 */ "Ambient air temperature",
    /* 0x47 */ "Absolute throttle position",
    /* 0x48 */ "Absolute throttle position, alternate",
    /* 0x49 */ "Accelerator pedal position",
    /* 0x4A */ "Accelerator pedal position, alternate",
    /* 0x4B */ "Accelerator pedal position, alternate 2",
    /* 0x4C */ "Commanded throttle actuator",
    /* 0x4D */ "Time run with MIL on",
    /* 0x4E */ "Time since trouble codes cleared",
    /* 0x4F */ "Maximum value for lambda, O2 voltage, O2 current and MAP",
    /* 0x50 */ "Maximum air flow rate from MAF",
    /* 0x51 */ "Fuel type",
    /* 0x52 */ "Ethanol fuel percent",
    /* 0x53 */ "Absolute evaporative system vapor pressure",
    /* 0x54 */ "Evaporative system vapor pressure",
    /* 0x55 */ "Short term secondary O2 trim, bank 1 and bank 3",
    /* 0x56 */ "Long term secondary O2 trim, bank 1 and bank 3",
    /* 0x57 */ "Short term secondary O2 trim, bank 2 and bank 4",
    /* 0x58 */ "Long term secondary O2 trim, bank 2 and bank 4",
    /* 0x59 */ "Fuel rail absolute pressure",
    /* 0x5A */ "Relative accelerator pedal position",
    /* 0x5B */ "Hybrid battery pack remaining life",
    /* 0x5C */ "Engine oil temperature",
    /* 0x5D */ "Fuel injection timing",
    /* 0x5E */ "Engine fuel rate",
    /* 0x5F */ "Emission requirements this vehicle is designed to",
    /* 0x60 */ "PIDs supported [$61-$80]",
};

}  // namespace

const char* name(uint8_t pid) {
  if (pid == 0x00 || pid > 0x60) return "Not a J1979 parameter";
  return kNames[pid] ? kNames[pid] : "Vendor specific, name unknown";
}

}  // namespace PidNames
