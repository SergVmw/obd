#!/usr/bin/env python3
"""Build and verify the isolated RPM/fuel diagnostic app image."""
from pathlib import Path
import hashlib
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ENV = "esp32s3_n16r8_diagnostic"
VERSION = "0.4.20-diag.1"
TARGET = "esp32s3-n16r8-diag"
BUILD_IMAGE = ROOT / f".pio/build/{ENV}/firmware.bin"
OUT_DIR = ROOT / "artifacts/diagnostic"
ARTIFACT = OUT_DIR / f"h2-gauge-v{VERSION}-{TARGET}.bin"


def locate_platformio() -> str:
    # Presence is not enough: a distribution package can put an older
    # PlatformIO on PATH that cannot even start under a newer Python. Probing
    # each candidate keeps a working install reachable instead of failing on
    # whichever broken one happens to be first on PATH.
    candidates = (
        shutil.which("pio"),
        shutil.which("platformio"),
        str(Path.home() / ".local/bin/pio"),
        str(Path.home() / ".platformio/penv/bin/pio"),
        str(Path.home() / ".platformio/penv/bin/platformio"),
    )
    broken: list[str] = []
    for candidate in candidates:
        if not candidate or not Path(candidate).is_file():
            continue
        probe = subprocess.run(
            [candidate, "--version"], capture_output=True, timeout=120
        )
        if probe.returncode == 0:
            return candidate
        broken.append(candidate)
    detail = (
        f" Found but unusable: {', '.join(broken)}." if broken else ""
    )
    raise SystemExit(
        "No working PlatformIO CLI found."
        + detail
        + " Install requirements-dev.txt, or put a working 'pio' on PATH"
        " (a system package built for another Python version is the usual"
        " cause and cannot be used)."
    )


subprocess.run([sys.executable, str(ROOT / "tools/embed_diagnostic_web.py")],
               cwd=ROOT, check=True)
subprocess.run([locate_platformio(), "run", "-e", ENV], cwd=ROOT, check=True)
if not BUILD_IMAGE.is_file():
    raise SystemExit(f"missing diagnostic build output: {BUILD_IMAGE}")

subprocess.run([
    sys.executable, str(ROOT / "tools/check_firmware_manifest.py"),
    str(BUILD_IMAGE), "--expected-version", VERSION,
    "--expected-target", TARGET,
], cwd=ROOT, check=True)

OUT_DIR.mkdir(parents=True, exist_ok=True)
shutil.copyfile(BUILD_IMAGE, ARTIFACT)
digest = hashlib.sha256(ARTIFACT.read_bytes()).hexdigest()
(OUT_DIR / "SHA256SUMS.txt").write_text(
    f"{digest}  {ARTIFACT.name}\n", encoding="ascii")


def describe_tree() -> str:
    """Name the tree this image was built from.

    VERSION is a constant in platformio.ini. It does not move when the code
    does, so two different trees produce two different binaries that both report
    the same version string. That is not hypothetical: an image built from the
    0.4.19 sources carried the label 0.4.20-diag.1 while missing every PID that
    0.4.20 added. A dump is only comparable against another dump if you know
    which build wrote it.
    """
    def git(*args: str) -> str:
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                              text=True).stdout.strip()

    full = git("rev-parse", "HEAD")
    if not full:
        return "commit: unknown, not a git checkout"
    line = f"commit: {git('rev-parse', '--short', 'HEAD')} ({full})"
    changes = git("status", "--porcelain").splitlines()
    if changes:
        line += (f"\nuncommitted changes at build time: {len(changes)}"
                 "\nthe image does not correspond to the commit above")
    return line


(OUT_DIR / "BUILD-COMMIT.txt").write_text(
    f"{describe_tree()}\nimage: {ARTIFACT.name}\nsha256: {digest}\n",
    encoding="utf-8")
(OUT_DIR / "README.md").write_text(f"""# Диагностическая тестовая прошивка H2 Gauge

- Версия: `{VERSION}`
- Target: `{TARGET}`
- App-образ: `{ARTIFACT.name}` ({ARTIFACT.stat().st_size} bytes)
- SHA-256: `{digest}`

Это отдельная тестовая сборка, **не production-релиз**. Файлы текущего релиза
не изменяются. Для OTA выбирайте только этот app `.bin`; это не factory/merged
образ и не содержит bootloader или таблицу разделов. Повторная сборка:
`python tools/build_diagnostic_firmware.py`.

## Что собирает

- независимые счётчики запросов, совпавших ответов, тайм-аутов, серии тайм-аутов,
  задержки ответа и неподдержки для PID `0x0B`, `0x0C`, `0x10`, `0x33`, `0x5E`;
- 90-секундный кольцевой буфер с шагом 500 мс и до 24 снимков событий;
- отметки устаревания RPM, длительной невалидности расчёта топлива, разрыва OBD,
  серий PID-тайм-аутов, поздних ответов и альтернативных ECU;
- кнопку «Скачать диагностику JSON» на главном экране сервисного портала.

История хранится только в RAM: после обычной поездки остановите автомобиль,
откройте сервисный портал и скачайте JSON до перезагрузки. В сервисном режиме
сбор временной истории заморожен, чтобы не записывать интегратор расхода,
который там не обновляется. Сохранение не пишет во flash и не увеличивает её износ.

Используется существующий функциональный опрос Mode 01 и существующая адресация;
диагностическая сборка не добавляет CAN/TWAI self-test и не меняет проводку,
битрейт или поведение production-сборки. Тайм-ауты MAP сами по себе не являются
выводом о причине устаревания RPM или пропадания расхода.
""", encoding="utf-8")

# This guard was pinned to a literal 0.4.3 path wrapped in is_file(), so once
# that release left releases/ it stopped running without saying so: it compared
# against nothing and still read as a passing check. Derive the current release
# instead, and when it is absent, say that the comparison did not happen rather
# than letting silence stand in for a result.
production_text = (ROOT / "include/version.h").read_text(encoding="utf-8")
production_version = re.search(r'H2G_FW_VERSION\s+"([^"]+)"', production_text).group(1)
production_target = re.search(r'H2G_BUILD_TARGET\s+"([^"]+)"', production_text).group(1)
production = ROOT / f"releases/h2-gauge-v{production_version}-{production_target}.bin"
compared = False
if production.is_file():
    compared = True
    if ARTIFACT.read_bytes() == production.read_bytes():
        raise SystemExit(
            f"diagnostic image is byte-identical to production {production_version}"
        )
else:
    print(
        f"note: production {production_version} is not in releases/, "
        "so the diagnostic image was not compared against it"
    )

print(f"Diagnostic app: {ARTIFACT.relative_to(ROOT)}")
print(f"SHA-256: {digest}")
print(f"Size: {ARTIFACT.stat().st_size} bytes")
print(f"Compared against production {production_version}: {'yes' if compared else 'no'}")
print("Build tree: see artifacts/diagnostic/BUILD-COMMIT.txt - the version string "
      "above does not change when the code does, so it cannot identify a build.")
