# Changelog

## 0.4.2 — 2026-09-30

Corrective release по результатам дополнительного аудита 0.4.1. Production-код
изменён, поэтому опубликованные бинарники 0.4.1 не подменялись: выпущены новая
версия и новые SHA-256. Аппаратно подтверждённой основой остаётся 0.3.9.

### DTC history и достоверность API

- Усечённый ISO-TP response теперь добавляет увиденные DTC, но не снимает
  historical-presence с кодов, которые могли остаться в отброшенном хвосте.
  Авторитетность передаётся отдельно для конкретного assembled response через
  `!assemblyTruncated`, а не выводится из scan-global overflow-флага.
- `history.currentStateKnown` теперь обязательно требует `!state.truncated` и
  успешный `Complete` всех Mode 03/07/0A. `Unsupported` не считается
  доказательством отсутствия ранее наблюдавшегося кода.

### Persistence reset и post-clear rollover

- `RuntimePersistence::factoryReset()` после обязательного удаления старых
  LittleFS/NVS данных принимает любую новую read-back-подтверждённую durable
  копию. Journal-only success больше не превращается в ложную ошибку, если NVS
  clear прошёл, но последующий NVS `putBytes` отказал.
- Post-clear scan получил отдельный private schedule-флаг: deadline `0` после
  точного `uint32_t` rollover больше не принимается за «не запланировано».
  Проверка по-прежнему выполняется до pause-guard периодического polling.
- Behavioral Mode 04 regression включает `7F 04 78` → `44` без retransmit.

### Проверки и упаковка

- OBD host suite расширен до 15 групп: добавлены усечённый хвост history и
  deadline, равный нулю после rollover; официальный Mode 04 test включает NRC
  `0x78`. Storage fault injection проверяет отказ NVS rewrite после clear.
- Пройдены host/static/font/browser gates и clean PlatformIO N16R8 build: RAM
  53 340 байт, Flash payload 1 189 945 байт; manifest
  `0.4.2 / esp32s3-n16r8` находится по offset `0x1c280`.
- OTA app: 1 190 368 байт, SHA-256 `163ea671…8dd1b`; factory:
  1 255 904 байта, SHA-256 `7bd7cdae…f5d5c`; финальный raw fragment 1360 байт.
- В `releases/` оставлены только app/factory/checksums/report 0.4.2.

Подробности: [`docs/CODE_AUDIT_0.4.1_2026-09-30.md`](docs/CODE_AUDIT_0.4.1_2026-09-30.md),
[`docs/OBD_DTC_DIAGNOSTICS_DESIGN.md`](docs/OBD_DTC_DIAGNOSTICS_DESIGN.md) и
[`releases/README-v0.4.2.md`](releases/README-v0.4.2.md).

## 0.4.1 — 2026-09-30

Исправленный release после review отозванного 0.4.0. Версия 0.4.0 не
устанавливалась на автомобиль, удалена из `releases/`, а её опубликованные
SHA-256 не заменялись in-place. Аппаратно подтверждённой основой остаётся 0.3.9.

### OBD/DTC transport и выбор ECU

- Functional discovery теперь начинает с PID `0C`; первый структурно валидный
  RPM response однократно фиксирует engine ECU. PID 01, обычная telemetry и
  physical Mode 03/07/0A/04 не принимают ответы другого ECU.
- `7F <service> 78` стал промежуточным ResponsePending без повторной передачи:
  P2*=5000 мс, абсолютный предел запроса=15000 мс, максимум восемь pending.
- NRC `0x11/0x12` классифицируются как unsupported; прочие финальные NRC имеют
  отдельный `negative_response` и не удаляют DTC/history как будто ответ пустой.
- Flow Control отправляется только когда после First Frame действительно нужны
  Consecutive Frames; удалена недостижимая проверка длины.

### Mode 04 и persistence

