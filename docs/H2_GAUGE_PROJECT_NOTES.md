# H2 Gauge 0.3.9 — актуальные проектные решения

**Дата актуализации:** 2026-09-22  
**Единственная аппаратная цель:** ESP32-S3 DevKitC-1 compatible с модулем ESP32-S3-WROOM-1-N16R8.  
**Состояние:** OTA 0.3.7 аппаратно подтверждён 2026-09-20. Штатный app `.bin` прошёл web OTA из APP0 в APP1, устройство автоматически перезагрузилось без RESET, running/boot — APP1, image state — `valid`; APP0 содержит 0.3.6.2, APP1 и текущая аппаратно принятая сборка — 0.3.7. Исправление последнего неполного raw-фрагмента доказано на реальном 768-байтном хвосте. 0.3.8 остаётся отдельным OTA-hardening кандидатом и требует физического gate. Следующий кандидат 0.3.9 добавляет custom visual assets и software-only journal 20/60; локальные программные проверки не заменяют тесты на физическом N16R8 и автомобиле.

## 1. Назначение

Круглый прибор для Haval H2 2018 1.5T с ГБО BRC Sequent 32 EVO OBD. Основные данные:

- относительный наддув;
- напряжение;
- текущий и средний расход;
- раздельная статистика бензина и LPG;
- скорость, RPM, температуры и диагностические значения;
- статус OBD и текущего топлива `95/LPG`.

## 2. Аппаратная платформа

```text
ESP32-S3-WROOM-1-N16R8
16 MB QSPI Flash
8 MB Octal PSRAM
GC9A01 240×240
SN65HVD230 3.3 V
одна кнопка
PC817/EL817 LPG input
```

Основная и единственная PlatformIO environment: `esp32s3_n16r8`.

```text
board: esp32-s3-devkitc-1
flash mode: QIO
memory type: qio_opi
PSRAM: OPI 80 MHz
native USB CDC: enabled
```

Generic board manifest PlatformIO может печатать базовое описание N8/4 MB без PSRAM. Оно не отражает overrides проекта. Физические 16/8 МБ окончательно подтверждаются runtime-log приобретённой платы.

## 3. GPIO

| Функция | GPIO |
|---|---:|
| Кнопка MODE/WAKE | 4 |
| LPG sense | 5 |
| LDR / ADC1_CH5 | 6 |
| TFT backlight control | 7 |
| TFT CS | 10 |
| TFT DC | 11 |
| TFT RST | 12 |
| TFT MOSI | 13 |
| TFT SCLK | 14 |
| TWAI TX | 16 |
| TWAI RX | 17 |

Зарезервированы и не используются:

- GPIO19/20 — native USB;
- GPIO35/36/37 — Octal PSRAM;
- GPIO0/3/45/46 — strapping;
- GPIO38 — RGB LED распространённой DevKitC-1 v1.1;
- GPIO43/44 — UART0/диагностический резерв.

## 4. CAN и OBD

Используется встроенный TWAI ESP32-S3 без MCP2515. Внешний физический трансивер обязателен.

SN65HVD230 находится в коробке дисплея рядом с ESP32-S3:

```text
GPIO16 → TXD pin 1
GPIO17 ← RXD pin 4
3V3 → VCC pin 3
GND → pin 2
CAN-L pin 6 → OBD pin 14
CAN-H pin 7 → OBD pin 6
Rs pin 8 → GND
Vref pin 5 → NC
```

Возле VCC установить 100 нФ и рекомендуется 1 мкФ. TX/RX должны быть короткими. CAN-H/CAN-L вести витой парой. Дополнительный терминатор 120 Ом не устанавливать.

OBD работает в read-only логике ISO 15765-4, 11-bit, начальная скорость 500 кбит/с. Производственные запросы — стандартный Mode 01. Mode 22/ISO-TP оставлен как отключённый framework; неподтверждённые Haval DID не добавляются.

TWAI обрабатывает bus-off через штатный recovery. CAN Monitor первой версии предоставляет REST-снимки агрегированных кадров и не разрешает произвольную отправку из web UI.

## 5. GC9A01 и интерфейс

```text
SCLK → GPIO14
MOSI → GPIO13
CS   → GPIO10
DC   → GPIO11
RST  → GPIO12
BLK  → GPIO7, подтверждённый logic-вход встроенного ключа
VCC  → 3.3 V
GND  → GND
```

Между GPIO12/RST и GND устанавливается 10 кОм. Это удерживает дисплей в reset до начала инициализации и предотвращает вспышку предыдущего кадра перед красным HAVAL.

Владелец подтвердил BLK→GPIO7 и наличие транзистора на модуле. Если BLK — штатный low-current logic-вход, внешний high-side каскад не нужен; полярность и ток ещё должны быть проверены на столе. Ток LED через GPIO не пропускается. LEDC 5 кГц / 8 бит; TFT_eSPI больше не включает BLK самостоятельно при init. Подсветка остаётся выключенной до очистки TFT, затем используется выбранный начальный уровень и плавный логотип HAVAL.

