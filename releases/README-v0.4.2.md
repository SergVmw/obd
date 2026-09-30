# H2 Gauge 0.4.2 — DTC history, reset durability and rollover corrections

**Дата упаковки:** 2026-09-30  
**Цель:** ESP32-S3 DevKitC-1 N16R8 (`esp32s3-n16r8`)  
**Config schema:** 6 (без миграции настроек)  
**Последний аппаратно подтверждённый release:** 0.3.9

## Статус и замена 0.4.1

0.4.2 — отдельный corrective release после дополнительного аудита уже упакованного 0.4.1. Бинарники 0.4.1 не заменялись: для изменённого production-кода выпущена новая версия и новые SHA-256. По правилу проекта «в `releases/` только актуальная версия» прежний комплект удалён из этой папки.

0.4.2 программно проверен и упакован, но Check Engine/DTC ещё требует аппаратной read-only приёмки на Haval H2. До сравнения Mode 03/07/0A с независимым сканером не выполняйте Mode 04. Отозванный 0.4.0 также не устанавливать.

## Артефакты

| Файл | Назначение | Размер | SHA-256 |
|---|---|---:|---|
| `h2-gauge-v0.4.2-esp32s3-n16r8.bin` | **Обычный app image для штатного web OTA** | **1 190 368 байт** | `163ea6714fdca4fdca5c7ab75b0e687650547689cab13ad339e185f0add8dd1b` |
| `h2-gauge-v0.4.2-esp32s3-n16r8-factory.bin` | Полная чистая USB-установка с offset `0x0` | **1 255 904 байта** | `7bd7cdaeb7d1381d61a0ea3010758fa166672a761db19b1b07f1ae9ab90f5d5c` |

Проверка:

```bash
cd releases
sha256sum -c SHA256SUMS-v0.4.2.txt
```

## Как обновлять

Для штатного OTA выбирайте только:

```text
h2-gauge-v0.4.2-esp32s3-n16r8.bin
```

Это обычный app image. Web OTA записывает неактивный OTA-слот, ждёт нативную проверку образа и boot read-back, затем автоматически перезагружает прибор. NVS, настройки, trip, бензиновая калибровка, DTC history, LittleFS journal и пользовательские ресурсы сохраняются.

После reboot физический экран «СЕРВИС» и верх web UI должны показать текущую сборку `0.4.2`, running/boot slot и версию каждого OTA-слота.

Factory image предназначен только для чистой USB-записи с offset `0x0`. Он содержит:

```text
0x00000  bootloader
0x08000  partitions_16mb_ota.csv
0x0E000  boot_app0
0x10000  application 0.4.2
```

Полная запись с erase удаляет NVS, настройки, trip, калибровки, DTC history, LittleFS journal и custom assets. Factory image нельзя загружать через web OTA.

## Исправления относительно 0.4.1

- Усечённый ISO-TP DTC-ответ теперь считается неавторитетным только для своей категории: фактически увиденные коды добавляются в history, но отсутствие кода в отброшенном хвосте больше не снимает его `lastPresentKinds`.
- Полнота определяется по конкретному assembled response (`!assemblyTruncated`), а не по scan-global `state.truncated`; поэтому переполнение общего 32-entry списка не обесценивает отдельный полный ответ другой категории.
- DTC API выставляет `history.currentStateKnown=false` при любом `state.truncated`. Только успешные полные ответы всех трёх категорий дают доказательство текущего состояния; `Unsupported` не приравнивается к `Complete`.
- `RuntimePersistence::factoryReset()` следует контракту checkpoint: после обязательной успешной очистки старых LittleFS/NVS данных достаточно хотя бы одной read-back-подтверждённой новой durable-копии. Проверен сценарий успешного journal и отказа NVS `putBytes` после успешного NVS clear.
- Post-clear deadline больше не использует timestamp `0` как признак отсутствия расписания. Отдельный private-флаг сохраняет обязательный scan, даже если `now + 1500U` после rollover точно равен нулю.
- Post-clear scan остаётся обязательным продолжением ручного Mode 04 и запускается при paused automatic polling.
- Официальный behavioral test Mode 04 теперь явно проходит цепочку `7F 04 78` → финальный `44` без повторной передачи команды.

## Сохранённая safety-модель Mode 04

Стирание никогда не автоматическое. Ручная команда требует:

1. подтверждённый engine ECU;
2. новое physical чтение Mode 03/07/0A;
3. отсутствие timeout/malformed/transport/final-NRC/truncation;
4. durable pre-clear checkpoint;
5. скорость `0 км/ч`;
6. RPM `< 50`;
7. ECU voltage `11,5…16,5 В`;
8. точное web-подтверждение последствий readiness/freeze-frame;
9. обязательное post-clear чтение и checkpoint.

Mode 04 не стирает permanent DTC напрямую. Если post-clear категория не прочитана полностью и успешно, её последнее подтверждённое historical-presence не выдаётся за исчезнувшее.

Engine ECU по-прежнему фиксируется только по первому структурно валидному ответу Mode 01 PID `0C`; алгоритм намеренно не усложнялся без аппаратного подтверждения проблемы на Haval H2. Рефакторинг дублированных persistence-модулей в общий base не входит в corrective release и остаётся отдельной задачей v0.5.0.

## Выполненные software gates

- 15 host-групп OBD/DTC, включая truncated history, NRC `0x78`, Mode 04 pending/final, P2*/absolute deadline, несколько ECU, physical target, paused continuation и exact-zero rollover;
- 13 host-групп OTA diagnostics;
- persistence/storage fault injection: torn/partial writes, corrupt readback, LittleFS-only/NVS-only checkpoint, dual failure и factory reset с отказом NVS rewrite после успешного clear;
- 25 brightness-групп, persistence snapshot/CRC и browser UI regressions;
- static DTC/storage/OTA/font integration gates и проверка embedded web UI;
- clean PlatformIO `esp32s3_n16r8` build: RAM 53 340 байт, application flash 1 189 945 байт;
- H2 manifest: `0.4.2 / esp32s3-n16r8`, app offset `0x1c280`;
- packaged app byte-for-byte совпадает с clean build output;
- повторная сборка из `/tmp/H2-Gauge-ci` дала byte-identical app и тот же SHA-256, подтвердив path prefix maps;
- factory layout, SHA-256 и размер OTA partition проверены;
- app size не выровнен искусственно: финальный raw fragment — 1 360 байт.

## Первая аппаратная приёмка

1. Выполнить штатный app OTA с аппаратно подтверждённой 0.3.9 и проверить automatic reboot, `current/running/boot = 0.4.2`, второй slot и image state `valid`.
2. На стоящем автомобиле сначала только прочитать PID 0C, PID 01 и Mode 03/07/0A; сравнить ECU ID и коды с независимым сканером.
3. Подтвердить отсутствие ложной смены engine ECU при ответах нескольких модулей.
4. Проверить transient history и восстановление после reboot из LittleFS/NVS.
5. Mode 04 проверять отдельно, только после read-only acceptance и с пониманием сброса readiness/freeze-frame.
6. После Mode 04 подтвердить post-clear statuses, немедленный checkpoint и сохранение permanent DTC до выполнения критериев ECU.

**Итог:** 0.4.2 программно проверен и упакован; аппаратная приёмка новой DTC-функции ещё не выполнена.
