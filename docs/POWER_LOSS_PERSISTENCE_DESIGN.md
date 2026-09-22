# Мгновенное отключение питания и сохранение рабочих данных — архитектура 0.3.9

**Статус:** software-only схема `LittleFS 20 с + NVS 60 с` реализована и покрыта локальными host/static gates в кандидате 0.3.9; серия физических random-cut тестов на N16R8 ещё требуется. Пользователь уточнил, что отдельного `ACC_OFF/POWER_FAIL` и гарантированного hold-up 50–200 мс, скорее всего, не будет. Аппаратные варианты ниже оставлены только как справочные и не являются обязательным условием.

## Зафиксированное ограничение software-only

Без раннего сигнала и запаса энергии невозможно гарантированно сохранить изменения, произошедшие после последней уже завершённой записи во Flash. Ни shutdown callback, ни ISR, ни RTC RAM не выполнятся после физического исчезновения 3.3 В. Поэтому software-only решение не пытается угадать момент отключения, а заранее создаёт power-loss-safe checkpoints с контролируемым износом Flash.

В 0.3.9 реализован append-only журнал в существующем LittleFS 7,875 МиБ:

- единый 140-байтный snapshot ordinary trip + petrol calibration;
- interval 20 секунд, только если relevant state изменился;
- немедленная запись на reset/start/apply calibration, входе/выходе service, low voltage, reboot и при наблюдаемом `engine running → stopped`;
- `magic + schema + monotonically increasing sequence + CRC` у каждой записи;
- `append + flush + read-back`; запись считается checkpoint только после success;
- предыдущая valid запись не изменяется при создании новой;
- два чередующихся journal segment по 256 КиБ с безопасной ротацией;
- при boot выбирается запись с наибольшим valid sequence из LittleFS и NVS;
- combined NVS mirror с той же sequence записывается раз в 60 секунд и служит fallback/migration path;
- LittleFS никогда не форматируется автоматически при обычной ошибке mount; первичная инициализация допустима только для доказанно полностью стёртого раздела;
- background/logo используют отдельные A/B-файлы в том же LittleFS и не конфликтуют с journal paths.

При работающем 20-секундном журнале обычное окно потери составляет 0–20 секунд, в среднем около 10 секунд, плюс длительность уже идущего `flush`. Если запись в момент cut оборвётся, boot scan остановится на torn tail и загрузит предыдущую valid CRC-запись. При недоступном LittleFS остаётся NVS fallback с окном 0–60 секунд и явной диагностикой ошибки.

Два сегмента по 256 КиБ занимают 512 КиБ, то есть около 6,35% LittleFS. Вместе с A/B background/logo остаётся более 90% раздела. Расчёт endurance приведён ниже; измерение реального write amplification/flush time и физические random-cut испытания остаются аппаратным gate.

## Оценка варианта LittleFS 20 секунд + NVS 60 секунд

Это расчёт endurance, а не гарантированный срок службы. Он предполагает настоящий ESP32-S3-WROOM-1-N16R8 с паспортными 100 000 program/erase cycles на сектор, записи только при dirty state и равномерное wear levelling. Паспорт модуля также указывает 20-летнее retention; высокая температура, неизвестный flash у compatible/clone платы, write amplification и прочие отказы ограничивают практический срок раньше математического результата.

Количество checkpoints в год активной работы:

| Активная работа автомобиля | LittleFS, каждые 20 с | NVS, каждые 60 с |
|---:|---:|---:|
| 1 ч/сутки | 65 700 | 21 900 |
| 2 ч/сутки | 131 400 | 43 800 |
| 4 ч/сутки | 262 800 | 87 600 |
| 8 ч/сутки | 525 600 | 175 200 |
| 24 ч/сутки | 1 576 800 | 525 600 |

### LittleFS journal

Консервативная модель использует только два segment по 256 КиБ, всего 512 КиБ, и округляет каждый примерно 140–160-байтный snapshot до 256 физических байт. В таком объёме помещается около 2 048 checkpoints; при интервале 20 секунд один полный оборот получается примерно раз в 11,38 часа активной работы.

При идеальном распределении по этим 128 секторам и 100 000 erase cycles математический предел составляет около 1,14 млн активных часов, или 130 лет непрерывной работы. Чтобы не выдавать идеальную модель за реальность, при условном десятикратном write amplification нижняя расчётная граница становится около 13 лет непрерывной записи. При 2 ч/сутки это соответствует примерно 156 годам по erase budget; практический проектный предел всё равно следует считать не более 20 лет до аппаратного подтверждения и с учётом retention/температуры.

Если LittleFS фактически распределит dynamic blocks по большей свободной части 7,875-МиБ раздела, запас будет выше; в обязательный расчёт это не включается.

