# H2 Gauge 0.3.2 — ESP32-S3 N16R8 release

Цель: `ESP32-S3 DevKitC-1 compatible`, модуль `ESP32-S3-WROOM-1-N16R8`, 16 МБ QSPI Flash, 8 МБ OPI PSRAM.

## Что изменилось в 0.3.2

- все экранные надписи переведены на встроенные Golos Text;
- три VLW subset используют 8-bit alpha anti-aliasing: 13 px SemiBold, 19 px SemiBold и 38 px Bold;
- три постоянных RGB565 text layer в PSRAM смешивают сглаженные края с фактическим карбоновым фоном;
- второй RGB565 sprite хранит единожды построенный carbon background, который восстанавливается через `memcpy()`;
- при ошибке PSRAM/framebuffer/text-layer allocation сохраняются 8-bit framebuffer и Golos GFX fallback;
- serial log раз в 300 кадров сообщает average/max render time, среднее время background restore, состояние cache/smooth и свободную PSRAM;
- web UI и экранная preview используют встроенный offline-subset Golos без CDN;
- сохранены улучшения надёжности: Task Watchdog, CAN RX budget, транзакционная конфигурация, защищённые destructive API и корректный OTA abort.

NVS schema остаётся `5`; обновление с 0.3.1 не требует сброса настроек.

## Файлы

### App / Web OTA

```text
h2-gauge-v0.3.2-esp32s3-n16r8.bin
1046944 bytes
SHA-256 defa6581f52b7851b3de7604f5160920b5fd4dadfbb727566334b2b70841a8d1
```

Это app image для веб-OTA после того, как ESP32-S3 уже получила правильные bootloader и 16-МБ partition table. Записывается в OTA app slot, не по адресу `0x0`.

### Factory image

```text
h2-gauge-v0.3.2-esp32s3-n16r8-factory.bin
1112480 bytes
SHA-256 14e18e14ccfc4ea10ad74b00b958c95f8c4b97bb0f862981c96e69279825ed4d
```

Merged image содержит:

```text
0x0000  bootloader
0x8000  partitions_16mb_ota
0xe000  boot_app0
0x10000 application
```

Его можно записать с offset `0x0`. Для чистой первой установки предварительный `erase_flash` удалит NVS и все прежние данные.

## Рекомендуемая первая прошивка

Из исходников через PlatformIO:

```bash
pio run -e esp32s3_n16r8 --target upload
```

PlatformIO запишет bootloader, partition table, boot_app0 и приложение по правильным адресам.

Альтернатива для готового factory image:

```bash
esptool.py --chip esp32s3 --port COM_PORT erase_flash
esptool.py --chip esp32s3 --port COM_PORT --baud 921600 write_flash 0x0 h2-gauge-v0.3.2-esp32s3-n16r8-factory.bin
```

Замените `COM_PORT` на фактический COM/tty. Для восстановления после неудачной прошивки удерживайте BOOT, кратко нажмите RESET и повторите запись.

## Проверка SHA-256

```bash
sha256sum -c SHA256SUMS-v0.3.2.txt
```

## Проверенная сборка

```text
PlatformIO 6.1.18
espressif32 6.8.1
Arduino-ESP32 2.0.17
RAM:   50 124 / 327 680 bytes (15.3%)
Flash: 1 046 529 / 4 194 304 bytes (25.0%)
```

## Ожидаемый serial log

Сборка включает native USB CDC. После загрузки откройте serial monitor 115200 на native USB-C. Ожидаются примерно:

```text
target: esp32s3-n16r8
Flash: 16 MB
PSRAM: 8 MB
```

Если обнаружено меньше 16 МБ Flash или меньше примерно 8 МБ PSRAM, firmware выводит `N16R8 ... check failed`. В этом случае не переходите к автомобильному монтажу до проверки платы и сборки.

## Partition table

```text
APP0      4 МБ
APP1      4 МБ
Core dump 64 КиБ
LittleFS  8064 КиБ
```

## Важно

- Release предназначен только для ESP32-S3 DevKitC-1 N16R8.
- App image нельзя записывать по адресу `0x0`.
- Factory image предназначен для адреса `0x0` и первой/восстановительной установки.
- При внешнем питании 5 В не подключать одновременно USB без проверки развязки питания конкретной DevKitC-1.
- Сборка и статические проверки выполнены; визуальное качество Golos, частота кадров и benchmark PSRAM-кэша на физическом GC9A01 ещё должны быть проверены на плате.