- Positive `0x44` больше не очищает historical-presence. Через 1,5 с обязательное
  Mode 03/07/0A выполняется как manual continuation даже при paused periodic
  polling; итог немедленно проходит отдельный durable checkpoint.
- RuntimePersistence checkpoint успешен, если хотя бы одна запрошенная копия
  LittleFS/NVS записана и прочитана обратно. Health отказавшего mirror остаётся
  ложным и виден отдельно.
- Ошибки аргументов/порядка OTA state machine больше не выставляют NVS
  `storageHealthy=false`; реальный отказ `Preferences` по-прежнему это делает.
- CRC32 переведён с восьми побитовых шагов на два nibble-table lookup на байт.

### Проверки и упаковка

- OBD suite расширен до 13 групп: NRC 0x78, P2*/absolute deadline, bounded
  pending, несколько ECU, immutable engine target, FC boundary и paused post-clear tail.
- OTA suite расширен до 13 групп; storage recovery fault injection проверяет
  partial/corrupt readback, LittleFS-only, NVS-only и dual failure checkpoints.
- Пройдены host/static/font/browser gates и PlatformIO N16R8 build: RAM
  53 340 байт, Flash payload 1 189 877 байт; manifest
  `0.4.1 / esp32s3-n16r8` находится по offset `0x1c280`.
- OTA app: 1 190 288 байта, SHA-256 `cd2f6f0b…af1d41`; factory:
  1 255 824 байт, SHA-256 `8263b8e2…513291`; финальный raw fragment 1280 байт.
- В `releases/` оставлены только app/factory/checksums/report 0.4.1.
- GCC `-ffile-prefix-map` нормализует `$PROJECT_DIR` и `$PROJECT_CORE_DIR` между
  локальной машиной и GitHub runner. Строгая CI-проверка build/app byte equality
  теперь воспроизводима без изменения размеров или SHA-256 release binaries.

Подробности: [`docs/OBD_DTC_DIAGNOSTICS_DESIGN.md`](docs/OBD_DTC_DIAGNOSTICS_DESIGN.md),
[`docs/CI_REPRODUCIBLE_BUILD_2026-09-30.md`](docs/CI_REPRODUCIBLE_BUILD_2026-09-30.md)
и последующий аудит [`docs/CODE_AUDIT_0.4.1_2026-09-30.md`](docs/CODE_AUDIT_0.4.1_2026-09-30.md).

## 0.4.0 — 2026-09-28 (отозван, не устанавливать)

Историческая запись исходного пакета поверх аппаратно принятого 0.3.9.
Поводом стало наблюдение мигающей лампы Check Engine при высокой скорости
2026-09-27. Бинарник 0.4.0 не устанавливался на Haval и после review был
отозван; исправления выпущены как 0.4.1 без замены прежних hashes.

### Check Engine и чтение DTC

- В polling добавлен Mode 01 PID 01 каждые 500 мс в движении: текущая MIL,
  confirmed DTC count и RAM-latch любого увиденного MIL ON до перезапуска.
- При первом ECU, переходе MIL и изменении DTC count асинхронно читаются stored
  Mode 03, pending Mode 07 и permanent Mode 0A; здоровое/аварийное периодическое
  перечитывание ограничено 300/60 секундами.
- Bounded transport поддерживает single/multi-frame ISO-TP, physical engine ECU,
  один outstanding transaction, 96-байтный response buffer, до 32 кодов,
  sequence checks, timeout, malformed и negative response/NRC.
- DTC преобразуются в `P/C/B/Uxxxx`; встроены осторожные расшифровки части
  generic-кодов. Неизвестные и Haval-specific значения не интерпретируются
  догадками. `P0300..P0312` отдельно классифицируются как misfire.
- Добавлена bounded history до 32 transient/changed кодов с source ECU,
  seen/last-present category masks, occurrence count и change sequence. Она
  сохраняется только при изменении через отдельные CRC/read-back LittleFS A/B
  journal (20 с) и NVS mirror (60 с) и восстанавливается после reboot.

