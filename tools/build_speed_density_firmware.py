#!/usr/bin/env python3
"""Build and verify the isolated Haval Speed-Density experimental image."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ENV = "esp32s3_n16r8_speeddensity"
VERSION = "0.4.3-sd.1"
TARGET = "esp32s3-n16r8-sd"
BUILD_IMAGE = ROOT / f".pio/build/{ENV}/firmware.bin"
OUT_DIR = ROOT / "artifacts/speed-density"
ARTIFACT = OUT_DIR / f"h2-gauge-v{VERSION}-{TARGET}.bin"
PROTECTED = (
    ROOT / "releases/h2-gauge-v0.4.3-esp32s3-n16r8.bin",
    ROOT / "releases/h2-gauge-v0.4.3-esp32s3-n16r8-factory.bin",
    ROOT / "artifacts/diagnostic/h2-gauge-v0.4.3-diag.1-esp32s3-n16r8-diag.bin",
)


def locate_platformio() -> str:
    # Presence is not enough: a distribution package can put an older
    # PlatformIO on PATH that cannot even start under a newer Python. Probing
    # each candidate keeps a working install reachable instead of failing on
    # whichever broken one happens to be first on PATH.
    candidates = (
        shutil.which("pio"),
        shutil.which("platformio"),
        str(ROOT / ".venv/bin/pio"),
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


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


protected_before = {path: sha256(path) for path in PROTECTED if path.is_file()}
subprocess.run([sys.executable, str(ROOT / "tools/embed_web.py")],
               cwd=ROOT, check=True)
subprocess.run([sys.executable, str(ROOT / "tools/check_speed_density_integration.py")],
               cwd=ROOT, check=True)
subprocess.run([sys.executable, str(ROOT / "tools/test_telemetry.py")],
               cwd=ROOT, check=True)
subprocess.run([locate_platformio(), "run", "-e", ENV], cwd=ROOT, check=True)
if not BUILD_IMAGE.is_file():
    raise SystemExit(f"missing Speed-Density build output: {BUILD_IMAGE}")

subprocess.run([
    sys.executable, str(ROOT / "tools/check_firmware_manifest.py"),
    str(BUILD_IMAGE), "--expected-version", VERSION,
    "--expected-target", TARGET,
], cwd=ROOT, check=True)

OUT_DIR.mkdir(parents=True, exist_ok=True)
shutil.copyfile(BUILD_IMAGE, ARTIFACT)
digest = sha256(ARTIFACT)
(OUT_DIR / "SHA256SUMS.txt").write_text(
    f"{digest}  {ARTIFACT.name}\n", encoding="ascii")
(OUT_DIR / "README.md").write_text(f"""# Haval H2 Speed-Density — экспериментальная прошивка

- Версия: `{VERSION}`
- Target: `{TARGET}`
- OTA app-образ: `{ARTIFACT.name}` ({ARTIFACT.stat().st_size} bytes)
- SHA-256: `{digest}`
- Статус: сборка и manifest проверены; проверка на автомобиле не выполнялась.

Это отдельная тестовая сборка, не production-релиз. Она не перезаписывает
релиз 0.4.3 или предыдущую `diag.1`. Для OTA выбирайте только указанный app
`.bin`; он не содержит bootloader или таблицу разделов. Пересборка:
`python tools/build_speed_density_firmware.py`.

## Что меняется

- Для расчёта расхода используется Speed-Density по `PID 0x0B` MAP, `PID 0x0C`
  RPM и `PID 0x0F` IAT; температура декодируется как `A - 40 °C`.
- Для бензина и LPG применяется общий расчёт воздушного потока, затем AFR,
  плотность и существующая пользовательская поправка выбранного топлива.
- Начальные параметры: рабочий объём `1.497 л`, VE `0.85`. VE постоянная,
  поэтому это оценка, особенно под наддувом; индивидуальная точность не
  подтверждена и может потребовать калибровки коэффициентом топлива.
- В этом образе запросы `PID 0x10` MAF и `PID 0x5E` Fuel Rate исключены из
  таблицы опроса и дополнительно заблокированы в отправителе. `PID 0x0F`
  запрашивается примерно раз в секунду.
- После обнаружения engine ECU по RPM последующие запросы Mode 01 направляются
  ему адресно: ответ ECU `0x7E8` соответствует request ID `0x7E0`. Первичный
  поиск ECU остаётся функциональным запросом `0x7DF`.
- Если RPM/MAP старше 2 с, IAT старше 5 с, двигатель остановлен или PID 0F не
  поддерживается, расчёт помечается недействительным и экран показывает
  прочерк, а не фиктивный ноль.
- При скорости ниже порога переключения (по умолчанию 5 км/ч) отображаются
  литры в час. На стоящем авто с работающим двигателем должен быть показан
  оценочный `л/ч`, когда все три входа свежие.
- Сервисный статус показывает поддержку/счётчики PID 0F, температуру IAT,
  оценённый поток воздуха и свежесть MAP/RPM/IAT.

## Проверка на автомобиле

1. Установить только если автомобиль безопасно припаркован; не запускать
   TWAI self-test и не менять проводку/битрейт.
2. После прошивки дать автомобилю обнаружить ECU, затем проверить на главном
   экране при работающем на холостом ходу двигателе. Ожидаем единицы `Л/Ч` и
   числовое значение, а не `--.-`.
3. В сервисном портале проверить `PID 0F · поддержка`, счётчики запросов/
   ответов, температуру впуска и счётчики MAP. Если данных нет, сохранить
   снимок страницы/JSON статуса до перезагрузки и сообщить, какой из входов
   отсутствует или устаревает.
4. Не считать начальное `VE=0.85` измеренным значением расхода. Сравнивать его
   сначала только как экспериментальную оценку; не использовать для расчётов
   заправки до калибровки.
""", encoding="utf-8")

for path, original_digest in protected_before.items():
    if not path.is_file() or sha256(path) != original_digest:
        raise SystemExit(f"protected release/diag artifact changed unexpectedly: {path}")

print(f"Speed-Density app: {ARTIFACT.relative_to(ROOT)}")
print(f"SHA-256: {digest}")
print(f"Size: {ARTIFACT.stat().st_size} bytes")