Красный логотип HAVAL проявляется плавно; перед ним не должна появляться шкала. Рабочий фон — тёмный карбон. Шкала расположена близко к кромке. Доступны segmented и настоящая непрерывная solid arc.

Полноэкранный framebuffer:

```text
RGB565: 240 × 240 × 2 = 115200 bytes
```

Он размещается в PSRAM. При отсутствии PSRAM используется аварийный 8-bit framebuffer 57 600 байт, а в log выводится ошибка.

Экранная типографика 0.3.2 построена на pinned Golos Text commit `cf2e27222937d97c2d858fff0499bcc667a64e9d` по лицензии SIL OFL 1.1. В PROGMEM находятся три VLW subset с 8-bit alpha: SemiBold 13 px (`16 015` байт), SemiBold 19 px (`28 218` байт) и Bold 38 px для цифр (`7 800` байт). Три font-specific RGB565 text layer постоянно загружают по одному smooth font, поэтому штатный кадр не вызывает `loadFont()`/`unloadFont()` и не создаёт churn метрик в heap.

Каждый text layer перед надписью получает точную полосу текущего framebuffer; callback TFT_eSPI читает фактический пиксель слоя и корректно смешивает полупрозрачные края с карбоном или цветом панели. После рендера полоса копируется обратно. При отсутствии PSRAM или неудачном создании любого слоя используется Golos SemiBold 14 px 1-bit GFX fallback без отключения интерфейса.

## 6. Кнопка и sleep

Единственная кнопка подключается между GPIO4 и GND, active LOW. GPIO4 поддерживает RTC wake.

- короткое нажатие — следующая страница;
- в режиме яркости «Ручное» четыре быстрых нажатия — День/Ночь с немедленным сохранением; 1–3 коротких нажатия ждут 400-мс окно, удержания отменяют multi-click;
- длинное — контекстное действие;
- около 5 секунд на стоящем автомобиле — service portal;
- удержание при старте около 3 секунд — принудительный сервис;
- default auto-return — 5 секунд.

Опасные действия блокируются по raw speed, а не по пользовательской скорректированной скорости.

Low-voltage deep sleep включается только при свежем PID 42 ниже 11.5 В непрерывно 3 секунды. Перед сном сохраняется trip, останавливается TWAI, TFT выключается и удерживается в reset. Пробуждение — GPIO4 или таймер 30 секунд.

## 7. LPG

Вход берётся параллельно двум проводам одной катушки BRC:

```text
катушка → полный мост 4×1N4007
       → 2.2 kΩ + 2.2 kΩ pulse-rated
       → LED PC817C/EL817C
```

Изолированный выход:

```text
3V3 → 10 kΩ → GPIO5/LPG_SENSE
GPIO5 → collector PC817
emitter → SIGNAL_GND
GPIO5 → 100 nF → SIGNAL_GND
```

Сигнал active LOW. Сторона катушки не соединяется с GND ESP32-S3. Разные клапаны не объединяются.

`lpgEnabled` — главный выключатель. Когда он выключен:

- GPIO5 не опрашивается;
- LPG mode и расчёты отключены;
- LPG-статистика и предупреждения скрыты;
- PID STFT/LTFT для LPG-контроля не запрашиваются;
- бензиновый расчёт продолжает работать.

При включённом LPG обязательны индикатор `95/LPG`, раздельные средние и раздельная статистика. Центральный средний расход автоматически следует текущему топливу.

STFT+LTFT warning включается после превышения абсолютной суммы 10% в течение 5 секунд. Это диагностическое предупреждение, а не команда автоматически менять карту BRC.

BRC K-Line/KWP2000 остаётся отключённым будущим источником. Неподтверждённые identifiers не используются.

## 8. Расход и калибровки

Бензиновый Auto-источник использует PID 5E с резервом по MAF. LPG рассчитывается по MAF с отдельными AFR, density и correction.

Full-tank калибровка бензина использует независимый постоянный интервал между полными заправками. Применение заправки:

```text
new correction = actual liters / accumulated raw petrol liters
```

После применения автоматически начинается следующий интервал. Обычный trip при этом не сбрасывается.

Коррекция скорости имеет диапазон `−20…+20 км/ч`, default `0`. Она применяется к отображению, расстоянию и л/100 км. Raw speed сохраняется для parked guards и безопасности.

DFCO использует throttle/RPM и может показывать нулевой расход при закрытом дросселе и повышенных оборотах.

## 9. Web UI, NVS и OTA

Service portal работает локально без интернета:

```text
SSID: H2-Gauge-XXXX
password default: h2gauge18
URL: http://192.168.4.1
```

Пароль нужно изменить после первой установки. Web UI содержит реальные настройки цветов, segmented/solid arc, preview, языка, центрального показателя, топлива, CAN, LPG, калибровок и defaults.

Полный сервис и OTA разрешены только при parked guards. Опциональный read-only Wi‑Fi во время обычной работы допускается архитектурой, но пока не реализован. BLE зарезервирован для будущей телеметрии.

