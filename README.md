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

## Текущая версия 0.3.6

Проект предназначен только для ESP32-S3 DevKitC-1 N16R8. Версия **0.3.6** исправляет активацию OTA-образа после успешной передачи: прошивка использует прямые операции ESP-IDF `esp_ota_*`, проверяет заголовок приложения, целевой чип, точную длину и hash образа, явно выбирает неактивный OTA-слот и читает выбор загрузчика обратно до ответа об успехе. После ответа устройство автоматически перезагружается; ручной RESET не нужен. После старта pending-образ подтверждается, а слоты, reset reason и результат предыдущего OTA доступны в сервисе без serial log.

Адаптивная подсветка 0.3.5 по LDR на ADC1 GPIO6 сохранена: Авто / Ручное / Всегда день / Всегда ночь, фильтрация, раздельные задержки, обучение диапазона и live-данные. Четыре быстрых нажатия в ручном режиме переключают День/Ночь с сохранением. Config schema **6** не изменена; обновление сохраняет NVS-настройки, trip, топливную и световую калибровки. [Подключение и алгоритм яркости](docs/BRIGHTNESS_GUIDE.md).

## Реализовано

- TWAI ESP32-S3, автоматический bus-off recovery и RX budget 16 кадров/2 мс;
- Task Watchdog для Arduino loopTask;
- обнаружение поддерживаемых Mode 01 PID;
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
- Wi‑Fi captive portal и watchdog-safe raw app OTA без multipart;
- нативная ESP-IDF OTA-запись с exact-length/hash validation, явным выбором и read-back целевого boot-слота;
- подтверждение pending-образа и постоянная post-reboot OTA-диагностика через API/UI;
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

Требуется PlatformIO:

```bash
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

Workflow `.github/workflows/platformio.yml` собирает только environment `esp32s3_n16r8` и сохраняет `firmware.bin`, `bootloader.bin` и `partitions.bin` как build artifacts.

## Release 0.3.6

```text
releases/h2-gauge-v0.3.6-esp32s3-n16r8.bin
Размер: 1 072 992 байт
SHA-256: 646babc02e7b7624a7d168c3dc03bfcc6d8b452ebdbeb411dd341aeee015f7f9

releases/h2-gauge-v0.3.6-esp32s3-n16r8-factory.bin
Размер: 1 138 528 байт
SHA-256: 3cfdf8b23d39693d38d35d351454a7953109bd98d3b6cfaeee975c4ac3753954
```

Первый файл — обычный app image для веб-OTA. Второй — merged factory image для действительно чистой записи с offset `0x0`; он не предназначен для OTA и стирает сохранённые данные. Инструкция, включая безопасный одноразовый USB-переход с неисправного updater 0.3.5 без `erase_flash`: [`releases/README-v0.3.6.md`](releases/README-v0.3.6.md).

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

Веб-OTA принимает только обычный app image `.bin`. Не загружать через него bootloader, partitions, factory/merged image или образ файловой системы. Нормальное обновление: выбрать app `.bin` → дождаться серверной проверки и выбора boot-слота → прибор сам перезагрузится. Достижение 100% передачи ещё не считается успехом; интерфейс ждёт `verified:true` и `bootVerified:true`. Текущий, выбранный и следующий слоты, состояние образа, причина reset и результат прошлой попытки показываются в «Система → OTA».

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
python3 tools/embed_web_fonts.py
python3 tools/embed_web.py
python3 tools/check_brightness_integration.py
python3 tools/check_ota_transport.py
pio run -e esp32s3_n16r8
```

## Шрифт и лицензия

Golos Text взят из официального upstream commit `cf2e27222937d97c2d858fff0499bcc667a64e9d`. Исходные TTF и созданные subset распространяются по SIL Open Font License 1.1; полный текст находится в [`docs/GOLOS_FONT_LICENSE.txt`](docs/GOLOS_FONT_LICENSE.txt).

## Пока не реализовано или не проверено

- физическая проверка прошивки 0.3.6 и реальный переход OTA 0.3.5 → 0.3.6 на ESP32-S3;
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
