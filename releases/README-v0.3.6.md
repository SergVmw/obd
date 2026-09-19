# H2 Gauge 0.3.6 — исправление активации OTA

**Только ESP32-S3 DevKitC-1 / ESP32-S3-WROOM-1-N16R8**, 16 МБ QSPI Flash + 8 МБ OPI PSRAM, GC9A01.

## Главное изменение

0.3.6 устраняет ситуацию, когда обычный app `.bin` полностью передан, сервер сообщил успех и выполнил автоматическую перезагрузку, но загрузчик снова запустил прежнюю версию.

- Arduino `Update` заменён прямыми `esp_ota_begin/write/end`.
- До записи проверяются ESP application header, app descriptor и chip ID ESP32-S3.
- Ожидаемая, принятая и записанная длины обязаны совпасть.
- `esp_ota_end()` проверяет checksum/hash образа.
- Сервер явно выбирает неактивный OTA slot через `esp_ota_set_boot_partition()`.
- Выбор немедленно читается обратно; HTTP success возможен только при совпадении target address.
- Ответ содержит `verified:true` и `bootVerified:true`; 100% передачи само по себе не считается установкой.
- После полного HTTP-ответа устройство автоматически перезагружается. Ручной RESET не нужен.
- Новый pending image явно подтверждается на старте.
- Running/boot/next slots, image state, reset reason и результат прошлой попытки доступны в сервисе без serial log.

Watchdog-safe raw `application/octet-stream` сохранён: multipart не используется, Task Watchdog не отключается. Адаптивная яркость 0.3.5, Golos, CAN, LPG, trip и топливные функции сохранены. Config schema остаётся **6**, поэтому OTA не сбрасывает настройки, trip, топливную или световую калибровки.

## Файлы

| Файл | Назначение | Размер |
|---|---|---:|
| `h2-gauge-v0.3.6-esp32s3-n16r8.bin` | **Обычный app image для OTA** или app-only USB | 1 072 992 байт |
| `h2-gauge-v0.3.6-esp32s3-n16r8-factory.bin` | Полная чистая USB-установка, offset `0x0` | 1 138 528 байт |

```text
App SHA-256:
646babc02e7b7624a7d168c3dc03bfcc6d8b452ebdbeb411dd341aeee015f7f9

Factory SHA-256:
3cfdf8b23d39693d38d35d351454a7953109bd98d3b6cfaeee975c4ac3753954
```

Проверка из каталога `releases`:

```bash
sha256sum -c SHA256SUMS-v0.3.6.txt
```

## Штатное обновление через Wi‑Fi

1. Автомобиль должен стоять; обеспечьте стабильное питание.
2. Войдите в сервис и подключитесь по Wi‑Fi к `H2-Gauge-XXXX`.
3. Используйте полноценный Chrome, Safari или Firefox по адресу `http://192.168.4.1`, а не captive-portal WebView. На части Android-устройств WebView не открывает выбор файла.
4. Если телефон уводит запросы в интернет, временно отключите мобильные данные, VPN, Private DNS, proxy и автоматическое переключение сети. **Wi‑Fi к прибору оставьте включённым.**
5. В «Система → OTA» выберите только `h2-gauge-v0.3.6-esp32s3-n16r8.bin` — файл **без `-factory`**.
6. Подтвердите установку, не выключайте питание и дождитесь серверной проверки.
7. После сообщения о выбранном slot ничего не нажимайте: устройство само перезагрузится в 0.3.6.

Не требуется вручную нажимать RESET, менять slot, повторно запускать установку или стирать Flash. Повторный вход в сервис нужен только если вы хотите посмотреть диагностику, а не для завершения OTA.

## Важное ограничение первого перехода с 0.3.5

Первый переход **0.3.5 → 0.3.6** выполняет ещё updater из установленной 0.3.5. Сначала обязательно попробуйте штатный Wi‑Fi-порядок выше.

Если после подтверждённой загрузки и автоматической перезагрузки прибор показывает 0.3.6 — переход завершён, USB не нужен.

Если старый updater сообщил успех, перезагрузил устройство, но прибор снова показывает 0.3.5, не лечите это ручным RESET, повторными входами в сервис или многократной загрузкой того же файла. Один раз разверните исправленный updater по USB способом ниже. Это исключительный bootstrap, а не нормальный OTA-процесс.

## Одноразовый USB bootstrap без потери данных

Только для известной таблицы `partitions_16mb_ota.csv` этого проекта и ESP32-S3 N16R8. Переведите плату в ROM download mode и запишите **обычный app image** в оба OTA-слота:

