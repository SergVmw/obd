# H2 Gauge 0.3.7 — исправленный raw OTA

**Цель:** ESP32-S3 DevKitC-1 / ESP32-S3-WROOM-1-N16R8,
16 МБ QSPI Flash + 8 МБ OPI PSRAM, дисплей GC9A01.

## Статус

**Аппаратно подтверждённый релиз.** Причина второго reset установлена побайтным
анализом APP1/`otadata`, исправление прошло clean build, host/static/browser и
artifact-тесты. 20 сентября 2026 обычный app `.bin` 0.3.7 успешно установлен
штатным web OTA: server verification завершилась, устройство автоматически
перезагрузилось без RESET, running/boot стали APP1, состояние image — `valid`.
APP0 сохранил использованный одноразовый мост 0.3.6.2, APP1 содержит и запускает
0.3.7.

## Точная причина прежнего reset

Дамп APP1 после отказа совпал с прежним образом 0.3.7 до offset `0x1074A4`.
Ровно последние 1 260 байт остались стёртыми (`FF`):

```text
размер образа       1 079 696 байт
raw buffer               1 436 байт
полные callback             751
записанный prefix     1 078 436 байт = 751 × 1 436
незаписанный хвост        1 260 байт
```

Arduino-ESP32 2.0.17 `WebServer` всё равно вызывал
`readBytes(buf, 1436)` для последней итерации. После 1 260 байт он ждал ещё
176 байт за пределами `Content-Length`. Ранее завершённый HTTP-запрос оставлял
в повторно используемом `_currentClient` timeout 5 000 мс. Это происходило
потому, что `WebServer::handleClient()` вызывает `_currentClient.setTimeout(5)`,
а `WiFiClient::setTimeout()` в этой версии принимает секунды и меняет
унаследованный `Stream::_timeout`. Следующее присваивание клиента не сбрасывает
этот timeout.

Итог: невозможный final read блокировал `loopTask` ровно на те же пять секунд,
на которые настроен его TWDT. Reset происходил **до** последнего `RAW_WRITE`,
`RAW_END`, `esp_ota_end()`, journal checkpoint и выбора APP1. `otadata`
подтвердил, что выбранным остался APP0.

`server_.client().setTimeout(...)` из callback проблему не решает:
`WebServer::client()` возвращает `WiFiClient` по значению, то есть меняется
временная копия, а не parser `_currentClient`.

Полный отчёт и аппаратный результат:
[`../docs/OTA_POSTMORTEM_2026-09-19.md`](../docs/OTA_POSTMORTEM_2026-09-19.md).

## Постоянное исправление

В проект локально включена библиотека `lib/H2PatchedWebServer`, основанная на
`WebServer` Arduino-ESP32 2.0.17. Глобальный пакет PlatformIO не изменяется.

Исправленный raw-путь:

- запрашивает только
  `min(HTTP_RAW_BUFLEN, Content-Length - totalSize)`;
- читает только доступные socket-байты, кормит TWDT внутри receive-loop и
  ограничивает отсутствие прогресса двумя секундами;
- устанавливает тот же 2-секундный defensive timeout непосредственно на
  настоящий parser client;
- не отключает watchdog и продолжает кормить его в OTA callbacks;
- оставляет прямые `esp_ota_write()`, `esp_ota_end()`, проверку образа,
  `esp_ota_set_boot_partition()` и обязательный read-back;
- выполняет потенциально блокирующие native finish/select операции worker-задачей
  с 30-секундной границей и кормлением TWDT ожидающим `loopTask`.

Журнал OTA обновлён до v3:

- `receiving` сохраняется до синхронного body-loop;
- прогресс сохраняется каждые 256 КиБ;
- далее следуют `verifying`, `image_verified`, `boot_selected`;
- post-reboot результат различает `interrupted_upload`,
  `interrupted_finalize`, `finalize_failed`, `boot_selection_failed`,
  `rolled_back`, `unexpected_slot` и `applied`;
- сохраняются reset reason и native `errorCode/errorName`.

## Актуальные файлы 0.3.7