### NVS mirror

NVS имеет пять 4-КиБ страниц; для garbage collection одна должна оставаться свободной. `TripState` сейчас занимает 64 байта и ориентировочно четыре 32-байтные NVS entries с blob index. При 126 entries на страницу и четырёх рабочих страницах консервативная ёмкость до 100 000 erase cycles составляет около 12,4 млн trip checkpoints.

При записи раз в минуту:

- около 23,6 года, если писать круглосуточно только trip blob;
- около 11,8 года при круглосуточной постоянно active petrol calibration, когда каждую минуту добавляется второй blob аналогичного размера;
- при 2 ч/сутки соответствующий raw erase budget превышает 140 лет даже с постоянно active calibration.

Config/OTA/light writes используют тот же NVS, но их частота значительно ниже. Для инженерной оценки следует дополнительно применить хотя бы двукратный safety factor на копирование live entries и garbage collection: тогда круглосуточная модель становится примерно 12 лет для одного blob или 6 лет для двух, а при 2 ч/сутки — примерно 142 или 71 год соответственно. Реальное число entries и amplification должно быть подтверждено тестом на той же версии ESP-IDF/Arduino.

### Практический вывод для 20/60

При типичной автомобильной работе 1–4 часа в сутки схема `LittleFS 20 s + NVS 60 s` с большим запасом должна пережить 20-летний проектный срок по erase budget. Даже крайне консервативная LittleFS-модель с 10× write amplification даёт около 78 лет при 4 ч/сутки. NVS mirror при 8 ч/сутки даёт около 71 года для одного trip blob или около 35 лет при постоянно active petrol calibration и двух blobs каждую минуту. Основные риски становятся не числом записей, а температурой под солнцем, качеством конкретного модуля, корректностью `flush`/rotation и устойчивостью к random power cut.

Источники исходных параметров:

- ESP32-S3-WROOM-1/1U datasheet, Memory Specifications: 100 000 P/E cycles, 20 years retention — <https://documentation.espressif.com/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf>;
- ESP-IDF NVS documentation: append model и снижение erase frequency примерно в 126 раз — <https://github.com/espressif/esp-idf/blob/master/docs/en/api-reference/storage/nvs_flash.rst>;
- ESP-IDF ESP32-S3 File System Considerations: LittleFS power-failure resilience и dynamic wear levelling — <https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/file-system-considerations.html>.

## Вывод

Если питание прибора исчезает одновременно с выключением зажигания, прошивка не получает событие shutdown и не может выполнить «последнее сохранение». Обычные развязывающие электролитические конденсаторы действительно не удержат ESP32-S3 + TFT достаточно долго. Более того, один конденсатор без отдельного сигнала `POWER_FAIL` мало полезен: прошивка продолжит рендер, CAN/Wi‑Fi и подсветку до brownout, потратив запас энергии вместо немедленного checkpoint.

В baseline 0.3.8 ordinary trip и активный бензиновый калибровочный интервал сохранялись раз в 60 секунд: при жёстком отключении терялось 0–60 секунд, в среднем около 30 секунд. Реализация 0.3.9 уменьшает нормальное окно до 0–20 секунд; прежний предел 0–60 секунд остаётся только при недоступном LittleFS и восстановлении из NVS mirror.

## Где находятся данные

### Оперативная RAM/PSRAM — исчезает сразу

В RAM находятся:

- текущие OBD/CAN значения и timestamps;
- фильтры MAP/boost/brightness;
- мгновенный и средний расчёт до следующего checkpoint;
- прирост ordinary trip после последней записи;
- прирост petrol full-tank calibration после последней записи;
- peak boost экрана;
- незаписанное обучение LDR;
- framebuffer, background cache и smooth text layers.

После полного обесточивания эти данные теряются.

### NVS во внутренней 16-МБ Flash

Раздел:

```text
nvs  offset 0x9000  size 0x5000 = 20 KiB
```

Namespaces:

| Namespace | Что хранится | Когда записывается |
|---|---|---|
| `h2gauge` | вся ConfigData schema 6 | сразу по web Save, factory reset, ручному Day/Night gesture |
| `h2persist` | combined 140-byte trip + petrol-calibration snapshot с sequence/CRC | dirty-only mirror каждые 60 секунд и при forced checkpoints |
| `h2trip` | legacy petrol/LPG trip для миграции/downgrade | reset trip, engine stop, service, low-voltage sleep |
| `h2petcal` | legacy full-tank petrol interval для миграции/downgrade | start/apply, engine stop, service, low-voltage sleep |
| `h2light` | обученный min/max LDR | при изменении не чаще раза в 10 минут; service/reboot/sleep |
| `h2ota` | OTA phase/progress/result | непосредственно в критических OTA checkpoints |

