# H2 Gauge 0.1.9 — решения и реализация

Дата: 2026-09-08. Статус: программная реализация завершена; автомобильная валидация Haval ещё требуется.

## Low-voltage policy

`PowerManager` принимает решение только по свежему Mode 01 PID 42. Отсутствующий или устаревший PID не интерпретируется как низкое напряжение. Условие сна — напряжение строго ниже 11.5 В непрерывно 3000 мс.

Перед deep sleep выполняется:

1. принудительный `TripStore::save()` независимо от обычного флага периодического сохранения;
2. остановка и деинсталляция TWAI;
3. освобождение 8-bit framebuffer;
4. LEDC duty подсветки = 0;
5. команда `TFT_DISPOFF` и GPIO26/RST = LOW;
6. удержание RTC GPIO26 в LOW на время сна;
7. отключение Wi‑Fi;
8. включение timer wakeup 30 секунд и EXT0 wakeup по GPIO32 LOW.

После пробуждения hold снимается до инициализации TFT, затем GPIO26 немедленно возвращается в LOW. Цикл 30 секунд выбран, чтобы устройство могло автоматически вернуться в работу после восстановления напряжения, не требуя нажатия кнопки.

Программный контроль не заменяет аппаратные fuse/reverse-polarity/TVS/filter/buck/UVLO. Полная схема и критерии выбора приведены в `docs/wiring.md`.

## TWAI recovery

Активированы alerts `BUS_OFF`, `BUS_RECOVERED`, `RX_QUEUE_FULL`, `TX_FAILED` и `TWAI_ALERT_AND_LOG`.

Сохраняется штатная последовательность ESP-IDF:

```text
BUS_OFF → twai_initiate_recovery()
BUS_RECOVERED → twai_start()
```

Не используется несуществующая для данного API повторная `twai_init()`. Добавлен watchdog состояния: через 2 секунды он проверяет `twai_get_status_info()` и повторяет допустимый переход для `BUS_OFF` или `STOPPED`, а состояние `RUNNING` снимает программный recovery latch. При deep sleep вызывается отдельный `ObdClient::shutdown()`.

## Adaptive Mode 01 polling

Добавлены:

- PID 11 — absolute throttle position;
- PID 06 — STFT bank 1, `(A-128)*100/128`;
- PID 07 — LTFT bank 1, `(A-128)*100/128`.

Планировщик переведён на round-robin, чтобы быстрые MAP/RPM PID не вытесняли диагностические PID при общем ограничении запросов. Когда свежая скорость ниже 1 км/ч, интервалы быстрых PID увеличиваются минимум до 1000 мс; PID 42 остаётся 1 Гц для корректного voltage policy. В движении используются исходные интервалы 100–500 мс в пределах общего `maxRequestsPerSecond`.

Polling 06/07 и 11 можно независимо отключить настройками `fuelTrimEnabled` и `dfcoEnabled`. Флаги упакованы в свободные биты поля `startPage`; размер `ConfigData` не увеличился.

## DFCO

Если DFCO включён, PID 11 свежий, throttle ≤1% и RPM >1000, расчётный raw fuel rate принудительно становится 0 л/ч. Это устраняет ложный расход MAF во время торможения двигателем, когда поток воздуха есть, но форсунки отключены. Эвристика применяется к расчётному бензину и LPG и отображается в `/api/status` как `dfcoActive`.

Условие намеренно консервативно и требует проверки на Haval по реальным логам throttle/RPM/Fuel Rate.

## STFT + LTFT и LPG

При включённом LPG и свежих PID 06/07 вычисляется сумма STFT+LTFT. Если абсолютное значение непрерывно выше 10% не менее 5 секунд, появляется `КАЛИБРОВКА ГБО!` / `CHECK LPG CALIBRATION` в нижней строке прибора. Таймер сбрасывается при возврате в диапазон, устаревании PID, выключении двигателя или переходе на бензин.

Это предупреждение не является автоматической командой менять карту BRC. Перед калибровкой необходимо исключить подсос воздуха, неисправности датчиков и проблемы бензиновой системы.

## Mode 22 framework

Добавлены `Mode22DidDefinition` и `Mode22Transport`:

- физический UDS ReadDataByIdentifier request `0x22`;
- 11-bit или 29-bit request/response ID в definition;
- single-frame positive response `0x62`;
- ISO-TP first/consecutive frame reassembly до 48 payload bytes;
- Flow Control CTS;
- round-robin interval и decoder callback;
- timeout/reset интегрированы в OBD transaction loop.

Production-конфигурация в `src/mode22_config.cpp` содержит `nullptr` и count 0. Поэтому 0.1.9 **не отправляет ни одного Mode 22 запроса**. DID, request IDs и формулы должны добавляться только после документированного захвата на целевом Haval.