| Файл | Назначение | Размер | SHA-256 |
|---|---|---:|---|
| `h2-gauge-v0.3.7-esp32s3-n16r8.bin` | **Обычный app image для web OTA** | **1 080 640** | `dc3b7d3b844481dde00db41c97deedc6204884d87c86ee156f6835b5d5ec652e` |
| `h2-gauge-v0.3.7-esp32s3-n16r8-factory.bin` | Полная чистая USB-установка по `0x0` | **1 146 176** | `06b841a847fc427ddcb39923ce7a400505d99f688fcdeabf5de94cb07215654a` |

Проверка:

```bash
sha256sum -c SHA256SUMS-v0.3.7.txt
```

Обычный app имеет остаток **768 байт** при делении на 1 436. Это намеренно
оставленный реальный неполный final fragment, который доказывает исправление;
бинарник не дополнялся до удобного размера.

## Аппаратное подтверждение

Контрольный переход выполнен полностью:

1. source APP0 содержал одноразовый мост 0.3.6.2;
2. через web выбран обычный `h2-gauge-v0.3.7-esp32s3-n16r8.bin`;
3. последний неполный 768-байтный raw-фрагмент принят без TWDT reset;
4. завершены `RAW_END`, проверка image, выбор и read-back APP1;
5. устройство автоматически перезагрузилось без ручного RESET;
6. сервис показал текущую сборку 0.3.7, APP0 0.3.6.2, APP1 0.3.7,
   running/boot APP1 и image state `valid`.

Одноразовые recovery-образы и аппаратные дампы после подтверждения удалены из
чистого Git-дерева. Для последующих обновлений используется только нормальный
порядок: обычный app `.bin` → server confirmation → автоматический reboot.

## Factory image — не средство ремонта

Factory image содержит:

```text
0x0000  bootloader
0x8000  partitions_16mb_ota
0xe000  boot_app0 / initial OTA data
0x10000 application 0.3.7
```

Он предназначен только для осознанной чистой USB-установки и может удалить
настройки, поездки и калибровки. В web OTA файл `-factory.bin` загружать
запрещено.

## Остальные возможности 0.3.7

- На физическом экране «СЕРВИС» отображаются текущая сборка, running slot и
  версии APP0/APP1.
- В web service постоянно видны карточки текущей сборки, APP0 и APP1; running
  slot отмечен зелёной рамкой.
- `/api/status.ota.slots[]` содержит version/source/address/running/boot/next.
- Каждый новый app содержит проверяемый H2 manifest; официальный legacy 0.3.6
  распознаётся по точному `app_elf_sha256`.
- Config schema остаётся 6; OTA не сбрасывает настройки, trip, топливную или
  световую калибровку.
- Raw transport остаётся `application/octet-stream`, без multipart.

## Проверки этой сборки

```text
PlatformIO 6.1.18
espressif32 6.8.1
Arduino-ESP32 2.0.17
Dependency graph: H2PatchedWebServer 2.0.0-h2.1
RAM:   52 892 / 327 680 байт (16.1%)
Flash: 1 080 229 / 4 194 304 байт (25.8%)
App image: 1 080 640 байт
Embedded UI: 109 525 байт → 57 724 байт gzip
```

- Финальная ESP32-S3 clean-сборка 0.3.7 завершилась без warnings/errors.
- 11 OTA-journal host-групп прошли с ASan/UBSan, включая reset в `receiving`
  с сохранённым прогрессом и миграцию v2→v3.
- Static validator проверяет exact remaining-length raw read, parser-client
  timeout, включённый TWDT, worker/semaphore finish/select, journal и read-back.
- Manifest checker нашёл ровно одну запись `0.3.7 / esp32s3-n16r8`.
- Packager проверил точное равенство build/release app, factory offsets и `FF`
  gaps, app payload по `0x10000`, SHA-256 и неполный 768-байтный final fragment.
- Golos VLW/webfont, embedded gzip, Python и прежние brightness/host проверки
  проходят.

## Отозванные файлы

Не использовать:

- мост 0.3.6.1 — размер 1 079 696, SHA-256
  `08d931145a297ec1f78f3447a884b4a24e32fb4a1e0b0b4dc417b38436df7ac4`;
- предыдущий 0.3.7 после второго reset — размер 1 079 696, SHA-256
  `b04c8d93c0d27ed927dc8bfd85734491314c6d83f6c8ddb5a8171c373b6186bb`;
- самый первый кандидат — размер 1 077 616, SHA-256
  `b5cea758be22a7480e2b6e4cc85d61ebcad72972ec94676a7e6c24c32cd648ad`.
