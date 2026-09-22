# Changelog

## 0.3.8 — 2026-09-22

Отдельный OTA-hardening кандидат для ESP32-S3 DevKitC-1 / N16R8. Аппаратно
подтверждённый 0.3.7 сохранён как исходная точка; 0.3.8 требует отдельной
проверки на приборе.

### OTA transport и API

- Добавлен абсолютный 180-секундный deadline всей raw OTA-сессии, который не
  продлевается медленным trickle; двухсекундный idle watchdog сохранён.
- `RAW_ABORTED` теперь различает idle/total timeout, disconnect и отказ handler,
  очищает OTA state, возвращает структурированный JSON при живом socket и после
  completion handler явно закрывает TCP.
- Добавлены `/api/ota/preflight` и `/api/ota/status`: браузер до полного binary
  POST передаёт точный metadata probe, а сервер проверяет имя, размер, ESP32-S3
  chip/app descriptor, движение, питание и реальный неактивный раздел.
- Ошибки содержат стабильный `code`, HTTP status, received/expected byte counts,
  timeout и при необходимости `Retry-After`; после сетевого сбоя UI восстанавливает
  конкретную причину через status endpoint.
- Во время OTA подавлены фоновые status/fuel/CAN polls; UI использует полученные
  от устройства `maxImageBytes`, `probeBytes` и timeout.

### Надёжность метаданных

- Начальный probe имеет точный compile-time размер ESP image header + segment
  header + app descriptor и может собираться из любого числа transport chunks.
- SDK-структуры заполняются выровненным `memcpy`; остаток блока, завершившего
  probe, сразу записывается без потери. Копирование descriptor version ограничено
  вместимостью destination.
- Native verify/select worker ограничен минимумом своего 30-секундного лимита и
  оставшегося общего OTA deadline. TWDT не отключается, multipart не возвращён.

### Проверки

- Расширен static transport gate: абсолютный timeout с `millis()` rollover,
  abort dispatch/close, metadata preflight и модель probe больше 1 436 байт.
- Browser fixture проверяет dynamic size limit, отказ несовместимого probe до
  binary POST, JSON error, network failure + status recovery и verified install.
- Полное описание решения и аппаратный чек-лист:
  [`docs/OTA_HARDENING_0.3.8.md`](docs/OTA_HARDENING_0.3.8.md).

## 0.3.7 — 2026-09-20

Аппаратно подтверждённый релиз для ESP32-S3 DevKitC-1 / N16R8.

### OTA

- Web OTA принимает обычный raw app `.bin` (`application/octet-stream`).
- Project-local WebServer 2.0.17 читает точный остаток `Content-Length`, кормит
  TWDT внутри receive-loop и ограничивает отсутствие прогресса двумя секундами.
- Сохранены прямые ESP-IDF write/end/select, проверка образа, boot read-back,
  rollback confirmation и включённый watchdog.
- Journal v3 различает `receiving`, `verifying`, `image_verified`,
  `boot_selected` и сохраняет receive progress/reset/native error.
- Реальный OTA APP0→APP1 завершён с автоматическим reboot; APP1/0.3.7 имеет
  состояние `valid`. Неполный последний raw-фрагмент — 768 байт.

### Интерфейс и устройство

- Физический и web-сервис показывают текущую сборку и версии APP0/APP1.
- Добавлены плавная адаптивная яркость GPIO6/GPIO7, ручной День/Ночь,
  автокалибровка диапазона и четыре режима яркости.
- Встроены сглаженные Golos UI / Golos Text.
- Сохранены текущие CAN/OBD, LPG, fuel/trip, power/deep-sleep и Mode 22 решения.

### Проверки

- 25 основных host regression-групп.
- 11 OTA journal/migration-групп с ASan/UBSan.
- Static transport, manifest, fonts, embedded UI, checksums и factory-layout.
- Browser fixture для slot cards, OTA phases, brightness и responsive layouts.
- CI зафиксирован на Ubuntu 24.04, PlatformIO 6.1.18 и проверенных версиях
  библиотек; host-заглушка `strlcpy()` совместима с fortified glibc 2.38+.

Подробный OTA postmortem: [`docs/OTA_POSTMORTEM_2026-09-19.md`](docs/OTA_POSTMORTEM_2026-09-19.md).
Анализ CI: [`docs/CI_POSTMORTEM_2026-09-20.md`](docs/CI_POSTMORTEM_2026-09-20.md).
