# GitHub Actions 0.3.7 — причина сбоя и исправление CI

Дата анализа: 2026-09-20. Репозиторий: `SergVmw/obd`.

## Наблюдавшийся сбой

GitHub Actions run `35470553078`, job `105970611660` завершился на шаге
`Run host regression suites`; последующие build/release-проверки не
запускались.

Публичный API показывал только `Process completed with exit code 1`, а raw log
без авторизации GitHub не отдавал. После этого владелец предоставил точный log:
GCC 13 останавливался при сборке `brightness_tests.cpp` на конфликте
`strlcpy()`:

```text
tests/host/Arduino.h: error: ‘std::size_t strlcpy(...)’ redeclared inline
without ‘gnu_inline’ attribute
/usr/include/x86_64-linux-gnu/bits/string_fortified.h:
note: ‘size_t strlcpy(...)’ previously defined here
```

## Точная причина

Host-заглушка `tests/host/Arduino.h` содержала собственную inline-реализацию
Arduino-совместимой `strlcpy()`. Это требовалось на старых Linux-системах, где
libc не предоставляла такую функцию.

glibc 2.38 добавила системные `strlcpy()` и `strlcat()`. В Ubuntu 24.04
используется более новая glibc; при активном `_FORTIFY_SOURCE` заголовок
`bits/string_fortified.h` уже определяет fortified inline `strlcpy()`. Затем
host-заглушка пыталась определить функцию повторно с несовместимыми атрибутами.
Это и было единственной показанной причиной exit code 1.

Первоначальная гипотеза о sanitizer/high-entropy ASLR возникла до получения raw
log и для этого запуска оказалась неверной: компиляция завершилась раньше, чем
мог стартовать ASan.

Сведения о добавлении `strlcpy()` в glibc 2.38:

- <https://sourceware.org/pipermail/libc-alpha/2023-July/150524.html>
- <https://sourceware.org/pipermail/libc-alpha/2023-April/147049.html>

## Исправление

1. Host fallback `strlcpy()` теперь компилируется только тогда, когда host libc
   не предоставляет системную функцию. Для glibc проверяются одновременно
   версия 2.38+ и `__USE_MISC`; учтены также libc macOS, BSD и Android.
2. Обе host-suite явно сбрасывают возможное distribution default и затем
   собираются с `_FORTIFY_SOURCE=3`, чтобы локальная проверка всегда
   воспроизводила строгий режим GitHub Runner без macro-redefinition warning.
3. ASan/UBSan и `-Werror` сохранены. Test executables остаются non-PIE, что
   отдельно устраняет известный риск ASan shadow-memory collision, но системный
   ASLR в workflow больше не понижается.
4. Runner зафиксирован как `ubuntu-24.04`; Python/build-зависимости и firmware
   libraries зафиксированы в `requirements-dev.txt` и `platformio.ini`.
5. GitHub Actions используются в текущей major-линии v7: `checkout`,
   `setup-python`, `upload-artifact`.

## Проверка исправленного дерева

После исправления выполнены:

- GCC 14 + `_FORTIFY_SOURCE=3`: 25 основных host regression групп — успешно;
- GCC 14 + `_FORTIFY_SOURCE=3`: 11 OTA diagnostic групп — успешно;
- GCC 13 + `_FORTIFY_SOURCE=3`: те же 25 + 11 групп — успешно;
- отдельный fortified `strlcpy` compile/link probe на glibc 2.41 — успешно;
- проверки OTA transport, brightness integration, Golos assets и manifest —
  успешно;
- чистая PlatformIO-сборка `esp32s3_n16r8` с зафиксированными версиями —
  успешно;
- app/factory release checksums — успешно.

Host-only исправление не меняет прошивку. Аппаратно проверенный app сохраняет
SHA-256:

`dc3b7d3b844481dde00db41c97deedc6204884d87c86ee156f6835b5d5ec652e`

Factory image сохраняет SHA-256:

`06b841a847fc427ddcb39923ce7a400505d99f688fcdeabf5de94cb07215654a`

## Критерий закрытия

Инцидент окончательно закрывается успешным GitHub Actions run после загрузки
исправленного дерева. Если новый run завершится ошибкой, анализируется его новый
log; старый конфликт `strlcpy()` уже имеет точный локально воспроизведённый
регрессионный тест.
