# Changelog

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