Config schema **6** дописывает 12-байтные настройки яркости после неизменённого schema-5 prefix (196 байт), перед checksum. Исправная запись schema 5 (200 байт) проверяется и автоматически переносится в текущую 212-байтную конфигурацию. Старые коэффициенты топлива, скорость, Wi-Fi и оформление сохраняются; новый режим — «Всегда день». Если старый ночной уровень выше дневного, он ограничивается дневным. Trip и бензиновая калибровка остаются в отдельных неизменённых namespace. Schema 4 и повреждённые записи заменяются defaults. Выученный LDR-диапазон хранится отдельно в `h2light/range`: явный 16-byte LE формат с версией/checksum.

POST `/api/config` транзакционный: новый объект проверяется отдельно, включая `isfinite()` и invariant порогов `min < 0 < warning < danger <= max`; невалидный объект получает HTTP 422 и не изменяет активную RAM-конфигурацию. При ошибке записи NVS сервер восстанавливает прежнюю RAM-конфигурацию.

Factory reset требует `X-H2G-Action: factory-reset` и имеет cooldown 10 секунд. Web OTA требует `X-H2G-Action: ota`, не допускает параллельную сессию и имеет cooldown 30 секунд. Все ошибки OTA сходятся в единый abort/cleanup path. OTA принимает только app image; bootloader, partition table, filesystem и factory image через app endpoint не загружаются.

В 0.3.6 OTA полностью переведён с Arduino `Update` на прямые `esp_ota_begin/write/end`. До первой записи проверяются ESP application header, descriptor и chip ID ESP32-S3; затем обязательны точное равенство Content-Length/received/written, финальная проверка образа, `esp_ota_set_boot_partition(target)` и read-back через `esp_ota_get_boot_partition()`. HTTP 200 выдаётся только после совпадения выбранного адреса с целевым. После завершённого ответа сервис сам перезагружает устройство.

Первая реальная попытка этой реализации дошла в браузере до 100%, но устройство reset до server confirmation: running и boot остались `app0@0x10000`, 0.3.6, а pending record отсутствовал. Браузерные 100% не доказывают `RAW_END`, поэтому это не был подтверждённый rollback. Точный reset reason той попытки неизвестен: доступный позже `power_on` относился уже к текущему запуску. После неё потенциально блокирующие `esp_ota_end()`/`esp_ota_set_boot_partition()` были вынесены из подписанного на TWDT `loopTask`, а durable journal перенесён перед финализацию. Позднейшая APP1-форензика второго отказа доказала, что выполнение фактически останавливалось ещё внутри последнего raw-read.

В исправленной 0.3.7 эти ESP-IDF вызовы исполняются коротким FreeRTOS worker. `loopTask` ждёт worker через отдельный semaphore с 100-мс интервалом и продолжает вызывать `feedLoopWDT()`; TWDT остаётся включённым. Ожидание ограничено 30 секундами: аномально не вернувшийся native call приводит к контролируемому software restart с уже сохранённой phase. Результаты обоих вызовов, boot read-back и все прежние проверки обязательны. Откат source selection после read-back mismatch также проходит через worker.

`OtaDiagnostics` хранит в отдельном namespace `h2ota` checksummed 64-байтную запись source/target/size/received/attempt/result/phase. Формат v3 читает прежние v1/v2 records. До входа raw parser в синхронный body-loop сохраняется `receiving`; каждые 256 КиБ обновляется принятый размер. Перед `esp_ota_end()` фаза становится `verifying`, после успешной проверки image — `image_verified`, после выбора и read-back boot-раздела — `boot_selected`. На старте target означает applied, source+`boot_selected` — настоящий rolled_back, source+`receiving` — `interrupted_upload`, а source на промежуточной финальной фазе — `interrupted_finalize`. Native ошибки сохраняются как `finalize_failed`/`boot_selection_failed` с `esp_err_t`; reset reason попытки также остаётся в record. Pending-образ подтверждается через `esp_ota_mark_app_valid_cancel_rollback()`. `/api/status` и UI показывают phase, received/image size, running/boot/next, image state и reset reason без serial monitor. Запись не меняет namespace конфигурации, trip, топливной или световой калибровки.

0.3.7 добавляет независимую H2 build identity. В DROM каждого нового app находится 56-байтный packed manifest с двумя magic, версией формата, размером, `H2G_FW_VERSION`, target и trailer. `FirmwareSlots` один раз при входе в сервис читает первые DROM-сегменты `app0/app1`, проверяет manifest и кэширует результат. Это не сканируется при каждом 1,2-секундном status poll. Ранее выпущенный 0.3.6 определяется по точному `app_elf_sha256`; неизвестный старый/повреждённый image честно показывается как «неизвестно» или «пусто». Версии обоих slots видны на GC9A01 service screen, в постоянных верхних web-карточках и в `ota.slots[]` API вместе с running/boot/next flags.

CAN RX обрабатывает не более 16 кадров и 2000 мкс за один вызов, чтобы очередь не монополизировала loop. Arduino loopTask подписан на Task Watchdog.

## 10. Питание и две коробки

Коробка возле OBD:

```text
F201 1 A
SMCJ30CA
TPS26600PWP
LC-фильтр
MP1584, настроенный на 5.00 V
входная сторона LPG optocoupler
```

