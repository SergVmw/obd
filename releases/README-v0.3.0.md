# H2 Gauge 0.3.0 — ESP32-S3 N16R8 release

Цель: `ESP32-S3 DevKitC-1 compatible`, модуль `ESP32-S3-WROOM-1-N16R8`, 16 МБ QSPI Flash, 8 МБ OPI PSRAM.

## Файлы

### App / Web OTA

```text
h2-gauge-v0.3.0-esp32s3-n16r8.bin
946784 bytes
SHA-256 1dfd8287b37de87c3d2b6e055727220f2dfd4f4f76d02bfc963da3ba87e31962
```

Это app image для веб-OTA после того, как ESP32-S3 уже получила правильные bootloader и 16-МБ partition table. Записывается в OTA app slot, не по адресу 0x0.

### Factory image

```text
h2-gauge-v0.3.0-esp32s3-n16r8-factory.bin
1012320 bytes
SHA-256 15e3c420aa660964bd23db947c2858bfbdf4317ee872f444852f83b3a1b44414
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
esptool.py --chip esp32s3 --port COM_PORT --baud 921600 write_flash 0x0 h2-gauge-v0.3.0-esp32s3-n16r8-factory.bin
```

Замените `COM_PORT` на фактический COM/tty. Для восстановления после неудачной прошивки удерживайте BOOT, кратко нажмите RESET и повторите запись.

## Проверка SHA-256

```bash
sha256sum -c SHA256SUMS-v0.3.0.txt
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
- App image нельзя записывать по адресу 0x0.
- Factory image предназначен для адреса 0x0 и первой/восстановительной установки.
- При внешнем питании 5 В не подключать одновременно USB без проверки развязки питания конкретной DevKitC-1.
