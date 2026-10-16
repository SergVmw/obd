# H2 Gauge

Круглый автомобильный информационный прибор для **Haval H2 2018 1.5T** с ГБО **BRC Sequent 32 EVO OBD**.

## Текущая аппаратная цель

- MCU: **ESP32-S3 DevKitC-1 compatible / ESP32-S3-WROOM-1-N16R8**;
- память: 16 МБ QSPI Flash + 8 МБ OPI PSRAM;
- дисплей: GC9A01 240×240;
- CAN: встроенный TWAI + внешний 3.3-вольтовый SN65HVD230 рядом с ESP32-S3;
- OBD: ISO 15765-4, 11-bit, начальная скорость 500 кбит/с;
- LPG: защищённый вход клапана через PC817/EL817 на GPIO5;
- управление: одна кнопка GPIO4;
- конфигурация: локальный Wi‑Fi service portal;
- сборка: PlatformIO environment `esp32s3_n16r8`.

## Текущий production-релиз 0.4.10

Прибор перезагружался при переходе на вкладку «Диагностика». Причина не в
вкладке: все обработчики портала идут в задаче `loopTask` со стеком 8192 байт, а
обработчик отчёта о поддерживаемых PID держал 12 КБ на стеке. Буфер больше стека
перезагружает плату, но только на том маршруте, который его обслуживает, поэтому
остальные двадцать девять выглядели исправными. Буфер вынесен из стека, а
`check_stack_budget.py` теперь запрещает такие локальные буферы в любом
обработчике.

Экспериментальные образы выровнены на текущую версию: они отстали на три
релиза, и образ, собранный сегодня, объявлял бы версию, которой в нём нет.

Отчёт: [`releases/README-v0.4.10.md`](releases/README-v0.4.10.md).

## Предыдущий production-релиз 0.4.8 (оставлен для отката)

Запись поездки содержала неинициализированные байты структуры — padding после
`checksum`, который `encode()` копировал вместе со всем остальным. Из-за этого
сравнение не узнавало неизменную поездку, и журнал дописывал запись каждые
20 секунд к состоянию, которое не двигалось. `encode()` копирует теперь только
определённую часть структуры; размер записи прежний.

VIN оставался пустым на всю поездку: две попытки по четыре секунды против
автомобиля, который едет, — недостаточно. Дальше запрос идёт раз в 30 секунд,
но только на стоянке, по той же полосе скорости, что выбирает профиль опроса.

Маски поддержки PID больше не собираются в никуда: в портале появился отчёт,
какие блоки отвечают на шине и какие параметры поддерживает каждый.

**Потеря данных при обновлении:** нет, схема записи не менялась, поездка
переживает обновление.

Опубликованный 0.4.7 не переписан: правки вышли отдельной версией.