MP1584 нельзя подключать непосредственно к OBD +12 В. До него обязательны предохранитель, TVS, reverse-polarity/OVP cutoff и фильтрация.

При мгновенном снятии switched питания последний software checkpoint выполнить невозможно. Текущие trip/petrol-calibration records пишутся в NVS раз в 60 секунд, поэтому abrupt cut теряет хвост 0…60 секунд. Пользователь уточнил, что отдельного `ACC_OFF/POWER_FAIL` и гарантированного hold-up, скорее всего, не будет; основной дальнейший путь — software-only. Предпочтительный проект следующей версии: 5-секундный append-only LittleFS journal с sequence/CRC, двумя bounded segments и сохранением 60-секундного NVS mirror как fallback. Полный аудит, ограничения и тестовая матрица сохранены в [`POWER_LOSS_PERSISTENCE_DESIGN.md`](POWER_LOSS_PERSISTENCE_DESIGN.md).

Коробка дисплея:

```text
ESP32-S3 DevKitC-1 N16R8
SN65HVD230
GC9A01
кнопка GPIO4
выход LPG на GPIO5
logic-вход ключа BLK от GPIO7
LDR-делитель и RC на GPIO6
```

Межблочный шестиконтактный разъём:

```text
1 +5V_PROTECTED
2 POWER_GND
3 CAN-H
4 CAN-L
5 LPG_SENSE
6 SIGNAL_GND
```

Отдельные 3.3 В между коробками не передаются. SN65HVD230 и TFT получают локальные 3.3 В.

TPS26600 reference settings:

```text
UVLO ≈ 5.98 V
OVP ≈ 24.03 V
RILIM 16.2 kΩ → ILIM ≈ 0.74 A
CdVdT 22 nF
```

RTN pin 8 TPS26600 нельзя напрямую соединять с GND pin 9. Подробный netlist находится в `h2-gauge-schematic-notes.md`.

## 11. Flash и release

`partitions_16mb_ota.csv`:

```text
NVS       20 KiB
OTA data   8 KiB
APP0       4 MiB
APP1       4 MiB
Core dump 64 KiB
LittleFS 8064 KiB
```

Аппаратно подтверждённая baseline-сборка 0.3.7 (историческая запись):

```text
RAM: 52 892 / 327 680 bytes (16.1%)
app Flash payload: 1 080 229 / 4 194 304 bytes (25.8%)
app .bin: 1 080 640 bytes
```

Release:

```text
h2-gauge-v0.3.7-esp32s3-n16r8.bin
SHA-256 dc3b7d3b844481dde00db41c97deedc6204884d87c86ee156f6835b5d5ec652e

h2-gauge-v0.3.7-esp32s3-n16r8-factory.bin
SHA-256 06b841a847fc427ddcb39923ce7a400505d99f688fcdeabf5de94cb07215654a
```

Первый файл — app image для OTA. Второй — merged image для чистой записи с offset 0x0; он намеренно содержит начальную OTA data и не используется для сохранения NVS.

Релиз 0.3.7 проверен PlatformIO 6.1.18, `espressif32@6.8.1` и Arduino-ESP32 2.0.17: clean-сборка успешна без warnings/errors, dependency graph выбирает project-local `H2PatchedWebServer`. OTA-validator подтверждает exact remaining-length raw read, 2-секундный timeout настоящего parser client, FreeRTOS worker/semaphore для финальных операций, включённый TWDT, journal v3, set/read-back boot partition, startup confirmation и per-slot manifests. Прошли 25 прежних host-групп и 11 OTA-diagnostics групп с ASan/UBSan, включая `interrupted_upload`, сохранённый receive progress и миграцию v2→v3. Embedded HTML 109 525 байт совпадает с 57 724-байтным gzip. Packaged app побайтно равен build output; проверены SHA-256, factory offsets/FF gaps и app payload по `0x10000`. Новый app имеет неполный 768-байтный final raw fragment, то есть тест не скрыт padding-ом. Аппаратная проверка мост 0.3.6.2→обычный OTA 0.3.7→автоматический запуск APP1 успешно завершена 2026-09-20.

Программно проверенный кандидат 0.3.8:

```text
RAM: 51 156 / 327 680 bytes (15.6%)
app Flash payload: 1 093 805 / 4 194 304 bytes (26.1%)
app .bin: 1 094 224 bytes; final raw fragment 1 428 bytes
SHA-256 app: a41fabdd08684473cdbbdb8df3b39654692e5a5e06508cc9658f7a34d9bf2e2d
SHA-256 factory: 5c5e511b4b0b39d9a0a21b7c9dc92b0079f8c9162fc1aac706df4c6cb03e6bb3
```

Clean PlatformIO build завершён без warnings/errors. Прошли 25 host + 11 OTA diagnostic групп, расширенный static gate, Playwright metadata-preflight/error-recovery fixture, deterministic embedded web 112 898→58 711 байт, fonts, manifest и packaged/factory layout. Это ещё не заменяет обязательный физический OTA 0.3.7→0.3.8; исторический протокол и acceptance checklist сохранены в [`OTA_HARDENING_0.3.8.md`](OTA_HARDENING_0.3.8.md).