### Экран и web service

- Главная status row приоритетно показывает красные `ПРОПУСКИ P03xx!` или
  `CHECK ENGINE`, либо жёлтый DTC без MIL; предупреждение доступно на основных
  страницах, а служебная физическая страница показывает сводку 03/07/0A.
- Web OBD получил список codes/type/ECU/description, текущий и latched MIL,
  category state, scan progress, NRC/error, асинхронное ручное чтение и
  отдельную таблицу current/historical DTC с состоянием CRC/LittleFS/NVS.
- Добавлены `GET /api/diagnostics/dtc`, `POST .../scan` и `POST .../clear`;
  синхронный WebServer не блокируется в ожидании ECU.

### Защищённый Mode 04

- Стирание никогда не автоматическое и требует action header, точного JSON
  confirmation, acknowledgement сброса readiness/freeze-frame и cooldown 60 с.
- Каждый clear сначала принудительно выполняет fresh Mode 03/07/0A. Timeout,
  malformed/transport error или truncation любой категории запрещает Mode 04.
- После успешного pre-scan state machine останавливается до немедленного
  read-back-проверенного history checkpoint; отказ обеих durable-копий запрещает
  дальнейшие запросы. Затем физически перечитываются speed PID 0D = 0,
  RPM PID 0C < 50 и ECU voltage PID 42 = 11,5..16,5 В. Движение, работающий
  двигатель, unsafe voltage, malformed или negative response отменяют команду.
- Positive `0x44` очищает локальные stored/pending и запускает контрольное
  чтение через 1,5 с. Permanent Mode 0A не заявляются как стёртые.

### Проверки

- Добавлены десять ASan/UBSan DTC host-групп: formatting, MIL latch, ECU
  discovery, single/multi-frame 03/07/0A, bounded truncation, transient
  history/restore, negative/timeout, mandatory pre-clear scan/preservation и
  success/refusal Mode 04.
- Storage recovery suite проверяет fixed LE/CRC history record, dirty-only
  20/60 mirror, reboot, torn journal/NVS recovery и dual-storage failure.
- Static gate связывает PID 01, transport, durable checkpoint, dashboard, API,
  confirmations и embedded UI; browser fixture проверяет current/historical
  DTC, storage health, P0301/misfire, scan header и точное clear body.
- PlatformIO `esp32s3_n16r8` успешно собран: internal RAM 53 316 байт; app
  Flash payload 1 187 797 байт; app binary 1 188 208 байт; manifest
  `0.4.0 / esp32s3-n16r8` найден по offset `0x1c280`.
- Упакованы обычный OTA app `h2-gauge-v0.4.0-esp32s3-n16r8.bin`
  (`e5cf27be…44a87`, 1 188 208 байт) и factory image (`cc140502…c6388`,
  1 253 744 байта); проверены SHA-256, byte equality и merged offsets.
- На момент исходной упаковки в `releases/` были четыре файла 0.4.0;
  при выпуске 0.4.1 они удалены по правилу «только актуальная версия».

Подробности: [`docs/OBD_DTC_DIAGNOSTICS_DESIGN.md`](docs/OBD_DTC_DIAGNOSTICS_DESIGN.md).

## 0.3.9 — 2026-09-22

Следующий кандидат поверх неизменённых бинарных артефактов 0.3.8. Добавлены
пользовательские визуальные ресурсы и software-only persistence при внезапном
отключении питания. Аппаратное acceptance 0.3.7→0.3.8 остаётся отдельным gate;
0.3.9 также не считался аппаратно подтверждённым до проверки на N16R8.

### Аппаратный OTA-результат — 2026-09-23

- Обычный app 0.3.9 успешно установлен штатным web OTA из работающей 0.3.7.
- Source был APP1/0.3.7, target после автоматического reboot — APP0/0.3.9.
- Сервис подтвердил current/running/boot = 0.3.9 в APP0, APP1 = 0.3.7,
  running image state = `valid`.
