#!/usr/bin/env python3
"""Build and verify the isolated RPM/fuel diagnostic app image."""
from pathlib import Path
import hashlib
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
(OUT_DIR / "README.md").write_text(f"""# Диагностическая тестовая прошивка H2 Gauge

- Версия: `{VERSION}`
- Target: `{TARGET}`
- App-образ: `{ARTIFACT.name}` ({ARTIFACT.stat().st_size} bytes)
- SHA-256: `{digest}`

Это отдельная тестовая сборка, **не production-релиз**. Файлы релиза 0.4.3
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

production = ROOT / "releases/h2-gauge-v0.4.3-esp32s3-n16r8.bin"
if production.is_file() and ARTIFACT.read_bytes() == production.read_bytes():
    raise SystemExit("diagnostic image unexpectedly identical to production 0.4.3")

print(f"Diagnostic app: {ARTIFACT.relative_to(ROOT)}")
print(f"SHA-256: {digest}")
print(f"Size: {ARTIFACT.stat().st_size} bytes")
