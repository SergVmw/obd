# GitHub Actions 0.4.1 — воспроизводимость firmware.bin между локальной и CI-сборкой

Дата анализа: 2026-09-30. Репозиторий: `SergVmw/obd`.

## Наблюдавшийся сбой

GitHub Actions run `36678832459`, job `109769620825` завершился ошибкой на
шаге `Check build and committed release artifacts`. До него успешно прошли:

- все host regression suites;
- embedded UI, fonts и OTA invariants;
- browser OTA/UI regression;
- PlatformIO build `esp32s3_n16r8`.

Удаление четырёх устаревших файлов 0.3.9 было выполнено правильно: commit
`6823c5a59c714086815059b46456ffad1fb48037` оставил в `releases/` только четыре
файла 0.4.1. Ошибка не была связана ни с этим удалением, ни с исходным кодом
OBD/OTA/UI.

## Точная причина

`tools/check_packaged_artifacts.py` намеренно требует, чтобы свежий
`.pio/build/esp32s3_n16r8/firmware.bin` был побайтово равен committed OTA app.
Это полезный release gate, но исходная PlatformIO-конфигурация не обеспечивала
воспроизводимость между разными абсолютными путями сборки.

Arduino-ESP32 компилирует часть framework и libraries при каждой сборке.
Макросы `ESP_LOG*` сохраняют `__FILE__` в application image. Локальный release
содержит пути вида:

```text
/home/user/.platformio/packages/framework-arduinoespressif32/...
```

GitHub runner компилирует те же файлы из `/home/runner/.platformio/...` и
checkout-каталога `/home/runner/work/obd/obd`. Это меняет:

1. строки `__FILE__` в `.rodata`;
2. размещение последующих данных при другой длине пути;
3. DWARF/path metadata в ELF;
4. `app_elf_sha256` в ESP application descriptor;
5. итоговый SHA-256 application image.

Поэтому зафиксированные версии Ubuntu, Python, PlatformIO, platform и libraries
сами по себе недостаточны для побайтовой воспроизводимости.

## Локальное воспроизведение

Один и тот же source tree был чисто собран с альтернативным PlatformIO core
`/home/user/.platformio-ci`:

- исходный app: 1 190 288 байт,
  `cd2f6f0bc966972997c8a91465aaf3b549a5084f11bc9a577a21c31776af1d41`;
- app с ненормализованным альтернативным core path: 1 190 352 байта,
  `06ad4ca756230cc516e4601c9603ef36e39c54f3bf75f3a357ae222e3a3d3d31`;
- Flash payload вырос с 1 189 877 до 1 189 941 байта;
- в новом image обнаружена строка `/home/user/.platformio-ci/...` вместо
  `/home/user/.platformio/...`.

Отдельная сборка из другого project directory при прежнем core path сохранила
runtime-размер, но получила другой hash из-за path-dependent ELF digest. Это
подтвердило обе части причины.

## Исправление

В `platformio.ini` добавлены глобальные GCC prefix maps:

```ini
-ffile-prefix-map=$PROJECT_DIR=/home/user/H2-Gauge
-ffile-prefix-map=$PROJECT_CORE_DIR=/home/user/.platformio
```

`$PROJECT_DIR` и `$PROJECT_CORE_DIR` раскрываются текущим PlatformIO/SCons для
конкретной машины. Канонические значения совпадают со средой, в которой уже был
собран release 0.4.1. Поэтому исправление:

- нормализует local/CI absolute paths;
- применяется к project, dependency и Arduino framework compilation;
- не отключает строгую проверку byte equality;
- не требует замены app/factory binaries;
- не меняет размеры, runtime-код или опубликованные SHA-256 0.4.1.

Ранний `extra_scripts`/`CCFLAGS` hook не подходит: framework library builders
получают build flags на другой стадии. Prefix maps должны находиться именно в
общем `build_flags` environment.

## Проверка исправления

После изменения выполнены две чистые сборки:

1. `/home/user/H2-Gauge` + `/home/user/.platformio`;
2. `/tmp/H2-Gauge-ci` + `/home/user/.platformio-ci`.

Обе получили один и тот же app:

```text
Размер: 1190288
SHA-256: cd2f6f0bc966972997c8a91465aaf3b549a5084f11bc9a577a21c31776af1d41
```

Дополнительно успешно прошли:

- firmware manifest `0.4.1 / esp32s3-n16r8`;
- build/committed-app byte equality;
- factory offsets и FF gaps;
- app/factory checksums;
- правило «в releases только текущая версия».

RAM осталась 53 340 байт, Flash payload — 1 189 877 байт. Следующий GitHub
Actions run после загрузки `platformio.ini` должен пройти тот же строгий gate
без изменения release binaries.
