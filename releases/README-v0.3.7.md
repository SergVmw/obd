# H2 Gauge 0.3.7 — версии сборок в APP0 / APP1

**Только ESP32-S3 DevKitC-1 / ESP32-S3-WROOM-1-N16R8**, 16 МБ QSPI Flash + 8 МБ OPI PSRAM, GC9A01.

## Что добавлено

0.3.7 показывает не только running/boot addresses, но и номер H2 Gauge, фактически записанный в каждом OTA-разделе.

- На физическом экране **«СЕРВИС»** выводятся текущая сборка/running slot и короткая строка `APP0 …  APP1 …`.
- Сверху главной страницы web service постоянно видны карточки **Текущая сборка / APP0 / APP1**.
- Зелёная рамка отмечает running slot.
- Для каждого slot отображаются roles `запущен`, `выбран boot`, `следующий OTA` и image state.
- `/api/status.ota.slots[]` содержит version, source, address, running/boot/next flags и descriptor diagnostics.
- Каждый app начиная с 0.3.7 содержит 56-байтный проверяемый H2 manifest в первом DROM-сегменте.
- Официальный app 0.3.6 без manifest распознаётся по точному `app_elf_sha256`.
- Пустой, повреждённый или неизвестный старый slot честно обозначается `пусто`/`неизвестно`, а framework descriptor не выдаётся за номер H2 Gauge.

Нативный OTA 0.3.6 сохранён без ослаблений: raw transport, прямые `esp_ota_begin/write/end`, ESP32-S3/header/exact-length/hash validation, явный выбор неактивного slot, read-back boot partition, automatic reboot и post-reboot result. Ручной RESET не нужен.

Config schema остаётся **6**. Обновление не сбрасывает настройки, trip, бензиновую калибровку или световую автокалибровку.

## Файлы

| Файл | Назначение | Размер |
|---|---|---:|
| `h2-gauge-v0.3.7-esp32s3-n16r8.bin` | **Обычный app image для OTA** или app-only USB | 1 077 616 байт |
| `h2-gauge-v0.3.7-esp32s3-n16r8-factory.bin` | Полная чистая USB-установка, offset `0x0` | 1 143 152 байт |

```text
App SHA-256:
b5cea758be22a7480e2b6e4cc85d61ebcad72972ec94676a7e6c24c32cd648ad

Factory SHA-256:
a22a840ff44e37e24fc756aec73cc239fc417d96d803c6b1c6e8d8f89cef111a
```

Проверка из каталога `releases`:

```bash
sha256sum -c SHA256SUMS-v0.3.7.txt
```

## Контрольный OTA 0.3.6 → 0.3.7

Это первая аппаратная проверка исправленного native updater, уже установленного в 0.3.6.

### До загрузки

1. Полностью остановите автомобиль и обеспечьте стабильное питание.
2. Войдите в сервис 0.3.6 и запишите:
   - running slot;
   - boot slot;
   - next OTA slot;
   - last result.
3. Подключитесь к `H2-Gauge-XXXX` в полноценном Chrome/Safari/Firefox по `http://192.168.4.1`, а не через captive-portal WebView.
4. Если телефон пытается уйти в интернет, временно отключите мобильные данные, VPN, Private DNS/proxy и автоматическое переключение сети. **Wi‑Fi к прибору оставьте включённым.**

### Загрузка

1. В «Система → OTA» выберите только `h2-gauge-v0.3.7-esp32s3-n16r8.bin` — файл без `-factory`.
2. Нажмите «Установить» и подтвердите.
3. Не закрывайте страницу и не выключайте питание.
4. 100% означает только передачу. Дождитесь сообщения, что image проверен и target slot выбран.
5. Ничего вручную не нажимайте: ESP32 должна сама перезагрузиться.

Нормальный процесс не требует RESET, ручной смены slot, повторной загрузки или USB.

### Ожидаемый результат

После автоматического reboot прибор работает как 0.3.7. Для диагностики позже снова войдите в сервис — этот повторный вход не нужен для завершения OTA.

Ожидается:

- «Текущая сборка»: `0.3.7`;
- зелёная рамка находится на slot, который до OTA был `next`;
- running slot = boot slot;
- running slot содержит `0.3.7`;
- противоположный source slot содержит официальный `0.3.6`;
- last OTA = `Применено` / `applied`;
- reset reason = `software`;
- `diagnosticsStorageHealthy = true`.