## REST CAN Monitor

`CanMonitor` хранит фиксированный массив из 32 агрегатов. Ключ — CAN ID, extended/RTR flags и направление RX/TX. Для каждого ключа сохраняются только последний payload, DLC, count и время последнего кадра. При заполнении используется LRU-вытеснение. Не создаётся неограниченная очередь raw frames.

Endpoints:

```text
GET  /api/can/snapshot
POST /api/can/clear
```

Ограничения безопасности/нагрузки:

- глобальный REST rate limit — один snapshot за 500 мс, иначе HTTP 429;
- web auto-refresh — 1 секунда;
- максимум 32 строки и около 6 KiB зарезервированного response String;
- TWAI RX обрабатывается порциями максимум по 64 кадра за loop;
- в service mode активные OBD requests приостановлены, но TWAI RX продолжает работать;
- endpoint не позволяет отправлять произвольные кадры.

Выбран REST snapshot, а не WebSocket/SSE, чтобы ограничить RAM, CPU и Wi‑Fi load и сохранить предсказуемое пассивное поведение.

## Архитектура задач

Приложение остаётся однопоточным cooperative loop. Отдельные FreeRTOS CAN/telemetry/UI tasks не добавлялись, поэтому `xQueueCreate` также не добавлялся: очередь без реального разделения на конкурентные задачи не устраняет гонку и создаёт лишнюю сложность. Если задачи появятся позднее, общие snapshots и ownership TWAI необходимо пересмотреть.

WebServer синхронный, но `CanMonitor` всё равно защищён `portMUX`, чтобы snapshot и record имели явную атомарную границу и класс оставался безопасным при будущем переносе callbacks/receive в разные tasks.

## Logging

Собственные `Serial.print*`/`Serial.printf` заменены на `ESP_LOGI`, `ESP_LOGW` и `ESP_LOGE` с tags `MAIN`, `OBD`, `POWER`, `UI`, `WEB`. `Serial.begin(115200)` сохранён как backend вывода логов на ESP32-WROOM/CP2102.

Native USB flags не добавлены: классический ESP32-WROOM не имеет native USB. Их применимость к S2/S3/C3 описана в wiring/troubleshooting.

## NVS migration

Версия конфигурации повышена с schema 2 до schema 3 без изменения размера структуры. При валидной schema 2 сохраняются все поля, включаются новые DFCO/fuel-trim defaults, прежний default auto-return 10 секунд при необходимости мигрируется в 5, затем checksum записывается уже для schema 3. После миграции пользователь может независимо отключить оба новых флага; повторно они не включаются.

## Документация и CI

Добавлены:

- `docs/wiring.md` — предохранитель, переполюсовка, TVS/load dump, LC/π filter, wide-input automotive buck, CAN, RST 10 кОм, подсветка через ключ, GPIO32 wake и LPG input;
- `docs/troubleshooting.md` — белый экран, нулевые данные, demo flag, 120 Ом, CAN Monitor, Mode 22, trims, deep sleep, AP/NVS и различие reset линий;
- `.github/workflows/platformio.yml` — `pio run -e esp32dev` на push и pull request с публикацией firmware artifact.

## Итог сборки

PlatformIO `esp32dev`, espressif32 6.8.1:

```text
RAM:   48 540 / 327 680 bytes (14.8%)
Flash: 987 481 / 1 572 864 bytes (62.8%)
```

Release app image: `releases/h2-gauge-v0.1.9-esp32.bin`, размер 994 192 байта, SHA-256 `86d01b6aec6c0f8c39b70e18e25b3f34576ec0032d1c8dab9f8e70d9fd4d77d6`.

Проверены: PlatformIO build, JavaScript syntax production/mockup, JSON example, gzip round-trip embedded web UI, отсутствие собственных `Serial.print*` и native-USB flags в build configuration.

## Требуемая автомобильная валидация

1. Проверить PID 42 мультиметром и безопасно испытать порог на регулируемом лабораторном питании, а не разрядом аккумулятора.
2. Проверить wake timer и GPIO32, отсутствие вспышки TFT и фактический ток deep sleep всей платы.
3. Создать bus-off на изолированном стенде, подтвердить recovery без зацикливания.
4. Записать поддержку/значения PID 11/06/07 и сравнить throttle с реальным закрытым положением.
5. Сравнить DFCO с PID 5E/временем впрыска.
6. Сравнить trims на бензине и LPG после прогрева.
7. Проверить REST snapshot при загруженной шине и использование heap.
8. Не активировать Mode 22 до подтверждения request/response IDs и DID.