Финально упакованный кандидат 0.3.9:

```text
RAM: 51 684 / 327 680 bytes (15.8%)
app Flash payload: 1 158 765 / 4 194 304 bytes (27.6%)
app .bin: 1 159 184 bytes; final raw fragment 332 bytes
SHA-256 app: 4eddc675f2f483eea922546f03debde6c22bd0b52f72a69c8f4dac8ee973c023
SHA-256 factory: 08917e028fb29841b54849f8266f8ba6512e36f671267581d543459cab7128bc
```

Clean build, H2 manifest `0.3.9 / esp32s3-n16r8`, host/static/browser gates, app/build byte equality, checksums и factory layout проверены. В `releases/` оставлена только 0.3.9. Полный отчёт: [`../releases/README-v0.3.9.md`](../releases/README-v0.3.9.md).

### Реализованный PSRAM-кэш и render benchmark

Статус: реализовано и проверено сборкой 2026-09-18; физический runtime-тест на GC9A01 ещё обязателен.

После успешного создания основного 16-bit framebuffer создаётся второй RGB565 sprite 240×240 в PSRAM. Карбоновый фон строится в нём один раз, а в начале каждого кадра восстанавливается точным `memcpy()` 115 200 байт через подтверждённый TFT_eSPI 2.5.43 API `getPointer()`. Поэтому 960 итераций построения узора, `fillSprite()` и около 100 тысяч графических перезаписей больше не выполняются каждый штатный кадр. Если allocation кэша не удался, остаётся прежний безопасный вызов `drawCarbonBackground()`.

Основные pixel buffers в штатном smooth-режиме:

```text
main RGB565 framebuffer     115 200 bytes
carbon RGB565 cache         115 200 bytes
text layers 240×24/32/46     48 960 bytes
-----------------------------------------
total pixel buffers         279 360 bytes PSRAM
```

Это не включает небольшие массивы метрик трёх загруженных smooth fonts. На N16R8 запас относительно 8 МБ PSRAM остаётся большим. Все sprite удаляются в `releaseFramebuffer()` перед deep sleep.

Агрегированная телеметрия печатается раз в 300 кадров, а не каждый кадр: average/max полного render, average восстановления фона, состояние background cache и smooth text layers, а также свободная PSRAM. Такой интервал не создаёт постоянной нагрузки serial log. Частоту кадров и эффект SPI-передачи нужно подтвердить на плате; успешная компиляция не является аппаратным benchmark.

Пример строки:

```text
UI benchmark 300 frames: render avg/max ... us, bg restore avg ... us, cache=yes, smooth=yes, free PSRAM=...
```

Для TFT_eSPI 2.5.43 используется `getPointer()`, а не неподтверждённый `getBuffer()`. Основной и фоновый sprite имеют одинаковую 16-битную глубину, поэтому внутреннее byte-swapped RGB565 представление копируется без преобразования. Smooth-font callback также работает только внутри текущего text layer и не обращается к TFT по SPI.

### OTA transport и гарантированная активация (исправление 0.3.6)

В Arduino-ESP32 2.0.17 multipart обрабатывается синхронно внутри одного `WebServer::handleClient()`: `_parseForm()` читает тело побайтово, а `_uploadReadByte()` способен ждать подключённого клиента в неограниченном цикле. Поскольку `enableLoopWDT()` подписывает `loopTask`, а Arduino кормит его только между вызовами `loop()`, передача дольше штатного 5-секундного TWDT вызывала panic и перезагрузку посреди OTA. Поэтому multipart не возвращается и watchdog не отключается.

Web UI отправляет сам файл как `application/octet-stream`, имя передаётся в `X-H2G-Filename`, подтверждение — в `X-H2G-Action`. Raw callback WebServer вызывается блоками до 1 436 байт и выполняет `feedLoopWDT()` до/после записи. Project-local parser ограничивает каждый read точным остатком `Content-Length` и задаёт настоящему parser client timeout 2 секунды — меньше 5-секундного TWDT. Потеря передачи поэтому приводит к abort/HTTP error, а не к ожиданию байтов за body или WDT panic. В service mode вызывается `WiFi.setSleep(false)`.

0.3.6 устраняет отдельный класс ошибки, когда upload доходил до подтверждённого успеха и автоматической перезагрузки, но устройство снова запускало прежний слот. Arduino `Update` больше не участвует. Сервер сам выбирает следующий неактивный OTA subtype, проверяет его размер, вызывает `esp_ota_begin`, передаёт все байты через `esp_ota_write` и завершает `esp_ota_end`. Перед первой записью нужны ESP magic, допустимое число сегментов, chip ID 9 (ESP32-S3) и `ESP_APP_DESC_MAGIC_WORD`; factory/bootloader/partition images поэтому не принимаются. Ожидаемая, полученная и записанная длины обязаны совпасть.

