# H2 Gauge 0.3.8 — OTA-hardening кандидат

**Дата сборки:** 2026-09-22  
**Плата:** только ESP32-S3 DevKitC-1 / ESP32-S3-WROOM-1-N16R8  
**PlatformIO environment:** `esp32s3_n16r8`  
**Config schema:** 6, без миграции и без намеренного сброса NVS  
**Статус:** программно проверенный кандидат; аппаратное OTA-подтверждение 0.3.8 ещё требуется.

Аппаратно подтверждённым исходным релизом остаётся 0.3.7: обычный app `.bin` успешно прошёл APP0→APP1, server verification, boot read-back и автоматический reboot. 0.3.8 выпущен отдельным номером и не подменяет тот результат.

## Файлы

| Файл | Назначение | Размер | SHA-256 |
|---|---|---:|---|
| `h2-gauge-v0.3.8-esp32s3-n16r8.bin` | **Обычный app image для web OTA** | **1 094 224 байт** | `a41fabdd08684473cdbbdb8df3b39654692e5a5e06508cc9658f7a34d9bf2e2d` |
| `h2-gauge-v0.3.8-esp32s3-n16r8-factory.bin` | Полная чистая USB-установка с offset `0x0` | **1 159 760 байт** | `5c5e511b4b0b39d9a0a21b7c9dc92b0079f8c9162fc1aac706df4c6cb03e6bb3` |

Проверка:

```bash
cd releases
sha256sum -c SHA256SUMS-v0.3.8.txt
```

В каталоге `releases/` намеренно оставлена только актуальная версия 0.3.8.

## Какой файл выбирать

### Штатное обновление из web-сервиса

Выбирать только:

```text
h2-gauge-v0.3.8-esp32s3-n16r8.bin
```

Не выбирать factory image, bootloader, partitions, filesystem или merged image. После выбора UI сначала читает небольшой metadata probe и отправляет `/api/ota/preflight`; полный binary POST начинается только после подтверждения имени, размера, ESP32-S3 header/app descriptor, условий устройства и неактивного раздела.

### Действительно чистая USB-установка

`h2-gauge-v0.3.8-esp32s3-n16r8-factory.bin` содержит:

```text
0x00000  bootloader
0x08000  partition table
0x0E000  boot_app0 / initial OTA data
0x10000  application 0.3.8
```

Этот файл записывается с offset `0x0` и удаляет прежние настройки/поездку/калибровки при полном erase. Он не предназначен для web OTA и не является способом «починить» штатное обновление.

## Что изменено относительно 0.3.7

- Единый absolute OTA deadline 180 секунд, который не продлевается slow trickle.
- Сохранён отдельный двухсекундный idle timeout и exact remaining-length raw read.
- Причины `RAW_ABORTED`: idle timeout, total timeout, disconnect, handler rejection.
- После completion error response TCP явно закрывается; OTA handle/state очищаются.
- Структурированные JSON errors со стабильным `code`, received/expected bytes, timeout и `Retry-After`.
- `/api/ota/status` восстанавливает конкретную ошибку после XHR network failure.
- `/api/ota/preflight` до полного image проверяет filename/size, metadata ESP32-S3, движение, питание и реальный target partition.
- UI использует динамические `maxImageBytes`, `probeBytes` и timeout и подавляет status/fuel/CAN polls во время OTA.
- Initial metadata probe имеет точный compile-time размер SDK-структур, собирается через любое число network chunks и копируется в выровненные структуры через `memcpy`.
- Остаток transport chunk, завершившего probe, записывается без потери.
- Native verify/select worker ограничен собственными 30 секундами и остатком общего deadline.
- Watchdog остаётся включённым; multipart остаётся отклонённым.

Полное техническое решение: [`../docs/OTA_HARDENING_0.3.8.md`](../docs/OTA_HARDENING_0.3.8.md).

## Результаты программной проверки

- Clean PlatformIO 6.1.18 build: **успешно**, warnings/errors отсутствуют.
- Platform: `espressif32@6.8.1`; Arduino-ESP32 2.0.17.
- Dependency graph выбрал project-local `H2PatchedWebServer 2.0.0-h2.1`.
- RAM: **51 156 / 327 680 байт (15,6%)**.
- Flash payload: **1 093 805 / 4 194 304 байт (26,1%)**.
- H2 manifest checker: ровно одна запись `0.3.8 / esp32s3-n16r8`.
- 25 host regression-групп brightness/input/config: **PASS**.
- 11 OTA journal/migration-групп: **PASS**.
- Static OTA gate: exact chunks, idle/absolute deadline, rollover, abort completion/TCP close, metadata preflight, structured errors, split/future probe model, native workers, boot read-back, journal: **PASS**.
- Browser fixture: dynamic limit, metadata preflight rejection до binary POST, structured HTTP error, connection-reset recovery через status, verified install и responsive UI: **PASS**.
- Embedded web: `112 898` байт HTML → deterministic `58 711` байт gzip, byte-for-byte check: **PASS**.
- Golos VLW/WOFF validation: **PASS**.
- Packaged app побайтно равен clean build output.
- Factory bootloader/partition/boot_app0 offsets, FF gaps и app payload с `0x10000`: **PASS**.
- App размер не кратен 1 436: последний raw-фрагмент **1 428 байт** (`1 094 224 = 761 × 1 436 + 1 428`). Регрессия не скрыта padding.

## Обязательная аппаратная проверка кандидата

1. Запустить сервис на аппаратно принятом 0.3.7.
2. Выбрать обычный app `h2-gauge-v0.3.8-esp32s3-n16r8.bin`.
3. Убедиться, что metadata preflight прошёл и назван правильный target slot.
4. Дождаться 100%, затем `verified:true`, `bootVerified:true` и автоматического reboot без RESET.
5. После старта подтвердить current/running/boot = 0.3.8, противоположный slot = 0.3.7, running image state = `valid`.
6. Проверить сохранность Wi‑Fi/config schema 6, trip, petrol/LPG и brightness calibration.
7. До этой проверки не называть 0.3.8 аппаратно подтверждённым релизом.
