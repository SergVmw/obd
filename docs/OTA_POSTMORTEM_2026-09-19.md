# OTA 0.3.7 — postmortem и аппаратное подтверждение

Дата анализа: 2026-09-19. Прибор: ESP32-S3 N16R8. Исходная сборка/updater:
мост 0.3.6.1 в APP0. Загружаемый образ: исправленный на тот момент 0.3.7.

## Источник данных

Пользователь опубликовал архив:

`docs/ota-forensics-20260919-233504.zip`

в репозитории `https://github.com/SergVmw/obd`.

Локальная копия:
`ota-forensics-20260919-233504.zip`.

SHA-256 архива:

`2293993f205e2b06a8ddd69b4ef0cc31d976186c31065cf7b45157f3d8e4251c`

Содержимое архива:

| Файл | Размер | SHA-256 |
|---|---:|---|
| `app1-after-failure.bin` | 1 079 696 | `92b1acb8870e66023d5f575306462a9ea7f69cb3fb06abdd98fa1000e308c94a` |
| `otadata-after-failure.bin` | 8 192 | `f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0` |
| `coredump-after-failure.bin` | 65 536 | `20b99a1c03437be338a33f97aa5e46af1081091e2168f332a068422f5e25b745` |
| `summary.txt` | 729 | `bf37e363905f865516073f3c73426a6dbbceedf6ffc520d40d37b3f0f553fea4` |

APP1 и `otadata` в ZIP побайтно совпадают с ранее скачанными отдельными
файлами. Coredump имеет валидное ELF-начало, но для установления механизма
сбоя не требуется.

## Побайтный результат APP1

Ожидаемый образ:

- размер: 1 079 696 (`0x107990`) байт;
- SHA-256:
  `b04c8d93c0d27ed927dc8bfd85734491314c6d83f6c8ddb5a8171c373b6186bb`.

Дамп после reset:

- размер: 1 079 696 байт;
- точный совпадающий prefix: 1 078 436 (`0x1074A4`) байт;
- первая разница: offset `0x1074A4`;
- оставшийся хвост: 1 260 (`0x4EC`) байт;
- весь хвост равен `0xFF`;
- различается только последний неполный 4-КиБ flash block.

Главное равенство:

```text
1 079 696 = 751 × 1 436 + 1 260
1 078 436 = 751 × 1 436
```

Следовательно, OTA callback получил и записал ровно 751 полный raw-блок.
Последний 1 260-байтный callback не состоялся. Поэтому не выполнялись:

- последний `esp_ota_write()`;
- `RAW_END`;
- `esp_ota_end()`;
- journal checkpoint финализации;
- `esp_ota_set_boot_partition()`;
- HTTP server confirmation.

Сообщение capture script `APP1_RESULT=DOES_NOT_MATCH_CORRECTED_0.3.7` верно,
но отличие не случайно: оно точно равно отсутствующему последнему raw-фрагменту.

Исходные дампы и coredump после завершения анализа удалены из чистого Git-дерева; их размеры, SHA-256 и все доказательные результаты сохранены выше.

## Результат otadata

Первая запись содержит `ota_seq=1`; вторая запись стёрта/не используется.
APP0 остался выбранным boot slot. Это независимо подтверждает, что выбор APP1
не выполнялся. Никакого доказанного rollback не было: новый image вообще не
был финализирован и выбран.

## Точная цепочка timeout

Исходный `libraries/WebServer/src/Parsing.cpp` Arduino-ESP32 2.0.17 выполнял:

```cpp
client.readBytes(_currentRaw->buf, HTTP_RAW_BUFLEN);
```

на каждой итерации, где `HTTP_RAW_BUFLEN == 1436`, не ограничивая запрос
остатком объявленного `Content-Length`.

На последней итерации:

1. по `Content-Length` оставалось 1 260 байт;
2. `readBytes()` запросил 1 436;
3. 1 260 действительных байт были прочитаны во внутренний buffer;
4. `Stream::readBytes()` продолжил `timedRead()` для несуществующего байта
   1 261 и не вернул callback уже прочитанные данные;
5. ожидание длилось `Stream::_timeout`.

Почему timeout был ровно 5 000 мс:

- после обычного запроса `WebServer::handleClient()` вызывает
  `_currentClient.setTimeout(5)`;
- `WiFiClient::setTimeout(uint32_t)` в Arduino-ESP32 2.0.17 принимает секунды
  и задаёт базовый `Stream::_timeout = 5 × 1000`;
- `_currentClient` повторно используется;
- `WiFiClient::operator=` копирует socket/buffer state, но не сбрасывает и не
  копирует унаследованный `Stream::_timeout`;
- новый OTA request поэтому начинает parsing с оставшимся timeout 5 000 мс.

`enableLoopWDT()` подписывает `loopTask` на 5-секундный TWDT. Синхронный
невозможный read удержал тот же `loopTask` пять секунд. Watchdog reset успел
раньше, чем `readBytes()` вернул 1 260 байт и parser вызвал `RAW_WRITE`.

Вызов `server_.client().setTimeout(...)` из `RAW_START` не является
исправлением: `WebServer::client()` возвращает `WiFiClient` по значению, так
что меняется timeout временной копии, а не `_currentClient`, которым читает
parser.

## Постоянное исправление

В проект локально vendored WebServer 2.0.17. Raw parser теперь:

```cpp
const size_t remaining = _clientContentLength - _currentRaw->totalSize;
const size_t requested =
    remaining < HTTP_RAW_BUFLEN ? remaining : HTTP_RAW_BUFLEN;
_currentRaw->currentSize =
    readRawBodyChunk(client, _currentRaw->buf, requested);
```

Кроме того, parser читает только уже доступные socket-байты, кормит TWDT на
каждом проходе и обрывает отсутствие прогресса через 2 секунды. Настоящий
`client` внутри raw branch получает тот же defensive read timeout — меньше
5-секундного TWDT. Глобальный PlatformIO framework не редактируется.

Новый app 0.3.7 имеет размер 1 080 640 байт:

```text
1 080 640 = 752 × 1 436 + 768
```

Таким образом, аппаратная проверка снова использует неполный последний
фрагмент (768 байт) и не скрывает дефект padding-ом.

Для доставки исправленного parser в выполняемый source slot был создан
app-only USB-мост 0.3.6.2. После него финальная 0.3.7 была установлена одним
обычным web OTA; результат зафиксирован ниже.

## Аппаратное подтверждение исправления

20 сентября 2026 выполнена окончательная проверка на приборе:

1. app-only мост 0.3.6.2 был записан в APP0;
2. обычный app `.bin` 0.3.7 загружен через штатный web OTA;
3. сервер завершил проверку image и выбор boot-раздела;
4. устройство автоматически перезагрузилось без ручного RESET;
5. сервис показал текущую сборку 0.3.7, APP0 0.3.6.2, APP1 0.3.7;
6. running/boot — APP1, состояние image — `valid`.

Новый образ имел неполный последний raw-фрагмент 768 байт. Следовательно,
успех непосредственно подтверждает exact-remaining parser fix, а не случайную
кратность размера. Одноразовые bridge/forensic binaries после подтверждения
удалены из чистого проекта; постоянное исправление и regression-тесты оставлены.
