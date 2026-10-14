# H2 Gauge 0.3.8 — усиление web OTA

**Дата решения:** 2026-09-22  
**Цель:** ESP32-S3 DevKitC-1 / ESP32-S3-WROOM-1-N16R8  
**Статус:** отдельный кандидат 0.3.8; аппаратно подтверждённый 0.3.7 остаётся исходной точкой и не переименовывается задним числом.

## Зачем нужен отдельный релиз

В 0.3.7 на реальном приборе доказано исправление последнего неполного raw-фрагмента: обычный app `.bin` прошёл APP0→APP1, `esp_ota_end()`, выбор boot-раздела и автоматический reboot. Однако прежний raw watchdog ограничивал только паузу между байтами. Клиент, который постоянно передаёт очень мало данных, мог никогда не превысить двухсекундный idle timeout и бесконечно удерживать один синхронный `WebServer::handleClient()`.

0.3.8 не заменяет и не переписывает результат 0.3.7. Это новый кандидат со следующими дополнительными гарантиями:

1. один абсолютный deadline всей операции OTA;
2. явная обработка `RAW_ABORTED` с очисткой состояния и закрытием TCP;
3. структурированные ошибки для браузера и отдельный status recovery;
4. серверный preflight до передачи полного app image;
5. буфер метаданных, не зависящий от размера сетевого блока и фиксированного предположения о размере структур ESP-IDF.

## Протокол браузера

### 1. Получение возможностей

Перед установкой браузер запрашивает:

```http
GET /api/ota/status
```

Ответ содержит как минимум:

- `minImageBytes` и фактический `maxImageBytes` неактивного OTA-раздела;
- `probeBytes` — точное число начальных байтов, необходимое этой сборке ESP-IDF;
- `idleTimeoutMs` и `timeoutMs`;
- состояние последней попытки, machine-readable `code`, текст ошибки;
- `receivedBytes`, `writtenBytes`, `expectedBytes` и elapsed time.

UI использует эти значения вместо жёстко прошитого четырёхмегабайтного предела. Во время OTA фоновые `/api/status`, fuel-calibration и CAN polls подавляются, чтобы единственный синхронный сервер не получал конкурирующие запросы.

### 2. Metadata preflight

Браузер читает только первые `probeBytes` выбранного локального файла и отправляет JSON:

```http
POST /api/ota/preflight
X-H2G-Action: ota
Content-Type: application/json

{
  "filename": "h2-gauge-v0.3.8-esp32s3-n16r8.bin",
  "size": 1090000,
  "probeHex": "e9..."
}
```

Сервер **до передачи полного образа** проверяет:

- наличие `.bin` и отсутствие признаков `factory`, `bootloader`, `partition`, `merged`, `spiffs`, `littlefs`;
- минимальный размер и вместимость реально выбранного неактивного раздела;
- отсутствие параллельной OTA и 30-секундный cooldown;
- известную скорость автомобиля не выше 3 км/ч;
- известное свежее напряжение ECU не ниже 11,3 В;
- source/target OTA subtype и то, что target физически отличается от running partition;
- точный размер metadata probe;
- ESP image magic, допустимое число сегментов, chip ID `9` (ESP32-S3) и `ESP_APP_DESC_MAGIC_WORD`.

Preflight не открывает OTA handle, ничего не стирает и не расходует cooldown попытки. Actual raw body снова проходит те же filename/size/partition проверки, а его реальные начальные байты повторно проверяются. Поэтому подмена preflight-данных не позволяет записать другой образ.

### 3. Передача app image

Только после `accepted:true` браузер отправляет сам файл:

```http
POST /api/ota
Content-Type: application/octet-stream
X-H2G-Action: ota
X-H2G-Filename: h2-gauge-v0.3.8-esp32s3-n16r8.bin
```

Multipart не возвращается. Factory image, bootloader, partition table и filesystem image через web OTA не принимаются.

## Два независимых ограничения времени

### Idle watchdog: 2 000 мс

`H2PatchedWebServer` читает только реально доступные socket-байты, запрашивает не больше точного остатка `Content-Length` и кормит TWDT на каждом проходе. Если прогресс отсутствует две секунды, parser создаёт `RAW_ABORT_IDLE_TIMEOUT`.

### Абсолютный deadline: 180 000 мс

Отдельный deadline начинается при входе raw parser и не продлевается каждым новым байтом. Проверка использует беззнаковое `millis() - startedAt`, поэтому корректна при rollover. Медленный клиент, посылающий хотя бы один байт каждые 1,9 секунды, всё равно будет остановлен не позднее общего трёхминутного предела.

Тот же предел действует на серверную OTA-сессию до завершения проверки и выбора boot-раздела. Native worker получает минимум из оставшегося общего времени и собственного 30-секундного лимита. TWDT не отключается.

## Семантика abort

В vendored WebServer добавлены причины:

- `RAW_ABORT_IDLE_TIMEOUT`;
- `RAW_ABORT_TOTAL_TIMEOUT`;
- `RAW_ABORT_DISCONNECTED`;
- `RAW_ABORT_HANDLER` — приложение отвергло запрос и не хочет дочитывать body.

При abort parser:

