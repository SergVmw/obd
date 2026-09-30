# H2 Gauge 0.4.1 — corrected Check Engine / DTC release

**Дата упаковки:** 2026-09-30  
**Цель:** ESP32-S3 DevKitC-1 N16R8 (`esp32s3-n16r8`)  
**Config schema:** 6 (без миграции настроек)  
**Последний аппаратно подтверждённый release:** 0.3.9

## Статус и замена 0.4.0

0.4.1 — отдельный исправленный выпуск после review диагностической реализации. Пакет 0.4.0 отозван, не устанавливался на автомобиль и удалён из `releases/`; его ранее опубликованные SHA-256 не заменялись и не переиспользовались. Не устанавливайте 0.4.0.

0.4.1 программно проверен и упакован, но новый DTC-код ещё требует аппаратной read-only приёмки на Haval H2. До сравнения Mode 03/07/0A с независимым сканером не выполняйте Mode 04.

## Артефакты

| Файл | Назначение | Размер | SHA-256 |
|---|---|---:|---|
| `h2-gauge-v0.4.1-esp32s3-n16r8.bin` | **Обычный app image для штатного web OTA** | **1 190 288 байта** | `cd2f6f0bc966972997c8a91465aaf3b549a5084f11bc9a577a21c31776af1d41` |
| `h2-gauge-v0.4.1-esp32s3-n16r8-factory.bin` | Полная чистая USB-установка с offset `0x0` | **1 255 824 байт** | `8263b8e2cc46058fd41c6de480d52a45f4cdffffac84f515a1fc19c6de513291` |

Проверка:

```bash
cd releases
sha256sum -c SHA256SUMS-v0.4.1.txt
```

## Как обновлять

Для штатного OTA выбирайте только:

```text
h2-gauge-v0.4.1-esp32s3-n16r8.bin
```

Это обычный app image. Web OTA записывает неактивный OTA-слот, ждёт нативную проверку образа и boot read-back, затем автоматически перезагружает прибор. NVS, настройки, trip, бензиновая калибровка, DTC history, LittleFS journal и пользовательские ресурсы сохраняются.

После reboot физический экран «СЕРВИС» и верх web UI должны показать текущую сборку `0.4.1`, running/boot slot и версию каждого OTA-слота.

Factory image предназначен только для чистой USB-записи с offset `0x0`. Он содержит:

```text
0x00000  bootloader
0x08000  partitions_16mb_ota.csv
0x0E000  boot_app0
0x10000  application 0.4.1
```

Полная запись с erase удаляет NVS, настройки, trip, калибровки, DTC history, LittleFS journal и custom assets. Factory image нельзя загружать через web OTA.

## Исправления 0.4.1

- Engine ECU фиксируется только по первому структурно валидному ответу Mode 01 PID `0C`; ответ другого ECU не может изменить physical target.
- После привязки обычные PID-ответы другого ECU игнорируются; Mode 03/07/0A/04 и safety PID отправляются только physical request ID выбранного engine ECU.
- `7F <service> 78` считается промежуточным ResponsePending: исходная команда не передаётся повторно, P2*=5000 мс, абсолютный предел одного запроса=15000 мс, максимум восемь pending-ответов.
- NRC `0x11/0x12` обозначают unsupported service; прочие финальные NRC имеют отдельный статус `negative_response` и не доказывают отсутствие DTC.
- Положительный Mode 04 (`0x44`) больше не очищает historical-presence. Через 1,5 с запускается обязательный post-clear Mode 03/07/0A; он продолжается как manual-operation tail даже при paused periodic polling, после чего история немедленно checkpoint-ится.
- Runtime checkpoint считается успешным, если хотя бы одна запрошенная durable-копия (LittleFS journal или NVS mirror) прошла write/readback; отказ второго backend остаётся видимым в health counters.
- Ошибки аргументов и порядка OTA state machine больше не помечают NVS storage unhealthy; реальная ошибка записи по-прежнему видна.
- Flow Control не отправляется, если ISO-TP First Frame уже содержит весь объявленный payload; удалена недостижимая проверка длины.
- CRC32 переведён с восьми побитовых шагов на два table-driven nibble lookup на байт.
- API/UI показывают engine lock, ResponsePending count и состояние post-clear verification/checkpoint.

## Safety-модель Mode 04

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

Mode 04 не стирает permanent DTC напрямую. Если post-clear категория не прочитана успешно, её последнее подтверждённое historical-presence не выдаётся за исчезнувшее.

## Выполненные software gates

- 13 host-групп OBD/DTC, включая NRC 0x78, P2*/absolute deadline, bounded pending, несколько ECU, physical target и paused post-clear continuation;
- 13 host-групп OTA diagnostics, включая раздельную storage/state-machine health semantics;
- persistence/storage fault injection: torn/partial writes, corrupt readback на flush boundary, LittleFS-only, NVS-only и total checkpoint failure;
- brightness, persistence snapshot/CRC, storage recovery и browser UI regressions;
- static DTC/storage/OTA/font integration gates и проверка embedded web UI;
- PlatformIO `esp32s3_n16r8` build: RAM 53 340 байт, application flash 1 189 877 байт;
- H2 manifest: `0.4.1 / esp32s3-n16r8`, app offset `0x1c280`;
- packaged app byte-for-byte совпадает с build output;
- factory layout, SHA-256 и размер OTA partition проверены;
- app size не выровнен искусственно: финальный raw fragment — 1280 байт.

## Первая аппаратная приёмка

1. Выполнить штатный app OTA с аппаратно подтверждённой 0.3.9 и проверить automatic reboot, `current/running/boot = 0.4.1`, второй slot и image state `valid`.
2. На стоящем автомобиле сначала только прочитать PID 0C, PID 01 и Mode 03/07/0A; сравнить ECU ID и коды с независимым сканером.
3. Подтвердить отсутствие ложной смены engine ECU при ответах нескольких модулей.
4. Проверить transient history и восстановление после reboot из LittleFS/NVS.
5. Mode 04 проверять отдельно, только после read-only acceptance и с пониманием сброса readiness/freeze-frame.
6. После Mode 04 подтвердить post-clear statuses, немедленный checkpoint и сохранение permanent DTC до выполнения критериев ECU.

**Итог:** 0.4.1 программно проверен и упакован; аппаратная приёмка новой DTC-функции ещё не выполнена.