После успешной проверки сервер вызывает `esp_ota_set_boot_partition(target)`, немедленно читает результат через `esp_ota_get_boot_partition()` и сравнивает физический адрес. Несовпадение даёт HTTP 500 и попытку восстановить source selection. Только после совпадения ответ содержит `verified:true`, `bootVerified:true` и `rebooting:true`; достижение браузером 100% передачи само по себе успехом не считается. Ответ завершается до отложенного software reset, ручной RESET не требуется.

До reboot в `h2ota` фиксируются source/target и размер. На новом старте pending-образ подтверждается до обычной работы, затем запись получает post-reboot результат. UI показывает фактические слоты и reset reason. Поле низкоуровневой диагностики называется `descriptorVersion`: в prebuilt Arduino-ESP32 2.0.17 это framework descriptor `esp-idf: v4.4.7 38eeba213a`, а не релиз H2 Gauge. Авторитетная текущая версия находится в `/api/status.version`, а версии содержимого slots — в проверенном H2 manifest/legacy-hash полях `ota.slots[]`; framework descriptor намеренно не выдаётся за номер релиза.

#### Реальный отказ последнего RAW_WRITE и окончательное исправление 0.3.7

19 сентября 2026 первая аппаратная попытка старого кандидата 0.3.7 (1 077 616 байт, SHA `b5cea758…48ad`) из исходной 0.3.6 закончилась reset до server confirmation. Чтобы убрать риск зависания `esp_ota_end()`/boot selection, были введены worker-задача, кормление TWDT и журнал фаз. Этот код попал в app-only мост 0.3.6.1 и следующий кандидат 0.3.7 (1 079 696 байт, SHA `b04c8d93…86bb`).

Вторая аппаратная попытка из моста 0.3.6.1 также дошла в браузере до 100%, после чего прибор автоматически reset и снова запустил APP0/0.3.6.1 без OTA journal. Браузерные 100% означали передачу body клиентом, но не доказывали последний parser callback на ESP.

Read-only дамп APP1 дал окончательную локализацию. Первые 1 078 436 байт (`0x1074A4`) побайтно совпали с ожидаемым app; последние 1 260 байт были `FF`. Арифметика `1 079 696 = 751 × 1 436 + 1 260` доказывает 751 успешный полный `RAW_WRITE` и отсутствие последнего 1 260-байтного callback. `otadata` сохранил `ota_seq=1`, то есть APP0; `RAW_END`, worker finalization и выбор APP1 не выполнялись.

Причина находится в raw parsing Arduino-ESP32 2.0.17: каждая итерация вызывала `client.readBytes(..., HTTP_RAW_BUFLEN)` для 1 436 байт без ограничения остатком `Content-Length`. Финальный read получил 1 260 байт и стал ждать ещё 176 несуществующих. После обычного запроса `WebServer::handleClient()` оставляет в повторно используемом `_currentClient` timeout 5 секунд: `WiFiClient::setTimeout(5)` в этой версии принимает секунды, а присваивание нового socket state не сбрасывает базовый `Stream::_timeout`. Пять секунд невозможного read совпали с 5-секундным TWDT `loopTask`; reset случился до возврата уже прочитанного хвоста в callback.

`WebServer::client()` возвращает `WiFiClient` по значению. Поэтому вызов timeout из `RAW_START` через `server_.client()` изменил бы только временную копию и не является исправлением.

В `lib/H2PatchedWebServer` vendored библиотека tag 2.0.17. Parser теперь запрашивает `min(HTTP_RAW_BUFLEN, _clientContentLength - totalSize)`, читает только доступные socket-байты, кормит TWDT внутри receive-loop и ограничивает отсутствие прогресса двумя секундами. Тот же timeout устанавливается непосредственно на reference настоящего parser client. `platformio.ini` игнорирует package library `WebServer`, dependency graph подтверждает `H2PatchedWebServer 2.0.0-h2.1`. Глобальный PlatformIO package не модифицируется. Watchdog остаётся включённым.

Journal повышен до v3. `recordReceiving()` выполняется после `esp_ota_begin()`, но до синхронного body-loop; каждые 256 КиБ записывается progress. На следующем boot source+`receiving` становится `interrupted_upload`. Затем тот же attempt без повторного увеличения номера проходит `verifying`, `image_verified`, `boot_selected`. Старые v1/v2 records читаются с миграцией phase semantics.

Мост 0.3.6.1 был отозван, потому что содержал неисправный parser. Новый app-only мост 0.3.6.2 (1 080 640 байт, SHA `345e4add…61d2`) включил parser fix и journal v3 и был один раз записан в APP0 по `0x10000`, без erase/NVS/otadata. Затем 0.3.7 того же размера (SHA `dc3b7d3b…652e`) успешно прошла обычным web OTA в APP1. Равенство `1 080 640 = 752 × 1 436 + 768` показывает, что аппаратно проверен именно неполный final fragment, а не подогнанный размер.

Полный postmortem и аппаратный результат:
[`OTA_POSTMORTEM_2026-09-19.md`](OTA_POSTMORTEM_2026-09-19.md). Одноразовые
recovery-образы и дампы после подтверждения удалены из чистого проекта.

#### Отдельный OTA-hardening кандидат 0.3.8