Физический экран «СЕРВИС» должен показать, например:

```text
FW 0.3.7 / APP1
APP0 0.3.6  APP1 0.3.7*
```

Звёздочка означает running slot. Конкретные APP0/APP1 могут быть переставлены — важно, что running перешёл на прежний next slot.

Если результат отличается, **не повторяйте OTA и не стирайте Flash**. Сохраните фотографию физических строк, снимок трёх верхних web-карточек и полный `/api/status`.

## Как определяется версия slot

ESP-IDF descriptor prebuilt Arduino-ESP32 2.0.17 содержит framework string вроде `esp-idf: v4.4.7 ...`, а не версию H2 Gauge. Поэтому:

- 0.3.7+ — собственный H2 manifest (`H2G_FW_VERSION`, target, magic/format/size/trailer);
- официальный 0.3.6 — exact known ELF SHA;
- running image — дополнительно сверяется с compile-time `H2G_FW_VERSION`;
- неизвестный legacy image не получает придуманную версию.

Manifest читается один раз при входе в сервис и кэшируется; web polling не сканирует Flash каждые 1,2 секунды.

## Исторический USB bootstrap без потери данных

Этот путь нужен только если на устройстве всё ещё находится старый updater 0.3.5, который сообщил OTA success, но не смог активировать новый slot. Для обычного перехода 0.3.6→0.3.7 он не нужен.

В ROM download mode для **известной таблицы `partitions_16mb_ota.csv` этого проекта** можно один раз записать обычный app 0.3.7 в оба OTA-слота:

```bash
esptool.py --chip esp32s3 --port COM_PORT --baud 921600 write_flash \
  0x10000 h2-gauge-v0.3.7-esp32s3-n16r8.bin \
  0x410000 h2-gauge-v0.3.7-esp32s3-n16r8.bin
```

- Не запускайте `erase_flash`.
- Не используйте `-factory.bin`.
- Не записывайте bootloader, partitions или OTA data для ремонта.
- NVS `0x9000`, OTA metadata `0xe000`, trip и калибровки не затрагиваются.
- Не применяйте эти адреса к другой таблице разделов или другой плате.

## Полная чистая установка — не способ ремонта OTA

**Удаляет настройки, trip и калибровки.** Используется только для сознательной первой/чистой установки. Factory image нельзя загружать через web OTA.

```bash
esptool.py --chip esp32s3 --port COM_PORT erase_flash
esptool.py --chip esp32s3 --port COM_PORT --baud 921600 write_flash \
  0x0 h2-gauge-v0.3.7-esp32s3-n16r8-factory.bin
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
RAM:   52 876 / 327 680 байт (16.1%)
Flash: 1 077 197 / 4 194 304 байт (25.7%)
App image: 1 077 616 байт
Embedded UI: 108 385 байт → 57 371 байт gzip
```

- 25 групп C++ host-регрессий с ASan/UBSan прошли.
- Browser fixture проверил карточки 0.3.7/0.3.6, running highlight, next role, очистку stale state, verified OTA workflow и layouts 360…1280 px.
- Manifest checker нашёл единственную валидную запись `0.3.7 / esp32s3-n16r8` в первом DROM-сегменте.
- OTA validator подтвердил raw transport, direct ESP-IDF operations, inactive target, exact-length, set/read-back boot slot, startup confirmation и diagnostics.
- App: ESP32-S3 chip ID 9, шесть сегментов, checksum `d2` valid, validation hash `e2e437806c595479f2e952daf883dcc72af17bc81654d5fd6fc98b0a7186ce70` valid.
- Golos/VLW/webfont, JavaScript, UTF-8, embedded gzip и factory composition проверены.

**Аппаратно ещё нужно подтвердить:** реальный OTA 0.3.6→0.3.7, автоматическую смену slot и корректные строки/карточки обеих сборок. Программные тесты не заменяют проверку на ESP32-S3.

Дополнительные инструкции: [веб-интерфейс](../docs/WEB_INTERFACE_GUIDE.md), [диагностика](../docs/troubleshooting.md), [яркость](../docs/BRIGHTNESS_GUIDE.md).