Config, trip, petrol calibration, brightness range и OTA record имеют проверки magic/schema/checksum. ESP-IDF NVS является log-structured и wear-levelled, поэтому внезапное питание обычно оставляет старую либо новую valid запись, но запись нельзя намеренно начинать уже на падающем питании без energy reserve.

### App slots

Код, встроенные fonts/web UI/HAVAL logo находятся в `app0` или `app1`. Они не являются изменяемыми operational data.

### LittleFS

```text
littlefs  offset 0x820000  size 0x7E0000 = 7.875 MiB
```

В 0.3.9 здесь находятся `/trip-journal.a`, `/trip-journal.b` и отдельный каталог `/assets` с A/B background/logo и manifests. App-only OTA раздел не затрагивает.

## Что реально теряется при жёстком выключении

### Ordinary trip

При исправном LittleFS максимум 20 секунд, в среднем около 10 секунд на случайный cut. Только при NVS-only fallback предел возвращается к 60 секундам.

Пример при 100 км/ч:

```text
20 s = 0.56 km maximum normal loss
10 s = 0.28 km average normal loss per random cutoff
```

Пример при расходе 8 л/ч:

```text
20 s = 0.044 l maximum normal loss
10 s = 0.022 l average normal loss
```

На множестве коротких поездок ошибка всё ещё может суммироваться в одну сторону, поэтому engine-stop checkpoint сохранён.

### Petrol calibration interval

Если калибровка active, теряется тот же нормальный хвост 0…20 секунд топлива/дистанции либо 0…60 секунд при NVS-only fallback.

### LDR autocalibration

Последние ещё не checkpoint-нутые extrema могут потеряться, максимум до 10 минут обучения. Базовые brightness settings и уже сохранённый диапазон остаются.

### Peak boost и live telemetry

Не сохраняются намеренно и после включения начинаются заново.

### Config

После успешного HTTP-ответа на Save уже находится в NVS и не ждёт выключения автомобиля. То же относится к manual Day/Night gesture после успешного `putBytes()`.

## Почему текущий low-voltage sleep не решает выключение зажигания

`PowerManager` требует:

1. свежий OBD PID 42;
2. напряжение ниже 11.5 В;
3. такое состояние непрерывно 3 секунды.

Только затем `enterLowVoltageSleep()` принудительно сохраняет trip, petrol calibration и brightness, выключает OBD/Wi‑Fi/display и входит в deep sleep.

Если switched supply пропадает сразу, ESP32 не проживёт 3 секунды. Кроме того, при выключении ECU PID 42 часто первым становится stale, а missing/stale PID специально не считается low voltage. Этот механизм защищает при устойчивом низком напряжении аккумулятора, но не является ignition-off detector.

## Оценка необходимой ёмкости

Упрощённая формула:

```text
C = I × t / ΔV
```

Если разрешить падение 5-В шины только на 0.5 В:

| Ток после/до load shedding | Время | Требуемая ёмкость |
|---:|---:|---:|
| 300 мА | 1 с | 0.6 Ф |
| 100 мА | 1 с | 0.2 Ф |
| 100 мА | 100 мс | 20 000 мкФ |
| 100 мА | 50 мс | 10 000 мкФ |
| 300 мА | 50 мс | 30 000 мкФ |

Идеальная оценка для 300 мА и ΔV=0.5 В:

```text
1 000 µF  ≈ 1.7 ms
4 700 µF  ≈ 7.8 ms
10 000 µF ≈ 16.7 ms
```

Реально время меньше из-за ESR, пиков Flash current, dropout регуляторов и brownout threshold. Следовательно, обычные 100–1 000 мкФ являются фильтрацией, а не секундным hold-up.

После немедленного отключения BLK, Wi‑Fi, CAN и TFT load может существенно снизиться, поэтому разумная инженерная цель — обеспечить не одну секунду полного режима, а примерно 50–200 мс аварийного режима. Точное время NVS write и ток надо измерить на реальной DevKitC-1.

## Варианты решения

### Вариант A — постоянное защищённое питание + отдельный ACC/IGN sense

Лучший логический вариант:

- прибор питается от защищённого постоянного OBD pin 16;
- отдельный вход сообщает состояние ACC/IGN;
- при ACC off прошивка немедленно checkpoint-ит данные;
- затем выключает backlight/CAN/Wi‑Fi и входит в deep sleep;
- пробуждение — ACC, GPIO4 или иная выбранная линия.

Проблема: ESP32-S3 DevKitC-1 вместе с onboard regulator/LED/USB circuitry может потреблять в deep sleep существенно больше голого модуля. Текущий timer wake каждые 30 секунд также увеличит средний ток. Перед постоянным подключением к аккумулятору обязательны измерение реального off-current и отдельная battery-drain оценка; wake policy потребуется изменить.

