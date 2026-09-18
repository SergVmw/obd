# H2 Gauge 0.3.3 — ESP32-S3 N16R8 OTA hotfix

Цель: `ESP32-S3 DevKitC-1 compatible`, модуль `ESP32-S3-WROOM-1-N16R8`, 16 МБ QSPI Flash, 8 МБ OPI PSRAM.

## Исправление 0.3.3

- скрытый HTML file input, который не открывался в некоторых мобильных браузерах, заменён постоянно видимым нативным выбором файла;
- удалён ограничивающий `accept`, несовместимый с частью Android file picker;
- перед загрузкой проверяются расширение `.bin` и размер app-образа от 4 КиБ до 4 МиБ;
- добавлены понятные состояния ошибок, тайм-аут 180 секунд и повторное включение кнопки после неудачи;
- web UI предупреждает, что системное окно captive portal может запрещать доступ к файлам и для OTA следует открыть `http://192.168.4.1` в обычном Chrome, Safari или Firefox.

Остальная функциональность 0.3.2, включая smooth Golos, PSRAM text layers и carbon background cache, сохранена. NVS schema остаётся `5`; сброс настроек не нужен.

## Файлы

### App / Web OTA

```text
h2-gauge-v0.3.3-esp32s3-n16r8.bin
1047312 bytes
SHA-256 90087cc470165f064b672a5a2732d04d09362bae0e9053faa64af1ec7cea7a84
```

Это app image для OTA. Его нельзя записывать по адресу `0x0`.

### Factory image

```text
h2-gauge-v0.3.3-esp32s3-n16r8-factory.bin
1112848 bytes
SHA-256 c38d7e82266a113104d3d310368b2e0f0f5617faef730c54830940508a29151b
```

Factory image записывается с offset `0x0` и содержит:

```text
0x0000  bootloader
0x8000  partitions_16mb_ota
0xe000  boot_app0
0x10000 application
```

## Как обновить устройство с 0.3.2

1. Подключитесь к Wi‑Fi `H2-Gauge-XXXX`.
2. Закройте автоматически открывшееся окно captive portal.
3. Запустите обычный Chrome, Safari или Firefox.
4. Введите `http://192.168.4.1` и откройте раздел «Система».
5. Попробуйте выбрать app-файл 0.3.3. В обычном браузере скрытый picker 0.3.2 обычно работает, даже если системный captive WebView его блокировал.
6. Если picker 0.3.2 не открывается и в обычном браузере, установите factory image 0.3.3 через USB; после этого дальнейший OTA будет работать через видимый нативный picker.

## USB-установка factory image

```bash
esptool.py --chip esp32s3 --port COM_PORT erase_flash
esptool.py --chip esp32s3 --port COM_PORT --baud 921600 write_flash 0x0 h2-gauge-v0.3.3-esp32s3-n16r8-factory.bin
```

Предварительный `erase_flash` удаляет NVS, поездку и калибровки. Если требуется сохранить настройки, сначала попробуйте OTA через обычный браузер.

## Проверка SHA-256

```bash
sha256sum -c SHA256SUMS-v0.3.3.txt
```

## Проверенная сборка

```text
PlatformIO 6.1.18
espressif32 6.8.1
Arduino-ESP32 2.0.17
RAM:   50 124 / 327 680 bytes (15.3%)
Flash: 1 046 897 / 4 194 304 bytes (25.0%)
```

Аппаратную проверку OTA в конкретном телефоне и браузере всё равно необходимо выполнить на устройстве.