22 сентября 2026 принят отдельный номер 0.3.8: уже аппаратно подтверждённый 0.3.7 не превращается в hotfix с тем же номером. Idle watchdog 2 секунды сохранён, но raw parser теперь имеет также единый absolute deadline 180 секунд с wrap-safe `millis()` arithmetic. Поэтому клиент, который постоянно посылает редкие байты и никогда не достигает idle timeout, всё равно не может удерживать `handleClient()` бесконечно.

`HTTPRaw` хранит abort reason, application abort request и elapsed time. Parser различает idle, total timeout, disconnect и handler rejection, вызывает `RAW_ABORTED`, затем даёт route completion handler отправить JSON и явно закрывает TCP. `ServicePortal` закрывает OTA handle, очищает active/success/read-back state, но сохраняет byte counters и machine-readable code для `/api/ota/status`. Браузер при XHR network error открывает новое соединение к status endpoint вместо безусловного сообщения «соединение потеряно».

Перед полным binary POST UI получает фактические `maxImageBytes`, `probeBytes` и timeout, читает только начало локального файла и выполняет `/api/ota/preflight`. Сервер проверяет filename/size, chip ID ESP32-S3, app descriptor, движение, питание и неактивный раздел. Тот же actual body повторно проверяется в `RAW_START`/`RAW_WRITE`. Preflight не открывает handle и не расходует cooldown. Фоновые status/fuel/CAN polls во время OTA остановлены.

Probe имеет compile-time размер `sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)`. Он собирается через любое число 1 436-байтных transport chunks; структуры заполняются выровненным `memcpy`, а остаток блока, завершившего probe, записывается сразу после него. Static model отдельно проверяет будущий probe больше одного network chunk. Native verify/select worker получает минимум из собственного 30-секундного лимита и остатка общего deadline; TWDT остаётся включённым, multipart остаётся запрещённым.

Полный протокол, коды ошибок, программный gate и аппаратный acceptance checklist сохранены в [`OTA_HARDENING_0.3.8.md`](OTA_HARDENING_0.3.8.md).

#### Кандидат 0.3.9: пользовательские фон и стартовый логотип

Raw upload пользовательских background/logo реализован в LittleFS 7,875 МиБ. Браузер валидирует PNG/JPEG/WebP, source bytes/decoded pixels, выполняет crop/resize/darken и выдаёт little-endian RGB565: фон строго `240×240 / 115 200` байт, логотип пропорционально вписывается в `220×80`. Preflight передаёт dimensions/length/CRC32; ESP повторно проверяет metadata и точный `Content-Length`, пишет header+payload в неактивный A/B-файл, выполняет flush/full CRC read-back и только затем переключает второй CRC-защищённый A/B manifest.

Asset raw parser использует общий 2-секундный idle watchdog и отдельный 30-секундный absolute deadline, задаваемый handler в `HTTPRaw`; OTA сохраняет 180-секундный deadline. Multipart и отключение TWDT не используются. Background читается один раз в существующий PSRAM cache; logo — один раз перед splash. Встроенные `drawCarbonBackground()`/`kHavalLogoRgb565` всегда остаются recovery path. Factory reset сохраняет оформление, а enable/fallback/delete доступны отдельно. App-only OTA не пересекается с LittleFS; `erase_flash`, `uploadfs` и смена partition table остаются разрушающими операциями. Полная архитектура и матрица проверки: [`CUSTOM_VISUAL_ASSETS_DESIGN.md`](CUSTOM_VISUAL_ASSETS_DESIGN.md).

#### Кандидат 0.3.9: software-only persistence 20/60

Trip и petrol calibration кодируются в единый канонический record 140 байт: header magic/schema/bytes/monotonic sequence, sealed 64-byte `TripState`, sealed 56-byte `PetrolCalibrationState`, outer CRC32. Dirty-only checkpoint каждые 20 секунд append-ится в `/trip-journal.a` или `.b`; каждый сегмент ограничен 256 КиБ. Append считается успешным только после `flush()` и exact read-back. Boot scan идёт по полным CRC-valid records до первого torn/corrupt tail; при ротации старый active segment не удаляется до записи нового valid record.

Combined NVS mirror `h2persist/snapshot` получает ту же sequence каждые 60 секунд. Boot выбирает newest valid LittleFS/NVS record с wrap-safe ordering, затем синхронизирует отставшую сторону. Если новых records нет, исходные `h2trip`/`h2petcal` мигрируют в sequence 1; legacy stores продолжают обновляться на forced lifecycle checkpoints для downgrade compatibility. Factory reset создаёт sequence новее найденной до очистки, поэтому не удалившийся stale segment не воскресит старый trip.

Forced checkpoints выполняются для trip reset, start/apply calibration, service entry/reboot/timeout, engine stop и low-voltage sleep. В service mode/OTA operational state не меняется и periodic journal не выполняется. UI/REST показывают mount status, recovery source, latest/journal/NVS sequence, active segment/bytes/tail/rotation, CRC validity, write age/counts/failures. Непустой не монтируемый LittleFS не форматируется; auto-format разрешён только после полного raw scan, доказавшего erased `0xFF` partition. Нормальное loss window 0–20 секунд, fallback 0–60 секунд. Полная архитектура/endurance: [`POWER_LOSS_PERSISTENCE_DESIGN.md`](POWER_LOSS_PERSISTENCE_DESIGN.md).