Отчёт: [метка `v0.4.8`](https://github.com/SergVmw/obd/blob/v0.4.8/releases/) — из рабочего дерева удалён.

## Релиз 0.4.7 (оставлен для отката)

Первый запрос VIN мог пропасть, когда шина ещё занята, а блок двигателя не
проснулся, — и тогда VIN не появлялся до перезагрузки. Добавлен один повтор
через четыре секунды и не более: идентификатор при работающем двигателе не
меняется, поэтому вторая неудача не превращается в опрос.

Харнесс ObdClient собирался без `-Werror` и пропускал предупреждение о
переменной, которая в production-сборке вычислялась и выбрасывалась. Чтение
перенесено внутрь диагностической сборки, `-Werror` включён и проверен.

Известное ограничение 0.4.7 — ретрай VIN не помогает, если прибор загрузился в
движении; исправлено в 0.4.8.

**Потеря данных при обновлении:** нет, схема записи не менялась.

Отчёт: [метка `v0.4.7`](https://github.com/SergVmw/obd/blob/v0.4.7/releases/) — из рабочего дерева удалён.

## Релиз 0.4.6 (оставлен для отката)

Частоту запросов научились сохранять, VIN научились читать: запрос шёл как
Mode 01 PID 09 вместо Service 09 PID 02, проверка ждала форму кадра, которую не
отправляет ни один ЭБУ. Известный дефект 0.4.6 — потерянный первый запрос VIN
не восстанавливался до перезагрузки; исправлено в 0.4.7.

**Потеря данных при обновлении:** нет, схема записи не менялась.

Отчёт: [метка `v0.4.6`](https://github.com/SergVmw/obd/blob/v0.4.6/releases/) — из рабочего дерева удалён.

## Релиз 0.4.5 (артефакты удалены, доступны по метке `v0.4.5`)

Экран «ГБО: коррекции», VIN, вкладка «Диагностика» с выгрузкой JSON, удаление
временной яркости и усреднение крупного расхода за минуту. Отдельно
исправлена потеря счётчиков топлива простоев: она была бесшумной и обесценивала
ДВИЖЕНИЕ и долю простоев после каждого цикла зажигания.

Стоимость простоя переведена на время простоя, маски поддержки перечитываются
каждые две минуты, экран диагностики различает ЭБУ двигателя и просто
ответивший блок, потолок запросов расширен до 60.

**Два известных дефекта 0.4.5,** оба исправлены в 0.4.6: чтение VIN было
нерабочим (запрос Mode 01 PID 09 вместо Service 09 PID 02) и частоту запросов
нельзя было сохранить выше 40 при том, что инструкция велит выставить 50.

**Потеря данных при обновлении:** запись выросла со 156 до 164 байт, поездка
обнулится один раз.

Отчёт и артефакты: [метка `v0.4.5`](https://github.com/SergVmw/obd/blob/v0.4.5/releases/) — из рабочего дерева удалены.

## Релиз 0.4.4 (артефакты удалены, доступны по метке `v0.4.4`)

Цель — только ESP32-S3 DevKitC-1 N16R8. **0.4.4** — это 0.4.3 плюс пять исправлений, найденных на автомобиле. Расход в 0.4.3 не появлялся вообще: `observeEngineResponse` защёлкивал первый ответивший блок на PID `0xC`, а в поле это оказался блок АКПП `0x7E9`, тогда как двигатель `0x7E8` ответил 1428 раз и все ответы были отброшены. Теперь маски поддержки объединяются по всем ответившим блокам, и сигнал двигателя берётся у того, кто его реально имеет; PID `01` (MIL и счётчик неисправностей) исключён намеренно, чтобы лампа Check Engine не следовала за блоком АКПП.

Остальные четыре исправления: коррекция скорости больше не применяется ниже полосы стояния, поэтому на месте перестали выдумываться километры и 45.5 л/100 км; топливо, сгоревшее без пробега, вынесено из общего счётчика, и экран расхода показывает СРЕДНИЙ, ДВИЖЕНИЕ и долю ПРОСТОИ отдельно; интервал накопления отбрасывается при неизвестной скорости, а не копит литры при неподвижном знаменателе; диагностическая сборка `esp32s3_n16r8_diagnostic` снова собирается после потерянной экранированной кавычки в `H2G_BUILD_TARGET`.

Потери интервалов теперь считаются и отдаются в `/api/status` → `droppedIntervals`. Профиль опроса суммарно требует около 50 запросов в секунду, поэтому рекомендованный `maxRequestsPerSecond` поднят до **50**.

**Потеря данных при обновлении:** размер `TripState` вырос с 64 до 80 байт, запись — со 140 до 156. Старые записи журнала не проходят проверку длины и CRC, поэтому **поездка один раз обнулится** при первом запуске после прошивки. Калибровка бака хранится отдельно и не затрагивается.

Известные ограничения 0.4.4: временный регулятор яркости в портале задаёт значение только в RAM и исчезает после перезагрузки — удаление запланировано; текст диагностики использует среднюю точку `·` (U+00B7), которой нет в шрифте интерфейса, поэтому символ рисуется пустым; кадры шины комфорта недоступны, прибор подключён к сегменту OBD одним трансивером.

Сохраняются DTC safeguards 0.4.2 без изменений: усечённый DTC-ответ не объявляет потерянные коды исчезнувшими, API не подтверждает неизвестное состояние как текущее, checkpoint/reset использует read-back-подтверждённую durable-копию, post-clear schedule корректен при `uint32_t` rollover. NRC `0x78` — промежуточный ResponsePending с P2*=5 с и абсолютным пределом 15 с, без повторной передачи команды. Стирание DTC не запускается автоматически и остаётся защищённым read-back, остановкой двигателя/автомобиля и сохранением history.

Фон преобразуется браузером в строго `240×240` и `115 200` байт RGB565 little-endian. ESP32 повторно проверяет размеры, `Content-Length`, CRC32 и read-back, пишет неактивный A/B-файл и только затем атомарно переключает CRC-защищённый manifest.

Изменившееся runtime-состояние записывается единым CRC-защищённым snapshot в двухсегментный LittleFS journal каждые 20 секунд; NVS mirror обновляется каждые 60 секунд. Нормальное окно потери — 0–20 секунд, NVS fallback — 0–60 секунд.

**Статус:** 0.4.4 был проверен в дороге на автомобиле владельца — расход, разделение простоев и среднее по движению. Артефакты 0.4.3, 0.4.4 и 0.4.5 удалены из рабочего дерева при переходе к 0.4.6/0.4.7 и остались в истории по меткам. Аппаратно подтверждённым OTA-основанием проекта остаётся 0.3.9 (2026-09-23). Config schema остаётся **6**.

История: [`CHANGELOG.md`](CHANGELOG.md). Release report и контрольные суммы: [Метка `v0.4.4`](https://github.com/SergVmw/obd/blob/v0.4.4/releases/). Дополнительные документы: [`docs/OTA_POSTMORTEM_2026-09-19.md`](docs/OTA_POSTMORTEM_2026-09-19.md), [`docs/OTA_HARDENING_0.3.8.md`](docs/OTA_HARDENING_0.3.8.md), [дизайн ресурсов](docs/CUSTOM_VISUAL_ASSETS_DESIGN.md), [дизайн persistence](docs/POWER_LOSS_PERSISTENCE_DESIGN.md), [DTC safety](docs/OBD_DTC_DIAGNOSTICS_DESIGN.md).

## Реализовано

- TWAI ESP32-S3, автоматический bus-off recovery и RX budget 16 кадров/2 мс;
- Task Watchdog для Arduino loopTask;
- обнаружение поддерживаемых Mode 01 PID;
- PID 01 MIL/DTC-count каждые 500 мс в движении и latched Check Engine warning на GC9A01;
- однократная привязка engine ECU по валидному PID 0C и фильтрация ответов других ECU;
- асинхронные physical Mode 03/07/0A, bounded ISO-TP, NRC 0x78 с P2*/absolute deadline, до 32 DTC, web-список и осторожные generic-расшифровки;
- reboot-persistent bounded history transient/changed DTC: dirty-only LittleFS 20 с + NVS 60 с;
- ручной Mode 04 только после fresh pre-scan, durable snapshot, web-confirmation и свежих safety checks speed/RPM/voltage;
- обязательное post-clear verification и немедленный checkpoint без преждевременного удаления historical presence;
- MAP, RPM, speed, MAF, throttle, STFT, LTFT, BARO, voltage, coolant, Fuel Rate и equivalence ratio;
- расчёт относительного наддува;
- бензиновый расход по PID 5E с резервом MAF;
- отдельная full-tank калибровка бензина без сброса trip;
- LPG по MAF с отдельным коэффициентом;
- глобальное отключение всех LPG-функций;
- коррекция скорости для экрана, расстояния и л/100 км;
- DFCO и предупреждение STFT+LTFT на LPG;
- раздельные накопители бензина и LPG;
- low-voltage deep sleep с таймером 30 секунд и GPIO4 wake;
- отключённый расширяемый Mode 22/ISO-TP framework без неподтверждённых Haval DID;
- REST CAN Monitor;
- четыре страницы GC9A01;
- Golos Text: три встроенных 8-bit anti-aliased VLW subset и Golos GFX fallback;
- три постоянных font-specific PSRAM text layer с alpha blending по реальному фону;
- 16-bit RGB565 framebuffer и carbon background cache в PSRAM, 8-bit аварийный fallback;
- агрегированный serial benchmark рендера каждые 300 кадров;
- адаптивный LDR-контроллер яркости, PWM GPIO7, автоматическое обучение диапазона;
- ручной День/Ночь четырьмя нажатиями с сохранением в NVS;
- NVS config schema 6 с миграцией schema 5;
- Wi‑Fi captive portal и watchdog-safe raw app OTA без multipart, с metadata preflight, idle/absolute timeout и status recovery;
- нативная ESP-IDF OTA-запись с exact-length/hash validation, явным выбором и read-back целевого boot-слота;
- подтверждение pending-образа и постоянная post-reboot OTA-диагностика через API/UI;
- H2 build manifest и отображение версий `app0/app1` на GC9A01 и главной странице сервиса;
- загрузка из web UI пользовательского `240×240` фона и пропорционального логотипа до `220×80`, RGB565 LE, CRC32, A/B-файлы и A/B manifest;
- startup-only загрузка пользовательских ресурсов в PSRAM с безусловным embedded carbon/HAVAL fallback;
- 20-секундный dirty-only LittleFS journal из двух сегментов по 256 КиБ и 60-секундный sequence-bearing NVS fallback;
- восстановление после torn tail/CRC-ошибки, выбор новейшего LittleFS/NVS snapshot и диагностика sequence/source/age/failures в web UI;
- безопасное монтирование LittleFS: автоматическое форматирование разрешено только для полностью стёртого раздела;
- Wi‑Fi power save отключается на время service mode.

## Распиновка ESP32-S3

| Функция | GPIO |
|---|---:|
| Кнопка MODE/WAKE | GPIO4 |
| LPG sense после PC817 | GPIO5 |
| LDR / ADC1_CH5 | GPIO6 |
| TFT BL PWM | GPIO7 |
| TFT CS | GPIO10 |
| TFT DC | GPIO11 |
| TFT RST | GPIO12 |
| TFT MOSI | GPIO13 |
| TFT SCLK | GPIO14 |
| TWAI TX | GPIO16 |
| TWAI RX | GPIO17 |

Не использовать в проекте:

- GPIO19/20 — native USB;
- GPIO35/36/37 — Octal PSRAM N16R8;
- GPIO0/3/45/46 — strapping pins;
- GPIO38 — RGB LED распространённой DevKitC-1 v1.1;
- GPIO43/44 — сохранены для UART0.

## GC9A01

| GC9A01 | ESP32-S3 |
|---|---:|
| SCLK | GPIO14 |
| MOSI | GPIO13 |
| CS | GPIO10 |
| DC | GPIO11 |
| RST | GPIO12 |
| BL | GPIO7 → подтверждённый logic-вход BLK встроенного транзистора |
| VCC | 3.3 В |
| GND | GND |

Между GPIO12/RST и GND установить 10 кОм. На текущем дисплее BLK уже соединён с GPIO7, на модуле есть транзистор. Подтвердите низкотоковый logic-вход и active-HIGH полярность тестом 20% ↔ 80%; ток LED через GPIO не пропускать. Дополнительный ключ при подтверждённом встроенном logic-входе не нужен.

LDR: `3.3 В → LDR → узел → 22 кОм → GND`; узел через `1 кОм` на GPIO6, `100 нФ` GPIO6–GND. [Принципиальная схема](docs/ambient-light-circuit.svg).

## SN65HVD230

Трансивер располагается рядом с ESP32-S3 и питается локальными 3.3 В.

| SN65HVD230 | Соединение |
|---:|---|
| pin 1 D/TXD | GPIO16 |
| pin 2 GND | GND |
| pin 3 VCC | 3.3 В, 100 нФ + рекомендуется 1 мкФ |
| pin 4 R/RXD | GPIO17 |
| pin 5 Vref | NC |
| pin 6 CAN-L | OBD pin 14 |
| pin 7 CAN-H | OBD pin 6 |
| pin 8 Rs | GND, high-speed mode |

Не устанавливать дополнительный терминатор 120 Ом.

## Кнопка

```text
GPIO4 ── кнопка ── GND
```

GPIO4 является RTC-capable pin и используется для пробуждения из deep sleep. При длинном проводе рекомендуется внешняя подтяжка 10 кОм к 3.3 В. В режиме яркости «Ручное» четыре быстрых нажатия переключают День/Ночь без перелистывания; выбор сохраняется. Одиночные 1–3 нажатия в этом режиме ждут паузу 400 мс, long/service hold сохраняют прежние функции.

## LPG input

```text
два провода одной катушки BRC
 → полный диодный мост
 → 2.2 кОм + 2.2 кОм
 → PC817C/EL817C

3.3 В → 10 кОм → GPIO5 → collector PC817
GPIO5 → 100 нФ → GND
emitter PC817 → GND
```

Вход active LOW. Сторона катушки гальванически не соединяется с GND ESP32-S3. 12 В напрямую на GPIO5 подавать нельзя.

## Разделение на две коробки

### Возле OBD

- предохранитель и защита автомобильного входа;
- TPS26600/TVS/фильтр;
- MP1584 12→5 В;
- диодный мост и входная сторона PC817.

### Возле дисплея

- ESP32-S3 N16R8;
- SN65HVD230;
- GC9A01;
- кнопка;
- выход PC817/pull-up/RC;
- ключ подсветки.

Рекомендуемый межблочный шестиконтактный разъём:

```text
1  +5V_PROTECTED
2  POWER_GND
3  CAN-H
4  CAN-L
5  LPG_SENSE
6  SIGNAL_GND
```

Отдельный провод 3.3 В между коробками не нужен.

Подробности: [`docs/wiring.md`](docs/wiring.md) и [`docs/h2-gauge-schematic-notes.md`](docs/h2-gauge-schematic-notes.md).

## Сборка

Установить зафиксированные build/validation зависимости и собрать:

```bash
python -m pip install -r requirements-dev.txt
pio run -e esp32s3_n16r8
```

Файл приложения:

```text
.pio/build/esp32s3_n16r8/firmware.bin
```

Первая полная прошивка по USB:

```bash
pio run -e esp32s3_n16r8 --target upload
```

Первая установка должна записать bootloader, таблицу `partitions_16mb_ota.csv` и приложение.

## GitHub Actions

Workflow `.github/workflows/platformio.yml` запускает host/static/font/browser gates, включая объединение масок PID, полосу стояния скорости, разделение простоев, отбрасывание интервала при неизвестной скорости, яркость, штатный PID 0B status, DTC state-machine, truncated-history semantics, NRC 0x78/P2*/absolute deadline, exact-zero rollover, multi-ECU binding, paused post-clear continuation, persistence fault injection и Mode 04 guards; собирает только `esp32s3_n16r8`, проверяет manifest, SHA-256, побайтовое совпадение app и factory layout committed release и сохраняет текущие `firmware.bin`, `bootloader.bin` и `partitions.bin` как build artifacts. Package-check проверяет, что `releases/` содержит только артефакты текущей версии и что их SHA-256 совпадает с содержимым; прежде он дополнительно сверял предыдущий production-релиз по литералу `previous_version`, но после того как в дереве остался один релиз, проверять там было нечего, и она бы молча проходила вхолостую. Два гейта (`check_ota_transport`, `check_diagnostic_integration`) проверяли версию литералом `0.4.3` и падали на любом её изменении; теперь они проверяют форму — строка в кавычках длиной не более 15 символов, потому что манифест хранит её в `firmwareVersion[16]`. GCC prefix maps в `platformio.ini` нормализуют разные project/PlatformIO paths локальной машины и GitHub runner, поэтому строгая byte-equality проверка воспроизводима; анализ исправления: [`docs/CI_REPRODUCIBLE_BUILD_2026-09-30.md`](docs/CI_REPRODUCIBLE_BUILD_2026-09-30.md).

## Упакованный release 0.4.10

```text
releases/h2-gauge-v0.4.10-esp32s3-n16r8.bin
Размер: 1,222,688 байт
SHA-256: 54535d63d5e95747808b7905f77d97a5ce0e3c7e3dd4782bd7887bfceb1ec310

releases/h2-gauge-v0.4.10-esp32s3-n16r8-factory.bin
Размер: 1,288,224 байт
SHA-256: e8f2580d3f45521d26ede03d3e8926a2d97338bef06139c93114dafced726f7e
```

Первый файл — обычный app image для штатного web OTA. Второй — merged factory
image для чистой записи с offset `0x0`; он не предназначен для web OTA. Полный
отчёт: [`releases/README-v0.4.10.md`](releases/README-v0.4.10.md).

В дереве остаётся только текущий релиз; предыдущие доступны по меткам.

## Flash и PSRAM

При старте serial log должен показать приблизительно:

```text
Flash: 16 MB
PSRAM: 8 MB
```

Если PSRAM не обнаружена, прибор выводит ошибку в log и использует 8-bit framebuffer вместо RGB565. Для N16R8 настроен memory type `qio_opi`.

## OTA partitions 16 МБ

Используется `partitions_16mb_ota.csv`:

- APP0: 4 МБ;
- APP1: 4 МБ;
- Core dump: 64 КиБ;
- LittleFS: 8064 КиБ;
- NVS и OTA metadata в начале Flash.

Веб-OTA принимает только обычный app image `.bin`. Не загружать через него bootloader, partitions, factory/merged image или образ файловой системы. Нормальное обновление: выбрать app `.bin` → дождаться серверной проверки и выбора boot-слота → прибор сам перезагрузится. Достижение 100% передачи ещё не считается успехом; интерфейс ждёт `verified:true` и `bootVerified:true`. Сборки в `app0/app1` постоянно видны сверху главной страницы сервиса; подробные адреса, boot/next roles, image state, reset reason и результат прошлой попытки находятся в «Система → OTA».

## Сервисный режим

Вход:

- удерживать GPIO4 около 3 секунд во время включения;
- либо удерживать около 5 секунд на стоящем автомобиле.

По умолчанию:

```text
SSID: H2-Gauge-XXXX
Password: h2gauge18
URL: http://192.168.4.1
```

Пароль необходимо изменить после первой установки.

Полная инструкция: [`docs/WEB_INTERFACE_GUIDE.md`](docs/WEB_INTERFACE_GUIDE.md).

## Обновление встроенного web UI

Воспроизводимая генерация экранных Golos fonts, offline webfonts и встроенного web UI:

```bash
python3 tools/generate_ui_fonts.py
python3 tools/check_ui_fonts.py
python3 tools/test_brightness.py
python3 tools/test_obd_diagnostics.py
python3 tools/test_ota_diagnostics.py
python3 tools/test_persistence.py
python3 tools/test_storage_recovery.py
python3 tools/embed_web_fonts.py
python3 tools/embed_web.py
python3 tools/check_brightness_integration.py
python3 tools/check_dtc_integration.py
python3 tools/check_storage_integration.py
python3 tools/check_ota_transport.py
pio run -e esp32s3_n16r8
python3 tools/check_firmware_manifest.py
```

## Шрифт и лицензия

Golos Text взят из официального upstream commit `cf2e27222937d97c2d858fff0499bcc667a64e9d`. Исходные TTF и созданные subset распространяются по SIL Open Font License 1.1; полный текст находится в [`docs/GOLOS_FONT_LICENSE.txt`](docs/GOLOS_FONT_LICENSE.txt).

## Пока не реализовано или не проверено

- автомобильная проверка CAN Haval;
- BRC K-Line/KWP2000;
- подтверждённые Haval Mode 22 DID;
- BMP280;
- speed-density fallback;
- рабочий read-only Wi‑Fi во время движения;
- Bluetooth LE telemetry;
- аппаратное измерение фактической частоты кадров и проверка blending Golos на GC9A01;
- аппаратная проверка BLK, ADC GPIO6, автокалибровки и четырёх нажатий;
- сертифицированная автомобильная плата питания.

## Важные ограничения

- DevKitC-1 не является automotive-qualified платой.
- N16R8 с Octal PSRAM имеет паспортный верхний предел окружающей температуры +65 °C без ECC.
- MP1584 нельзя подключать непосредственно к OBD +12 В без предохранителя, TVS, OVP/reverse-polarity protection и фильтрации.
- Не подавать одновременно внешние 5 В и USB без проверки схемы развязки конкретной платы.
- Первый CAN-тест выполнять на стоящем автомобиле, сначала при питании ESP32-S3 от USB.

## Документация

- [`docs/BRIGHTNESS_GUIDE.md`](docs/BRIGHTNESS_GUIDE.md) — яркость, LDR, автокалибровка и проверка;
- [`docs/ambient-light-circuit.svg`](docs/ambient-light-circuit.svg) — принципиальная схема ADC-входа;
- [`docs/wiring.md`](docs/wiring.md) — полное подключение и межблочный кабель;
- [`docs/h2-gauge-schematic-notes.md`](docs/h2-gauge-schematic-notes.md) — pin-to-pin netlist и расчёты;
- [`docs/WEB_INTERFACE_GUIDE.md`](docs/WEB_INTERFACE_GUIDE.md) — все настройки веб-интерфейса;
- [`docs/troubleshooting.md`](docs/troubleshooting.md) — диагностика;
- [`docs/H2_GAUGE_PROJECT_NOTES.md`](docs/H2_GAUGE_PROJECT_NOTES.md) — журнал решений.