- Физически пройден 332-байтный финальный raw-фрагмент без ручного RESET/TWDT.
- Custom assets и abrupt-power-loss persistence остаются отдельными аппаратными gates.

### Пользовательский фон и логотип

- Web UI принимает локальные PNG/JPEG/WebP, ограничивает тип, размер и число
  декодированных пикселей, показывает preview и преобразует изображение в
  little-endian RGB565.
- Фон центрируется/crop/darken до строгих `240×240` и `115 200` байт; логотип
  пропорционально вписывается в `220×80` и имеет ровно `width×height×2` байт.
- ESP32 проверяет metadata, точный HTTP `Content-Length`, размер payload, CRC32,
  полный file read-back и только затем переключает CRC-защищённый A/B manifest.
- Raw-body transport не использует multipart, кормит TWDT, имеет 2-секундный
  idle timeout и 30-секундный absolute deadline даже при byte trickle.
- Dashboard читает ресурсы один раз при старте в PSRAM/current cache. Встроенные
  carbon background и HAVAL logo остаются безусловным fallback; app-only OTA
  не затрагивает LittleFS.

### Persistence при обрыве питания

- Введён канонический 140-байтный snapshot trip + petrol calibration с
  magic/schema/monotonic sequence и внешним CRC32.
- Dirty-only append выполняется каждые 20 секунд в два LittleFS-сегмента примерно
  по 256 КиБ; append+flush+read-back, torn-tail recovery и безопасная A/B-ротация
  не требуют раннего `POWER_FAIL` или hold-up питания.
- Sequence-bearing NVS mirror обновляется каждые 60 секунд. При старте выбирается
  новейшая валидная запись LittleFS/NVS; миграция `h2trip`/`h2petcal` сохранена.
- Reset, старт/завершение калибровки, вход/выход сервиса, low voltage, reboot и
  остановка двигателя форсируют checkpoint; legacy NVS stores также обновляются
  для downgrade-совместимости.
- Непустой не монтируемый LittleFS никогда не форматируется автоматически;
  форматирование допускается только при доказанно полностью стёртом разделе.
- System UI показывает mount/source/sequence/segment/CRC/tail/write age/counters
  и failures. Нормальное окно потери — 0–20 секунд, NVS fallback — 0–60 секунд.

### Проверки

- Добавлены ASan/UBSan host-тесты интервалов, dirty-only writes, newest-source
  selection, torn tail, corrupt CRC, NVS repair, monotonic factory reset и A/B
  asset upload/read-back/fallback.
- Browser fixture проверяет source validation, background/logo conversion,
  payload byte count/CRC, raw headers/body, enable/delete и persistence panel.
- Static gate связывает размеры/CRC/A-B/fallback/LittleFS policy с embedded UI;
  PlatformIO N16R8, OTA, brightness, fonts и все прежние gates сохранены.
- Финальный app `1 159 184` байта (`SHA-256 4eddc675…73c023`) и factory
  `1 224 720` байт (`SHA-256 08917e02…7128bc`) проверены по manifest,
  byte equality, offsets/FF gaps и `SHA256SUMS-v0.3.9.txt`.

Подробности: [`docs/CUSTOM_VISUAL_ASSETS_DESIGN.md`](docs/CUSTOM_VISUAL_ASSETS_DESIGN.md)
и [`docs/POWER_LOSS_PERSISTENCE_DESIGN.md`](docs/POWER_LOSS_PERSISTENCE_DESIGN.md).

## 0.3.8 — 2026-09-22

Отдельный OTA-hardening кандидат для ESP32-S3 DevKitC-1 / N16R8. Аппаратно
подтверждённый 0.3.7 сохранён как исходная точка; 0.3.8 требует отдельной
проверки на приборе.

### OTA transport и API

- Добавлен абсолютный 180-секундный deadline всей raw OTA-сессии, который не
  продлевается медленным trickle; двухсекундный idle watchdog сохранён.
