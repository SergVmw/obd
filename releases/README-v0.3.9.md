# H2 Gauge 0.3.9 — assets + power-loss persistence кандидат

**Дата сборки:** 2026-09-22  
**Плата:** только ESP32-S3 DevKitC-1 / ESP32-S3-WROOM-1-N16R8  
**PlatformIO environment:** `esp32s3_n16r8`  
**Config schema:** 6  
**Статус:** программно проверенный кандидат; аппаратная проверка assets, random power cut и OTA на N16R8 ещё требуется.

Аппаратно подтверждённым исходным релизом остаётся 0.3.7. Отдельный OTA-hardening gate 0.3.7→0.3.8 не закрывается и не подменяется выпуском 0.3.9. Уже собранные артефакты 0.3.8 не изменялись; после успешной упаковки 0.3.9 каталог `releases/` очищен согласно правилу «только актуальная версия».

## Файлы

| Файл | Назначение | Размер | SHA-256 |
|---|---|---:|---|
| `h2-gauge-v0.3.9-esp32s3-n16r8.bin` | **Обычный app image для web OTA** | **1 159 184 байт** | `4eddc675f2f483eea922546f03debde6c22bd0b52f72a69c8f4dac8ee973c023` |
| `h2-gauge-v0.3.9-esp32s3-n16r8-factory.bin` | Полная чистая USB-установка с offset `0x0` | **1 224 720 байт** | `08917e028fb29841b54849f8266f8ba6512e36f671267581d543459cab7128bc` |

Проверка:

```bash
cd releases
sha256sum -c SHA256SUMS-v0.3.9.txt
```

## Какой файл выбирать

### Штатное обновление из web-сервиса

Выбирать только:

```text
h2-gauge-v0.3.9-esp32s3-n16r8.bin
```

Не выбирать factory image, bootloader, partitions, LittleFS/filesystem или merged image. UI сначала выполняет metadata preflight, затем отправляет app как raw `application/octet-stream`. Успех подтверждается только после проверки image, выбора и read-back boot partition; прибор перезагружается автоматически.

Обычный app OTA записывает только неактивный app slot. Раздел LittleFS не пересекается с `app0/app1`, поэтому custom assets и 20-секундный journal сохраняются.

### Действительно чистая USB-установка

`h2-gauge-v0.3.9-esp32s3-n16r8-factory.bin` содержит:

```text
0x00000  bootloader
0x08000  partition table
0x0E000  boot_app0 / initial OTA data
0x10000  application 0.3.9
```

Файл записывается с offset `0x0`. При действительно чистой установке с предварительным erase будут удалены NVS, trip, калибровки, LittleFS journal и custom assets. Factory image не предназначен для web OTA и не является способом исправить штатное обновление.

## Что изменено относительно 0.3.8

### Пользовательский фон и логотип

- Локальные PNG/JPEG/WebP валидируются и декодируются в браузере.
- Фон центрируется/crop/darken до строгих `240×240`, RGB565 LE, **115 200 байт**.
- Логотип пропорционально вписывается максимум в `220×80`; payload строго `width×height×2`.
- До передачи показываются preview, конечные dimensions/bytes и CRC32.
- ESP32 проверяет metadata, точный `Content-Length`, payload length, CRC32 и полный file read-back.
- Неактивный A/B asset bank становится активным только после CRC-защищённого A/B manifest commit.
- Raw upload имеет 2-секундный idle timeout, отдельный 30-секундный absolute deadline и не отключает TWDT.
- Custom background/logo читаются один раз при startup в PSRAM/current cache.
- Встроенные carbon/HAVAL остаются безусловным fallback.
- Есть отдельные enable/embedded-fallback/delete controls; factory reset сохраняет assets.

### Software-only persistence 20/60

- Trip и petrol calibration объединены в канонический **140-байтный** record с magic/schema/monotonic sequence/CRC32.
- Dirty-only append в LittleFS выполняется каждые **20 секунд**.
- Два segment `/trip-journal.a/.b` ограничены **256 КиБ** каждый.
- Append считается успешным только после `flush()` и exact read-back.
- Boot scan прекращается на первом torn/corrupt record и сохраняет предыдущий valid snapshot.
- Combined NVS mirror с той же sequence обновляется каждые **60 секунд**.
- Boot выбирает newest valid LittleFS/NVS snapshot и сохраняет миграцию `h2trip`/`h2petcal`.
- Reset/calibration/service/reboot/low-voltage/engine-stop выполняют forced checkpoint.
- Непустой unmountable LittleFS никогда не форматируется автоматически; one-time format разрешён только для полностью стёртого `0xFF` раздела.
- Web UI показывает mount/source/sequence/segment/tail/CRC/write age/counts/failures.
- Нормальное окно потери: **0–20 секунд**; NVS-only fallback: **0–60 секунд**.

## Результаты программной проверки

- Clean PlatformIO 6.1.18 build: **PASS**, warnings/errors отсутствуют.
- Platform: `espressif32@6.8.1`; Arduino-ESP32 2.0.17.
- Dependency graph: project-local `H2PatchedWebServer 2.0.0-h2.1`.
- RAM: **51 684 / 327 680 байт (15,8%)**.
- Flash payload: **1 158 765 / 4 194 304 байт (27,6%)**.
- H2 manifest: ровно одна запись `0.3.9 / esp32s3-n16r8`.
- 25 brightness/input/config host regression groups: **PASS**.
- 11 OTA diagnostics/migration groups: **PASS**.
- CRC/canonical persistence ASan/UBSan tests: **PASS**.
- Storage recovery ASan/UBSan: dirty intervals, newest source, torn tail, corrupt CRC, NVS repair, monotonic reset и asset A/B/fallback: **PASS**.
- Static brightness/storage/OTA/font gates: **PASS**.
- Browser fixture: image validation, RGB565 conversion, byte count/CRC, raw body/headers, enable/delete, persistence panel, OTA recovery и responsive layouts: **PASS**.
- Embedded web: `128 808` байт HTML → deterministic `63 047` байт gzip, byte-for-byte check: **PASS**.
- Packaged app побайтно равен clean build output.
- Factory bootloader/partition/boot_app0 offsets, FF gaps и app payload с `0x10000`: **PASS**.
- App размер не кратен 1 436: последний raw-фрагмент **332 байта** (`1 159 184 = 807 × 1 436 + 332`), поэтому parser regression не скрыта padding.

## Обязательная аппаратная проверка 0.3.9

1. Обеспечить стабильное питание и остановленный автомобиль.
2. Выбрать обычный app `h2-gauge-v0.3.9-esp32s3-n16r8.bin`.
3. Дождаться metadata preflight, 100%, `verified:true`, `bootVerified:true` и автоматического reboot без RESET.
4. Подтвердить current/running/boot = 0.3.9, противоположный slot, image state `valid` и сохранность config schema 6.
5. Загрузить фон и логотип, проверить preview на GC9A01, reboot, enable/fallback/delete.
6. Выполнить app0↔app1 OTA/rollback и подтвердить сохранность assets/journal.
7. Провести серию random power cuts в начале/середине/конце append и при segment rotation.
8. Подтвердить newest LittleFS/NVS recovery, torn-tail fallback и реальные bounds 20/60.
9. Измерить typical/worst-case append+flush+read-back и отсутствие render/TWDT деградации.
10. До этих проверок не называть 0.3.9 аппаратно подтверждённым релизом.