1. передаёт частичный прочитанный блок в `RAW_WRITE`, если он есть;
2. вызывает ровно один `RAW_ABORTED` с причиной и elapsed time;
3. позволяет route completion handler сформировать JSON;
4. после ответа явно вызывает `WiFiClient::stop()`.

`ServicePortal::failOta()` закрывает открытый ESP-IDF OTA handle, сбрасывает `otaAllowed_`, `otaInProgress_`, success/read-back flags и сохраняет код ошибки с byte counters. Счётчики намеренно не обнуляются, чтобы `/api/ota/status` мог объяснить сетевой сбой. Если исходный socket уже разорван, браузер после `onerror` запрашивает status отдельным соединением.

Типичный JSON ошибки:

```json
{
  "ok": false,
  "status": 408,
  "code": "idle_timeout",
  "error": "No OTA upload progress for 2 seconds",
  "receivedBytes": 2048,
  "expectedBytes": 1090000,
  "timeoutMs": 180000
}
```

Для `429` также возвращаются `retryAfterSeconds` и HTTP `Retry-After`.

## Независимый metadata probe

Размер probe вычисляется при компиляции:

```cpp
sizeof(esp_image_header_t) +
sizeof(esp_image_segment_header_t) +
sizeof(esp_app_desc_t)
```

Массив имеет ровно этот размер. Каждый raw transport block делится на:

1. недостающую часть probe;
2. остаток того же блока.

Когда probe заполнен, структуры копируются через `memcpy` в локальные естественно выровненные объекты; cast невыравненного byte buffer запрещён. После проверки сначала записывается probe, затем без потери записывается остаток текущего блока. Алгоритм одинаково работает, если будущий SDK сделает probe меньше, равным или больше 1 436-байтного сетевого блока. Копирование строки descriptor также ограничено вместимостью destination.

## Сохранённые обязательные проверки

0.3.8 не убирает ни одну гарантию 0.3.7:

- запись только в неактивный OTA slot через `esp_ota_begin/write/end`;
- точное равенство Content-Length, received и written;
- ESP header/chip/app descriptor validation;
- checksum/hash validation внутри `esp_ota_end()`;
- durable phases `receiving`, `verifying`, `image_verified`, `boot_selected`;
- progress checkpoint каждые 256 КиБ;
- явный `esp_ota_set_boot_partition(target)` и read-back физического адреса;
- восстановление source selection при mismatch;
- startup rollback confirmation;
- диагностика APP0/APP1 и H2 firmware manifest;
- автоматический reboot только после полного HTTP success response;
- включённый loop TWDT и отключённый Wi-Fi power save в service mode;
- явное отклонение multipart.

## Стабильные коды ошибок

Основные коды API:

- `confirmation_required`, `invalid_content_type`;
- `invalid_preflight`, `preflight_too_large`, `invalid_probe`, `invalid_image`;
- `invalid_filename`, `invalid_size`, `image_too_large`;
- `ota_busy`, `rate_limited`, `vehicle_moving`, `low_voltage`;
- `no_target_partition`, `ota_begin_failed`, `flash_write_failed`;
- `length_overflow`, `incomplete_image`;
- `idle_timeout`, `total_timeout`, `client_disconnected`, `upload_aborted`;
- `image_verification_failed`, `boot_selection_failed`, `boot_readback_mismatch`.

Текст предназначен человеку, а `code` — browser test, log collector или будущему клиенту API.

## Проверки кандидата

Обязательный программный gate:

1. 25 host regression-групп brightness/input/config с ASan/UBSan;
2. 11 OTA journal/migration-групп с ASan/UBSan;
3. static transport check для exact final chunk, rollover, idle/absolute deadlines, abort dispatch, preflight и probe splitter, включая модель будущего probe больше одного network chunk;
4. Playwright fixture: dynamic partition limit, metadata preflight rejection **до binary POST**, structured HTTP error, network failure + `/api/ota/status`, успешное verified/read-back подтверждение;
5. deterministic `web/index.html` → gzip → `src/web_ui_gz.h`;
6. проверка Golos assets;
7. clean PlatformIO build `esp32s3_n16r8` на pinned `espressif32@6.8.1` / Arduino-ESP32 2.0.17;
8. H2 manifest 0.3.8, byte-identical packaged app, merged factory offsets/FF gaps и SHA-256 manifest.

## Что ещё требует физического подтверждения

Кандидат нельзя называть аппаратно принятым до проверки на настоящем N16R8:

1. войти в сервис на работающем 0.3.7;
2. выбрать обычный app `h2-gauge-v0.3.8-esp32s3-n16r8.bin`;
3. увидеть успешный metadata preflight;
4. дождаться 100%, server verification, boot read-back и автоматического reboot;
5. подтвердить current/running/boot = 0.3.8 в новом slot, прежний 0.3.7 в противоположном slot и running image state `valid`;
6. убедиться, что настройки schema 6, trip, fuel и brightness calibration сохранены;
7. отдельно при возможности проверить controlled abort медленного/оборванного клиента и последующий status recovery.

До этой процедуры формулировка результата: **«0.3.8 программно проверенный кандидат; 0.3.7 аппаратно подтверждённый релиз»**.