```bash
esptool.py --chip esp32s3 --port COM_PORT --baud 921600 write_flash \
  0x10000 h2-gauge-v0.3.6-esp32s3-n16r8.bin \
  0x410000 h2-gauge-v0.3.6-esp32s3-n16r8.bin
```

- Не запускайте `erase_flash`.
- Не используйте `-factory.bin`.
- Не записывайте bootloader, partitions или OTA data для этого ремонта.
- NVS `0x9000`, OTA metadata `0xe000`, trip и калибровки не затрагиваются.
- `COM_PORT` замените на фактический порт.
- Не применяйте эти адреса к другой таблице разделов или другой плате.

После bootstrap прибор должен запустить 0.3.6 независимо от того, на какой из двух app-slots указывает существующая OTA metadata.

### Как доказать работу нового updater

После запуска 0.3.6 можно один раз загрузить через сервис тот же обычный app-файл 0.3.6. Версия останется 0.3.6, но native handler должен:

1. записать противоположный неактивный slot;
2. подтвердить `verified` и `bootVerified`;
3. автоматически перезагрузить прибор;
4. после следующего входа в сервис показать новый running slot и последний результат `Применено` (`applied`).

Это проверяет именно updater 0.3.6 end-to-end. В дальнейшем обычный процесс всегда один: выбрать app `.bin`, дождаться проверки, получить автоматическую перезагрузку в новый release.

## OTA-диагностика 0.3.6

В «Система → OTA» отображаются:

- авторитетная версия H2 Gauge;
- запущенный, выбранный boot и следующий OTA slots с адресами;
- состояние running image;
- причина последнего reset;
- результат попытки: applied / rolled back / unexpected slot / pending;
- исправность хранения диагностической записи.

Низкоуровневое поле API `descriptorVersion` — descriptor prebuilt Arduino framework (`esp-idf: ...`), а не номер H2 Gauge. Для номера релиза используйте только верхнеуровневое `/api/status.version`.

## Полная чистая установка — не способ ремонта OTA

**Эта операция удаляет настройки, trip и калибровки.** Она приведена только для сознательной первой/чистой установки. Не используйте её для исправления активации OTA и не загружайте factory image через web UI.

```bash
esptool.py --chip esp32s3 --port COM_PORT erase_flash
esptool.py --chip esp32s3 --port COM_PORT --baud 921600 write_flash \
  0x0 h2-gauge-v0.3.6-esp32s3-n16r8-factory.bin
```

Factory layout:

```text
0x0000  bootloader
0x8000  partitions_16mb_ota
0xe000  boot_app0 / initial OTA data
0x10000 application
```

## Проверено программно

```text
PlatformIO 6.1.18 / espressif32 6.8.1 / Arduino-ESP32 2.0.17
Чистая сборка ESP32-S3 N16R8: успешно
RAM:   52 644 / 327 680 байт (16.1%)
Flash: 1 072 573 / 4 194 304 байт (25.6%)
App image: 1 072 992 байт
Embedded UI gzip: 56 774 байта
```

- 25 групп C++ host-регрессий с ASan/UBSan прошли.
- Browser fixture проверил brightness и OTA cards/workflow, factory rejection и layouts 360…1280 px.
- OTA static validator подтвердил raw transport, прямые ESP-IDF operations, exact-length, abort paths, set/read-back boot slot, startup confirmation и diagnostics.
- Проверены Golos/VLW/webfont, JavaScript, UTF-8 и точное совпадение embedded gzip с `web/index.html`.
- App: ESP32-S3 chip ID 9, шесть сегментов, checksum `b3` valid, validation hash `3533dc6cc4d29cc43f8b6d070ca28f68f47d450bd85d2945e6a83e51ef940c7e` valid.
- Factory payload побайтно содержит тот же app по offset `0x10000`; bootloader, partition table и initial OTA data проверены.

**Ещё обязательно проверить на реальном устройстве:** переход 0.3.5→0.3.6, затем OTA из 0.3.6 с автоматической активацией противоположного slot; GPIO6/ADC с Wi‑Fi, BLK/PWM, startup/deep sleep, реальный NVS и кнопку. Сборка и host/browser-тесты не заменяют аппаратные испытания.

Дополнительные инструкции: [веб-интерфейс](../docs/WEB_INTERFACE_GUIDE.md), [диагностика](../docs/troubleshooting.md), [яркость](../docs/BRIGHTNESS_GUIDE.md).
