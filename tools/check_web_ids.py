#!/usr/bin/env python3
"""Check that every id the inline script reaches for exists in the markup.

This is the failure neither the firmware nor a browser test reliably sees, and
the dangerous half of it is quiet. The page's accessors are `$(id)`, `n(id)`,
`v(id)`, `c(id)` and `set(id, value)`:

    set(id, value) { const e = $(id); if (!e || value === undefined) return; ... }

A missing id makes `set()` return without doing anything. The input is never
filled from the GET, so it keeps whatever the markup says, and if Save is then
enabled the page posts the device's defaults over the owner's settings. That is
the 0.4.16 overwrite, reached by renaming one input instead of through a failed
load. `n()`, `v()` and `c()` are the loud ones: `Number(null.value)` throws a
TypeError, so a missing id on the save path breaks the page visibly.

The accessors are found by their own definition, so renaming them does not
silently turn this gate into a no-op: the script fails loudly if it cannot find
them, rather than matching nothing and passing.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
html = (ROOT / "web/index.html").read_text(encoding="utf-8")

script_start = html.index("<script>")
markup = html[:script_start]
script = html[script_start:]

declared = set(re.findall(r'\bid="([^"]+)"', markup))

# Every `<name>=id=>` arrow function, plus the declared `function set(`.
names = set(re.findall(r"([A-Za-z_$][\w$]*)=id=>", script))
if "function set(" not in script:
    sys.exit("  не найдена объявленная функция set(id,value) — разметка скрипта изменилась")
names.add("set")
if len(names) < 5:
    sys.exit(f"  найдено подозрительно мало помощников доступа: {sorted(names)}")
print(f"  помощники доступа по id: {sorted(names)}")

# Only string-literal arguments count. config() and apply() spell every id out
# as a literal, so nothing is missed, and no object key can be mistaken for one.
used = set()
for name in sorted(names):
    pattern = r"(?<![A-Za-z0-9_$])%s\(\s*'([^']+)'\s*\)" % re.escape(name)
    used |= set(re.findall(pattern, script))
    used |= set(re.findall(r'(?<![A-Za-z0-9_$])%s\(\s*"([^"]+)"\s*\)'
                           % re.escape(name), script))

missing = sorted(used - declared)
unused = sorted(declared - used)

print(f"  объявлено id в разметке: {len(declared)}")
print(f"  достижимо из скрипта:    {len(used)}")

# Only this direction fails the gate. An id in the markup the script never
# touches is usually a container or a tab pane whose children are used instead
# (slotApp0Card holds slotApp0Version and slotApp0Meta), or an anchor for CSS.
# Treating those as errors would mean maintaining a list of legitimate ones,
# and a gate that needs such a list is a gate that gets switched off.
if unused:
    print(f"  в разметке не читается скриптом ({len(unused)}, "
          f"ожидаемо для контейнеров секций):")
    for name in unused:
        print(f"    {name}")

assert not missing, (
    "скрипт обращается к id, которых нет в разметке — это станет NaN в POST "
    f"или ошибкой при загрузке: {missing}")
print("  каждый id, к которому обращается скрипт, существует в разметке")