### Вариант B — switched supply + POWER_FAIL + hold-up

Если питание должно физически отключаться:

- upstream IGN/ACC или comparator/eFuse `PG` идёт на отдельный GPIO;
- diode/ideal-diode не даёт hold-up capacitor разряжаться обратно в автомобильную цепь;
- reservoir находится на выбранной защищённой шине;
- `POWER_FAIL` приходит раньше, чем 5 В/3.3 В выйдут из допуска;
- ISR только ставит flag;
- loop/task немедленно выключает BLK, прекращает CAN/Wi‑Fi/render;
- синхронно сохраняет compact trip/calibration record;
- переводит display в reset и ждёт отключения/deep sleep.

Целевой hold-up после load shedding: сначала измерить NVS write, затем заложить минимум 2–3-кратный запас, ориентировочно 50–200 мс. Требование «1 секунда на полном токе» делает ёмкость порядка 0.2–0.6 Ф и обычно неоправданно.

Суперконденсатор требует ограничения зарядного тока, проверки ESR, утечки, напряжения и automotive temperature; его нельзя просто добавить параллельно 5 В.

### Вариант C — внешняя FRAM

I²C FRAM (например, подходящая automotive/temperature версия семейства MB85RC) позволяет записывать compact trip record каждую секунду или чаще:

- очень высокая endurance;
- запись без erase и практически без задержки;
- при мгновенном cut теряется не больше выбранного периода;
- большой hold-up для последней записи не требуется, потому что предыдущая запись уже завершена.

Нужны дополнительная микросхема, адрес/пины/развязка и versioned dual-record с sequence+CRC. Для действительно мгновенного отключения это наиболее надёжный вариант точной статистики, если постоянное питание/ACC sense нежелательны.

### Вариант D — только более частые internal-Flash checkpoints

Можно уменьшить 60 секунд до 10–15 секунд и писать только при изменении. Это уменьшит loss window, но увеличит NVS traffic в 4–6 раз. NVS-раздел всего 20 КиБ; без расчёта erase cycles и rotating journal интервал 1 секунда применять нельзя.

Компромисс без hardware:

- trip/petrol calibration checkpoint каждые 10–15 секунд;
- дополнительный checkpoint при подтверждённом переходе RPM `running → stopped`;
- checkpoint при входе в service/reboot оставить;
- checksum/schema сохранить;
- измерить NVS erase/write behaviour и срок службы.

Этот вариант не даёт нулевой потери и не исправляет cut во время самого write.

## Принятая комбинация при software-only ограничении

1. Уже упакованный 0.3.8 не изменять; его аппаратная OTA-приёмка остаётся отдельным gate.
2. В 0.3.9 использовать 20-секундный dirty-only append-only LittleFS journal как основное хранилище operational counters.
3. Использовать 60-секундный combined NVS checkpoint с той же sequence как независимый fallback; старые `h2trip`/`h2petcal` сохранить для миграции и downgrade compatibility.
4. Выполнять opportunistic checkpoint при реально увиденном `engine running → stopped`, но не считать его гарантией: при одновременном исчезновении ECU и питания код не выполнится.
5. Показывать mount/write/CRC/age/source/sequence diagnostics в service UI и при отказе LittleFS продолжать через NVS fallback.
6. Не устанавливать internal-NVS interval 1–5 секунд: маленький 20-КиБ раздел для такого write rate не предназначен.
7. Не рассчитывать на случайный остаточный заряд фильтрующих конденсаторов.

## Реализация и оставшиеся проверки

Реализовано:

- records с magic, schema, monotonic sequence и CRC;
- ordinary trip и petrol calibration в одном согласованном snapshot;
- append+flush+read-back без перезаписи предыдущей valid записи;
- newest valid LittleFS/NVS selection и torn/corrupt tail recovery;
- два bounded segment с безопасной ротацией и dirty-only writes;
- blank-only auto-format policy;
- mount/write/CRC/age/source diagnostics в service UI/REST;
- host coverage для интервалов, NVS repair, torn tail, corrupt CRC и monotonic reset;
- отсутствие journal writes во время OTA/service raw upload;
- app-only partition layout, сохраняющий LittleFS.

Остаётся физически проверить:

- typical/worst-case append, flush и read-back time на N16R8;
- random cut в начале/середине/конце append и при segment rotation;
- недоступный/corrupt LittleFS на реальном разделе и NVS fallback;
- app0↔app1 OTA/rollback с сохранением journal и assets;
- ускоренный flash-wear/write-amplification сценарий.