- `RAW_ABORTED` теперь различает idle/total timeout, disconnect и отказ handler,
  очищает OTA state, возвращает структурированный JSON при живом socket и после
  completion handler явно закрывает TCP.
- Добавлены `/api/ota/preflight` и `/api/ota/status`: браузер до полного binary
  POST передаёт точный metadata probe, а сервер проверяет имя, размер, ESP32-S3
  chip/app descriptor, движение, питание и реальный неактивный раздел.
- Ошибки содержат стабильный `code`, HTTP status, received/expected byte counts,
  timeout и при необходимости `Retry-After`; после сетевого сбоя UI восстанавливает
  конкретную причину через status endpoint.
- Во время OTA подавлены фоновые status/fuel/CAN polls; UI использует полученные
  от устройства `maxImageBytes`, `probeBytes` и timeout.

### Надёжность метаданных

- Начальный probe имеет точный compile-time размер ESP image header + segment
  header + app descriptor и может собираться из любого числа transport chunks.
- SDK-структуры заполняются выровненным `memcpy`; остаток блока, завершившего
  probe, сразу записывается без потери. Копирование descriptor version ограничено
  вместимостью destination.
- Native verify/select worker ограничен минимумом своего 30-секундного лимита и
  оставшегося общего OTA deadline. TWDT не отключается, multipart не возвращён.

### Проверки

- Расширен static transport gate: абсолютный timeout с `millis()` rollover,
  abort dispatch/close, metadata preflight и модель probe больше 1 436 байт.
- Browser fixture проверяет dynamic size limit, отказ несовместимого probe до
  binary POST, JSON error, network failure + status recovery и verified install.
- Полное описание решения и аппаратный чек-лист:
  [`docs/OTA_HARDENING_0.3.8.md`](docs/OTA_HARDENING_0.3.8.md).

## 0.3.7 — 2026-09-20

Аппаратно подтверждённый релиз для ESP32-S3 DevKitC-1 / N16R8.

### OTA

- Web OTA принимает обычный raw app `.bin` (`application/octet-stream`).
- Project-local WebServer 2.0.17 читает точный остаток `Content-Length`, кормит
  TWDT внутри receive-loop и ограничивает отсутствие прогресса двумя секундами.
- Сохранены прямые ESP-IDF write/end/select, проверка образа, boot read-back,
  rollback confirmation и включённый watchdog.
- Journal v3 различает `receiving`, `verifying`, `image_verified`,
  `boot_selected` и сохраняет receive progress/reset/native error.
- Реальный OTA APP0→APP1 завершён с автоматическим reboot; APP1/0.3.7 имеет
  состояние `valid`. Неполный последний raw-фрагмент — 768 байт.

### Интерфейс и устройство

- Физический и web-сервис показывают текущую сборку и версии APP0/APP1.
- Добавлены плавная адаптивная яркость GPIO6/GPIO7, ручной День/Ночь,
  автокалибровка диапазона и четыре режима яркости.
- Встроены сглаженные Golos UI / Golos Text.
- Сохранены текущие CAN/OBD, LPG, fuel/trip, power/deep-sleep и Mode 22 решения.

### Проверки

- 25 основных host regression-групп.
- 11 OTA journal/migration-групп с ASan/UBSan.
- Static transport, manifest, fonts, embedded UI, checksums и factory-layout.
- Browser fixture для slot cards, OTA phases, brightness и responsive layouts.
- CI зафиксирован на Ubuntu 24.04, PlatformIO 6.1.18 и проверенных версиях
  библиотек; host-заглушка `strlcpy()` совместима с fortified glibc 2.38+.

Подробный OTA postmortem: [`docs/OTA_POSTMORTEM_2026-09-19.md`](docs/OTA_POSTMORTEM_2026-09-19.md).
Анализ CI: [`docs/CI_POSTMORTEM_2026-09-20.md`](docs/CI_POSTMORTEM_2026-09-20.md).
