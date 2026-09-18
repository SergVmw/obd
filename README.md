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

## Текущая версия 0.3.3

Проект предназначен только для ESP32-S3 DevKitC-1 N16R8. Настроены 16-МБ partition table, QIO Flash/OPI PSRAM, native USB CDC и проверка фактического размера памяти. Версия 0.3.3 сохраняет anti-aliased Golos Text, постоянные PSRAM text layers и RGB565-кэш карбонового фона из 0.3.2 и исправляет выбор `.bin` в OTA: вместо скрытого элемента используется видимый нативный file picker, совместимый с обычными мобильными браузерами. Также включены безопасные fallback, Task Watchdog, CAN RX budget, транзакционная конфигурация и NVS schema 5.

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
- NVS schema 5;
- Wi‑Fi captive portal и локальное app OTA.

## Распиновка ESP32-S3

| Функция | GPIO |
|---|---:|
| Кнопка MODE/WAKE | GPIO4 |
| LPG sense после PC817 | GPIO5 |
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
| BL | GPIO7 через подтверждённый транзисторный ключ |
| VCC | 3.3 В |
| GND | GND |

Между GPIO12/RST и GND установить 10 кОм. На стенде BL/BLK можно оставить на постоянных 3.3 В, а GPIO7 не подключать. PWM разрешён только после установки ключа или подтверждения штатного logic-входа BLK.

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

GPIO4 является RTC-capable pin и используется для пробуждения из deep sleep. При длинном проводе рекомендуется внешняя подтяжка 10 кОм к 3.3 В.

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

## Release 0.3.3

```text
releases/h2-gauge-v0.3.3-esp32s3-n16r8.bin
SHA-256: 90087cc470165f064b672a5a2732d04d09362bae0e9053faa64af1ec7cea7a84

releases/h2-gauge-v0.3.3-esp32s3-n16r8-factory.bin
SHA-256: c38d7e82266a113104d3d310368b2e0f0f5617faef730c54830940508a29151b
```

Первый файл — app image для веб-OTA. Второй — merged factory image для чистой записи с offset `0x0`. Инструкция: [`releases/README-v0.3.3.md`](releases/README-v0.3.3.md).

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

Веб-OTA принимает только app image `firmware.bin`. Не загружать через него bootloader, partitions или merged image.

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
python3 tools/embed_web_fonts.py
python3 tools/embed_web.py
pio run -e esp32s3_n16r8
```

## Шрифт и лицензия

Golos Text взят из официального upstream commit `cf2e27222937d97c2d858fff0499bcc667a64e9d`. Исходные TTF и созданные subset распространяются по SIL Open Font License 1.1; полный текст находится в [`docs/GOLOS_FONT_LICENSE.txt`](docs/GOLOS_FONT_LICENSE.txt).

## Пока не реализовано или не проверено

- физическая проверка прошивки 0.3.3 на приобретённой ESP32-S3;
- автомобильная проверка CAN Haval;
- BRC K-Line/KWP2000;
- подтверждённые Haval Mode 22 DID;
- BMP280;
- speed-density fallback;
- рабочий read-only Wi‑Fi во время движения;
- Bluetooth LE telemetry;
- аппаратное измерение фактической частоты кадров и проверка blending Golos на GC9A01;
- day/night automation;
- сертифицированная автомобильная плата питания.

## Важные ограничения

- DevKitC-1 не является automotive-qualified платой.
- N16R8 с Octal PSRAM имеет паспортный верхний предел окружающей температуры +65 °C без ECC.
- MP1584 нельзя подключать непосредственно к OBD +12 В без предохранителя, TVS, OVP/reverse-polarity protection и фильтрации.
- Не подавать одновременно внешние 5 В и USB без проверки схемы развязки конкретной платы.
- Первый CAN-тест выполнять на стоящем автомобиле, сначала при питании ESP32-S3 от USB.

## Документация

- [`docs/wiring.md`](docs/wiring.md) — полное подключение и межблочный кабель;
- [`docs/h2-gauge-schematic-notes.md`](docs/h2-gauge-schematic-notes.md) — pin-to-pin netlist и расчёты;
- [`docs/WEB_INTERFACE_GUIDE.md`](docs/WEB_INTERFACE_GUIDE.md) — все настройки веб-интерфейса;
- [`docs/troubleshooting.md`](docs/troubleshooting.md) — диагностика;
- [`docs/H2_GAUGE_PROJECT_NOTES.md`](docs/H2_GAUGE_PROJECT_NOTES.md) — журнал решений.
