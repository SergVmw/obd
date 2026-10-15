#!/usr/bin/env python3
"""One configuration value, one range, everywhere.

The portal clamps a field, the firmware refuses to store it, and the form
declares bounds of its own. Those three were maintained separately, so a range
could be widened in one place and left behind in another. The symptom was not a
rejected value at the form: it was an HTTP 500 reading "NVS save failed" for a
request the flash never refused, on a value the documentation told the owner to
set.

These checks assert agreements between sites rather than any particular
number, so raising a ceiling does not mean hunting for every copy of it.
"""

import re
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CONFIG = (ROOT / "src/app_config.cpp").read_text(encoding="utf-8")
PORTAL = (ROOT / "src/service_portal.cpp").read_text(encoding="utf-8")
HTML = (ROOT / "web/index.html").read_text(encoding="utf-8")

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


# The range the portal enforces when accepting a settings form.
portal_ranges = {
    field: (int(low), int(high))
    for field, low, high in re.findall(r'\{"(\w+)",\s*(-?\d+),\s*(-?\d+)\}', PORTAL)
}
check(len(portal_ranges) > 0, "no portal range table found; the parser is stale")

# The range the firmware enforces when persisting. A field can legitimately be
# checked by only one side, so this collects the ones that are bounded on both
# ends rather than demanding that valid() restate the whole table. Both bounds
# are written as "config.<field>", so the prefix has to be allowed between them.
persisted_bounds = {}
for field, low, high in re.findall(
    r"\b(\w+)\s*>=\s*(-?\d+)\s*&&\s*(?:\w+\.)*\b\1\s*<=\s*(-?\d+)", CONFIG
):
    persisted_bounds.setdefault(field, (int(low), int(high)))

# Numeric factory defaults, as written in defaults().
factory_defaults = {
    field: int(value)
    for field, value in re.findall(r"^\s*c\.(\w+)\s*=\s*(-?\d+)\s*;", CONFIG, re.M)
}

# The form's own declaration for each input that carries an id.
form_fields = {}
for tag in re.findall(r"<input\b[^>]*>", HTML):
    identifier = re.search(r'id="(\w+)"', tag)
    if identifier:
        form_fields[identifier.group(1)] = {
            attribute: (int(value) if value.lstrip("-").isdigit() else value)
            for attribute, value in re.findall(r'(\w+)="([^"]*)"', tag)
        }

# The "factory values" button of the settings form.
default_config = re.search(r"const DEFAULT_CONFIG=(\{.*?\});", HTML, re.S)
check(default_config is not None, "DEFAULT_CONFIG preset not found in the form")
web_defaults = {}
if default_config:
    section = re.search(r"obd:\{([^}]*)\}", default_config.group(1))
    if section:
        web_defaults = {field: int(value) for field, value in
                        re.findall(r"(\w+):(-?\d+)", section.group(1))}

# 1. The form must not offer a value the portal would reject. It is allowed to
#    be stricter than the portal: the brightness calibration fields carry a
#    second rule of their own, enforced on the firmware side, and narrowing the
#    form is how that rule reaches the user before a round trip.
for field, (low, high) in sorted(portal_ranges.items()):
    attributes = form_fields.get(field)
    if attributes is None:
        continue  # Not a form input; the portal range may cover an API-only field.
    form_low = attributes.get("min")
    form_high = attributes.get("max")
    check(
        form_low is None or form_low >= low,
        f"{field}: the form allows {form_low}, below the {low} the portal "
        f"accepts; a value could be offered that save() then refuses",
    )
    check(
        form_high is None or form_high <= high,
        f"{field}: the form allows {form_high}, above the {high} the portal "
        f"accepts; a value could be offered that save() then refuses",
    )

# 2. Where the firmware bounds a field on both ends, those bounds must be the
#    portal's. This is the check that the 40-versus-60 ceiling failed.
for field, (low, high) in sorted(portal_ranges.items()):
    stored = persisted_bounds.get(field)
    check(
        stored is None or stored == (low, high),
        f"{field}: firmware refuses to store outside {stored} while the portal "
        f"accepts {low}..{high}; save() would answer 500 'NVS save failed' on "
        f"a value the form offers",
    )

# 3. A factory default must sit inside the range that would have to accept it,
#    and must agree with both places that display it.
for field, default in sorted(factory_defaults.items()):
    bounds = portal_ranges.get(field)
    if bounds is not None:
        check(
            bounds[0] <= default <= bounds[1],
            f"{field}: factory default {default} lies outside the accepted "
            f"range {bounds[0]}..{bounds[1]}",
        )
    attributes = form_fields.get(field)
    if attributes is not None and "value" in attributes:
        check(
            attributes["value"] == default,
            f"{field}: the form offers {attributes['value']} but the firmware "
            f"defaults to {default}; the two presets would disagree",
        )
    if field in web_defaults:
        check(
            web_defaults[field] == default,
            f"{field}: the form's factory preset says {web_defaults[field]} but "
            f"the firmware defaults to {default}; 'factory values' would write "
            f"something the firmware never produces",
        )

if failures:
    print("configuration ranges disagree:", file=sys.stderr)
    for failure in failures:
        print(f"  - {failure}", file=sys.stderr)
    sys.exit(1)
print(
    f"Configuration ranges agree: {len(portal_ranges)} bounded fields, "
    f"{len(factory_defaults)} factory defaults, "
    f"{len(persisted_bounds)} bounds restated by the firmware"
)