## 11a. Адаптивная яркость — решение 2026-09-19

Приняты пользовательские варианты: плавный Авто, `3.3 В → LDR → 22 кОм → GND` на ADC1 GPIO6 через 1 кОм/100 нФ, ручной День/Ночь четырьмя нажатиями с сохранением, автоматическое обучение диапазона вместо кнопок захвата. Четыре режима — Auto / Manual / AlwaysDay / AlwaysNight. Default AlwaysDay оставлен намеренно: ещё не смонтированный LDR не должен менять яркость.

`BrightnessLogic` независим от Arduino: median5 + EMA 800 мс, гистерезис 32 ADC, deadband 1 п.п., раздельные задержки default 3000/1000 мс, линейная интерполяция, PWM slew 25 п.п./с. `BrightnessManager` читает четыре ADC1 отсчёта раз в 50 мс без неограниченных циклов. Он вызывается до ветвления normal/service, поэтому web-save применяется сразу, а ADC доступен в сервисе. Во время raw OTA аппаратный PWM удерживает последний duty.

Автокалибровка принимает только устойчивые двухсекундные окна, не учится на насыщении, ждёт диапазон ≥800 ADC, затем использует точки 15%/85%. Диапазон только расширяется, чтобы постоянный свет не переопределял день/ночь. До обучения действуют заданные исходные пороги. Изменённый диапазон записывается не чаще раза в 10 минут, плюс штатные checkpoints; отключение питания до checkpoint может потерять последние наблюдения. Нельзя отличить обрыв LDR от темноты только пассивным ADC-делителем: отсутствие датчика не обещается диагностировать автоматически.

POST конфигурации проверяет целочисленные диапазоны, boolean-типы, `night <= day` и `ADC day - night >= 200` до commit. Сброс диапазона защищён action-header/cooldown и сначала пишет NVS, затем очищает RAM. Dashboard сам владеет PWM, `TFT_BL/TFT_BACKLIGHT_ON` убраны из build flags для исключения принудительной 100%-ной вспышки в `TFT_eSPI::init()`.

Проверены 25 групп реальных C++ host-регрессий с ASan/UBSan, browser-контракт с тестовыми REST-ответами на 360…1280 px и сборка ESP32-S3. Host stubs не эмулируют электрический ADC, LEDC или настоящий ESP NVS; физические проверки остаются обязательными. Подробности: [BRIGHTNESS_GUIDE.md](BRIGHTNESS_GUIDE.md), [принципиальная схема](ambient-light-circuit.svg).

## 12. Обязательная физическая проверка

1. Проверить маркировку N16R8 на модуле.
2. Выполнить первую полную USB-прошивку.
3. Подтвердить runtime-log: target `esp32s3-n16r8`, Flash около 16 МБ, PSRAM около 8 МБ.
4. Проверить отсутствие вспышки TFT и работу GPIO12/RST.
5. Проверить кнопку GPIO4 и оба wake source.
6. Проверить GPIO5 через лабораторный LPG-вход.
7. Проверить питание SN65HVD230 и отсутствие дополнительного 120 Ом.
8. Подключить CAN сначала на стоящем автомобиле и запросить `01 00`.
9. Проверить реальные PID `0B`, `10`, `33`, `42`, `5E` и ECU response ID.
10. Измерить ток, падение 5 В, температуру закрытого корпуса и поведение под солнцем.
11. Подтвердить BLK logic/active-HIGH, работу 20% ↔ 80%, ADC GPIO6, обучение и ручной четырёхкратный жест по [чек-листу яркости](BRIGHTNESS_GUIDE.md).
12. **Выполнено 2026-09-20:** app-only мост 0.3.6.2 в APP0 → штатный web OTA обычного app 0.3.7 → server verification → автоматический reboot без RESET. Подтверждены current 0.3.7, APP0 0.3.6.2, APP1 0.3.7, running=boot APP1 и image state `valid`; последний 768-байтный raw fragment прошёл без TWDT.
13. **Ожидает выполнения для 0.3.8:** с работающего 0.3.7 пройти metadata preflight, штатный OTA обычного app 0.3.8 и автоматический reboot; подтвердить running/boot 0.3.8, противоположный slot 0.3.7, state `valid` и сохранность schema 6/NVS.
14. **Ожидает выполнения для 0.3.9 assets:** загрузить фон/логотип, проверить preview на GC9A01, reboot, disable/delete, app0↔app1 OTA/rollback и random cut на стадиях A/B commit.
15. **Ожидает выполнения для 0.3.9 persistence:** измерить append/flush/read-back, выполнить серию random cut в начале/середине/конце append и при rotation, подтвердить newest LittleFS/NVS recovery и реальные loss bounds 20/60.

N16R8 с Octal PSRAM имеет паспортный верхний предел окружающей температуры +65 °C без ECC. DevKitC-1 не является automotive-qualified платой.
