#!/usr/bin/env python3
"""Check that the trace's declared column names match the values written.

The trace publishes its own schema: signalsOrder, signalAgesOrder, fuelOrder,
obdOrder and pidColumns say what each column is, and there is no version number
to fall back on. So a column added to the writer without a name is a column
nobody can read, and a name added without a value is a column that is always
absent. Neither raises an error anywhere: the JSON stays valid, a reader that
resolves names finds what it expects, and the new value silently lands under
the previous column's name.

Two shapes of writer exist and both are checked. The signals and ages groups
are C arrays iterated in a loop; the header fields, the fuel group and the obd
group are sequential appendTrace calls separated by commas. A loop is counted
by its initialiser, a sequential run by its number of appends, and a row inside
the per-PID loop by its appends between the row brackets.

The group boundaries come from the writer's own markers rather than from a
list maintained here, so the check follows the file instead of drifting from
it. The gate fails loudly when it cannot find what it looks for: matching
nothing would leave it passing on a file it never read, which is the failure
mode that lets a check outlive the thing it checked.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
source = (ROOT / "src/service_portal.cpp").read_text(encoding="utf-8")

start = source.index("void appendTraceSample(String& output, const ObdTraceSample& sample) {")
end = source.index('output += ",\\"samples\\":["', start)
body = source[start:end]

APPEND = re.compile(r"\bappendTrace(?:UInt|Float|Bool|Age)\s*\(")

# The first group opens with ",[" and every group after it opens with "],[",
# which also closes the one before. Those markers are the group boundaries.
opens = [m.start() for m in re.finditer(r'output \+= "\]?,\[";', body)]
if len(opens) != 5:
    sys.exit(f"  ожидалось 5 границ групп, найдено {len(opens)}: разметка изменилась")
b_header = 0
b_signals, b_ages, b_fuel, b_obd, b_pids = opens


def names_of(keyword):
    m = re.search(r'\\"%s\\":\[(.*?)\]' % keyword, source)
    if not m:
        sys.exit(f"  не найден {keyword}: разметка трассы изменилась, гейт надо переписать")
    found = re.findall(r'\\"([^"\\]+)\\"', m.group(1))
    if not found:
        sys.exit(f"  {keyword} пуст или не разобран")
    return found


def array_fields(name, lo, hi):
    m = re.search(r"\b%s\[\]\s*=\s*\{(.*?)\};" % re.escape(name), body[lo:hi], re.S)
    if not m:
        sys.exit(f"  не найден инициализатор {name}[] в своём диапазоне")
    return [e.strip().replace("sample.", "") for e in m.group(1).split(",") if e.strip()]


def array_len(name, lo, hi):
    return len(array_fields(name, lo, hi))


problems = []
n_header = len(APPEND.findall(body[b_header:b_signals]))
n_signal = array_len("signals", b_signals, b_ages)
n_age = array_len("ages", b_ages, b_fuel)
n_fuel = len(APPEND.findall(body[b_fuel:b_obd]))
n_obd = len(APPEND.findall(body[b_obd:b_pids]))

rows = re.findall(r"output \+= '\[';(.*?)\n\s*\}\n", body[b_pids:], re.S)
if not rows:
    sys.exit("  не найден цикл записи группы pids")
per_row = len(APPEND.findall(rows[0]))

k_re = re.search(r"static constexpr size_t kPidCount = (\d+);",
                 (ROOT / "include/obd_trace.h").read_text(encoding="utf-8"))
if not k_re:
    sys.exit("  не найден kPidCount в obd_trace.h")
k = int(k_re.group(1))

d_signal = names_of("signalsOrder")
d_age = names_of("signalAgesOrder")
d_fuel = names_of("fuelOrder")
d_obd = names_of("obdOrder")
d_pid = names_of("pidColumns")
d_columns = names_of("sampleColumns")

for label, written, declared in (
        ("signals", n_signal, len(d_signal)),
        ("signalAges", n_age, len(d_age)),
        ("fuel", n_fuel, len(d_fuel)),
        ("obd", n_obd, len(d_obd)),
        ("pids", per_row, len(d_pid))):
    mark = "ok " if written == declared else "СБОЙ"
    print(f"  {mark} {label:12s} {written:3d} значений / {declared:3d} имён")
    if written != declared:
        problems.append(f"{label}: записано {written}, объявлено {declared}")

# sampleColumns names the leading scalars and then one entry per group
n_named = n_header + 5
mark = "ok " if len(d_columns) == n_named else "СБОЙ"
print(f"  {mark} {'sampleColumns':12s} {len(d_columns):3d} имён / "
      f"{n_header} одиночных + 5 групп")
if len(d_columns) != n_named:
    problems.append(f"sampleColumns: {len(d_columns)} имён против "
                    f"{n_header} одиночных полей и 5 групп")

# In the signals group the published name is the field name, spelled the same
# way. That makes a positional check possible there and nowhere else: the other
# groups deliberately publish short names (responses for obdResponseCount, rpm
# for rpmAgeMs), so a table of those would be a second place to forget an
# update. Checking signals exactly catches the swap a length check cannot see,
# where two names trade places and every column silently reports the wrong one.
sig_fields = array_fields("signals", b_signals, b_ages)
mismatch = [(i, f, d_signal[i] if i < len(d_signal) else "<нет имени>")
            for i, f in enumerate(sig_fields)
            if i >= len(d_signal) or f != d_signal[i]]
if mismatch:
    problems.append("signals: имя и поле разошлись по позициям: " + "; ".join(
        f"позиция {i} пишет {f} под именем {d}" for i, f, d in mismatch))
    for i, f, d in mismatch:
        print(f"  СБОЙ signals       позиция {i}: пишется {f}, объявлено {d}")
else:
    print(f"  ok  {'signals':12s} имена совпадают с полями по позициям")

# A length check cannot see two names trading places: every column still exists
# and still holds a number, and each one just reports the wrong reading. In the
# signals group that is caught above, because there the published name is the
# field name. The obd group deliberately publishes short names instead, so a table
# of all of them would be a second place to forget an update - but where a
# declared name IS spelled exactly as the field written under it, the two must
# land on the same index, and that can be demanded without a table.
# The obd group is the fourth run of separators, between the fifth-from-last
# marker and the last one; the gate above unpacks the same five.
_b_obd = opens[3]
# One entry per append, in order, so the indices line up with the declared names.
# An append that does not name a field - connected is a bit test, not a member -
# is kept as None so it still occupies its column rather than shifting the rest.
obd_fields = []
for m in re.finditer(
        r"appendTrace(?:UInt|Float|Bool|Age)\s*\(\s*output\s*,(.*?)\);",
        body[_b_obd:opens[4]], re.S):
    field = re.search(r"sample\.([A-Za-z0-9_]+)", m.group(1))
    obd_fields.append(field.group(1) if field else None)
# The obd group has always published short names for the counters, and that is
# fine, but it makes "not a field name" ambiguous: a column renamed to something
# no field answers to would otherwise pass as a legitimate short name, and a
# reader resolving names would simply never find it. So the short names are
# listed here. Anything new in this group has to be spelled as its field, which
# costs a reader nothing and is what makes the positional check above cover every
# column rather than the four identifiers.
_obd_short_names = {
    "connected", "responses", "timeouts", "canErrors", "busErrors",
    "tec", "rec", "rxMissed", "alternateEcuResponses",
}
_written = [f for f in obd_fields if f]
for index, name in enumerate(d_obd):
    if name not in _written:
        if name not in _obd_short_names:
            problems.append(f"obd: {name} на позиции {index} не имя поля и "
                            f"не заявленное короткое имя")
            print(f"  СБОЙ obd            {name} на позиции {index}: не имя "
                  f"поля и не короткое имя из списка")
        continue
    if index >= len(obd_fields):
        problems.append(f"obd: {name} объявлен на позиции {index}, "
                        f"но записано всего {len(obd_fields)} значений")
        print(f"  СБОЙ obd            {name} на позиции {index} некуда писать")
    elif obd_fields[index] != name:
        problems.append(f"obd: позиция {index} пишет {obd_fields[index]} "
                        f"под именем {name}")
        print(f"  СБОЙ obd            позиция {index}: пишется "
              f"{obd_fields[index]}, объявлено {name}")
n_exact = sum(1 for i, n in enumerate(d_obd)
              if n in [f for f in obd_fields if f]
              and i < len(obd_fields) and obd_fields[i] == n)
if n_exact:
    print(f"  ok  {'obd':12s} {n_exact} имён совпадают с полями по позициям")

if problems:
    for p in problems:
        print("  " + p)
    sys.exit(1)
print("  схема трассы согласована: у каждой объявленной колонки есть значение,\n"
      "  и у каждого записанного значения есть имя")