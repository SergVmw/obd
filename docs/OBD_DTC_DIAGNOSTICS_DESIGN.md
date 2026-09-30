# Check Engine / OBD DTC diagnostics — архитектура 0.4.1

**Дата:** 2026-09-29  
**Статус:** исправлено после review отозванного 0.4.0, проверено host/static/browser/PlatformIO и упаковано отдельным release 0.4.1; до аппаратной проверки на Haval H2 чтение считать новым, а стирание DTC — экспериментальным.  
**Исходное событие:** 2026-09-27 владелец наблюдал мигающую лампу Check Engine при высокой скорости.

## 1. Важное предупреждение

Мигающий Check Engine под высокой нагрузкой часто означает активные пропуски зажигания с риском перегрева и повреждения катализатора. Прошивка помогает зафиксировать MIL и считать коды, но не делает дальнейшее движение безопасным. Если мигание повторится, нужно немедленно снизить нагрузку и безопасно остановиться. При неровной работе, потере тяги, сильной вибрации или запахе несгоревшего топлива двигатель лучше заглушить и продолжать диагностику без нагрузки.

Стандартный OBD PID 01 передаёт команду MIL ON/OFF и число подтверждённых DTC, но не отдельный признак «лампа мигает». Поэтому прошивка не утверждает, что распознала именно мигание: она опрашивает статус каждые 500 мс, фиксирует любое увиденное MIL ON до конца текущего запуска и немедленно инициирует чтение DTC.

## 2. Реализованный охват

### Автоматически и только для чтения

- Mode 01 PID `01` — статус MIL и число подтверждённых DTC;
- Mode `03` — сохранённые emissions-related DTC;
- Mode `07` — ожидающие DTC текущего/последнего цикла;
- Mode `0A` — постоянные DTC;
- single-frame и bounded ISO-TP multi-frame ответы;
- functional PID `0C` discovery: первый структурно валидный RPM response из `0x7E8..0x7EF` однократно фиксирует engine ECU;
- следующий PID `01`, Mode 03/07/0A, safety PID и Mode 04 адресуются физически только этому ECU; ответы другого ECU не меняют target и не заполняют обычную telemetry;
- NRC `0x78` продолжает тот же запрос без retransmit: initial P2=750 мс, P2*=5000 мс, абсолютный предел=15000 мс и максимум восемь pending-response;
- NRC `0x11/0x12` означает unsupported service, прочие финальные NRC — отдельный отрицательный результат, а не пустой список кодов;
- до 32 текущих кодов и до 96 байт собранного ответа;
- отдельная bounded-история до 32 кодов с ECU, когда-либо наблюдавшимися категориями, последним известным присутствием, счётчиком появлений и change sequence;
- история изменений сохраняется CRC-защищённым двухсегментным LittleFS journal каждые 20 секунд и NVS mirror каждые 60 секунд; перед Mode 04 обе стороны синхронизируются немедленно;
- negative response/NRC, timeout, malformed sequence и transport error остаются видимыми в web status.

Если ECU ещё не известен, ручное чтение выполняет цепочку `functional 01/0C → lock response ID → physical 01/01 → physical 03/07/0A`. Обычный polling также первым запрашивает PID 0C и после lock игнорирует ответы других модулей. Затем здоровое состояние перепроверяется не чаще одного раза в 300 секунд, а состояние с MIL/DTC — раз в 60 секунд. Переход MIL OFF→ON или изменение сообщённого ECU числа DTC вызывает внеочередное чтение. Все запросы проходят через общий `maxRequestsPerSecond` и единственный outstanding OBD transaction.

### Никогда не выполняется автоматически

Mode `04` — стирание emissions DTC. Команда доступна только из Wi-Fi service UI и требует отдельного подтверждения пользователя.

## 3. Формат и представление кодов

Два байта DTC преобразуются в стандартную форму `P/C/B/U` + четыре символа, например `P0301`. Категории не объединяются: один код может отдельно присутствовать как stored, pending или permanent.

Встроены краткие русские расшифровки для ограниченного набора общеупотребительных generic-кодов, особенно `P0300..P0312`, смеси, MAF/MAP, наддува, питания и катализатора. Для неизвестного generic-кода UI честно сообщает, что нужна документация; для manufacturer-specific кода не придумывается расшифровка Haval.

Код — диагностический указатель, а не готовый приговор детали. Например, `P0301` может быть вызван свечой, катушкой, форсункой, смесью, компрессией, проводкой или другими причинами. На автомобиле с BRC необходимо сравнивать проявление на бензине и LPG, но не стирать код до сохранения freeze-frame и условий возникновения.

## 4. Экран прибора

Строка статуса на главном, топливном и температурном экранах имеет приоритет:

1. пропуски `P0300..P0312` — красное `ПРОПУСКИ P03xx!`;
2. текущий или зафиксированный в этом запуске MIL — красное `CHECK ENGINE` и первый код;
3. DTC без MIL — жёлтое `DTC Pxxxx`;
4. предупреждение калибровки LPG;
5. обычные `95/LPG` и OBD.

Служебная физическая страница показывает MIL, сообщённое ECU число кодов, количества stored/pending/permanent, первый текущий код либо первый код истории, признак misfire, OBD timeout и CAN errors. Полный текущий список, сохранённая история и стирание находятся в web UI.

MIL latch хранится только в RAM текущего запуска. Отдельная история DTC переживает reboot и сохраняет исчезнувший/transient код для последующего просмотра, но UI отличает текущий список от «последнего наблюдения». ECU остаётся главным источником истины: история не выдаётся за текущую активную неисправность после успешного полного сканирования.

## 5. Web API

```text
GET  /api/diagnostics/dtc
POST /api/diagnostics/dtc/scan
     X-H2G-Action: dtc-scan

POST /api/diagnostics/dtc/clear
     Content-Type: application/json
     X-H2G-Action: dtc-clear
     {"confirmation":"CLEAR_DTC","acknowledgeReadinessReset":true}
```

Операции асинхронные: POST возвращает `202`, а браузер читает состояние через GET. WebServer не блокируется в ожидании ECU, поэтому не повторяется прежняя проблема multipart/TWDT.

GET возвращает:

- текущий и latched MIL;
- engine ECU ID и признак immutable lock;
- статусы Mode 03/07/0A, промежуточный ResponsePending и его bounded count;
- массив кодов с типом, raw value, ECU, misfire flag и осторожной расшифровкой;
- operation/error/NRC/timeout;
- bounded history с `currentlyListed`, `historicalOnly`, `seenKinds`, `lastKnownPresentKinds`, occurrence/change sequence и состоянием CRC/LittleFS/NVS;
- факт обязательного pre-clear scan и durable snapshot, состояние и измеренные safety-gate значения последнего Mode 04;
- pending/complete/preserved состояния обязательного post-clear verification.

## 6. Защита стирания

После подтверждения браузера ESP32 не отправляет Mode 04 сразу. Для каждого отдельного clear он физически и последовательно выполняет:

1. новое physical-чтение Mode `03`, `07`, `0A` у подтверждённого PID 0C engine ECU — старый scan не считается достаточным;
2. отказ, если хотя бы одна категория завершилась timeout/malformed/transport/final negative response либо ответ был truncated; `0x11/0x12` остаются явным unsupported;
3. немедленный CRC journal + NVS checkpoint обновлённой истории; до подтверждения хотя бы одной read-back-проверенной durable-копии CAN-запросы приостановлены;
4. PID `0D`: скорость должна быть `0 км/ч`;
5. PID `0C`: двигатель должен быть остановлен (`RPM < 50`);
6. PID `42`: напряжение ECU должно быть `11,5..16,5 В`;
7. только затем отправляется физический запрос `01 04`.

Явный `unsupported` для категории считается завершённым ответом ECU, но отображается пользователю. Любой незавершённый pre-scan, отказ обеих persistent-копий, timeout, malformed/negative response, движение, работающий двигатель или небезопасное напряжение отменяют команду. Повторное принятие clear ограничено cooldown 60 секунд.

Успешный Mode 04:

- удаляет сохранённые и ожидающие emissions DTC в ECU;
- удаляет freeze-frame и сбрасывает readiness-мониторы;
- может временно погасить MIL, если причина больше не активна;
- **не стирает permanent Mode 0A напрямую**;
- не ремонтирует неисправность: активный код вернётся.

Positive response `0x44` не считается доказательством исчезновения причины и не очищает `lastPresentKinds`. Через 1,5 секунды прошивка выполняет обязательное контрольное physical-чтение 03/07/0A. Это явно помеченное продолжение ручной операции запускается даже когда periodic polling поставлен на паузу сервисным режимом. Только успешно прочитанная категория может изменить historical-presence; timeout/NRC/unsupported сохраняет последнее подтверждённое наблюдение. После завершения scan state machine останавливается в `preserve_after_clear`, main немедленно checkpoint-ит историю и лишь затем возвращает операцию в idle. Permanent entries не объявляются стёртыми до фактического ответа ECU.

## 7. Ограничения первой реализации

- Читается основной engine ECU, а не все возможные модули автомобиля. ABS/SRS/BCM и фирменные Haval-коды требуют адресов и протоколов соответствующих блоков.
- Стандартные Mode 03/07/0A не заменяют Haval-specific расширенную диагностику и freeze-frame viewer.
- Отдельного стандартизованного признака мигающей против постоянно горящей MIL нет.
- Максимум 32 текущих DTC, 32 history entries и 96 байт собранного payload; truncation показывается явно, а truncated pre-clear scan запрещает Mode 04.
- Один запрос ждёт initial P2 750 мс; каждый принятый NRC 0x78 переключает inactivity window на P2* 5 с, но не сдвигает абсолютную границу 15 с. Девятый pending завершает запрос timeout.
- История bounded: при переполнении сначала вытесняется самая старая уже отсутствующая запись; `truncated` остаётся видимым. Это журнал наблюдений, не freeze-frame viewer и не замена ECU.
- LittleFS DTC journal использует два сегмента по 64 КиБ, CRC/read-back и ротацию; NVS — отдельный namespace. Запись происходит только при изменении payload, а не на каждом poll.
- Встроенная расшифровка ограничена проверенными generic-значениями. Неизвестные и manufacturer-specific коды не интерпретируются догадками.

## 8. Аппаратный acceptance checklist

Проверять на стоящем автомобиле с устойчивым питанием:

1. При зажигании ON подтвердить functional PID 0C lock, затем physical PID 01, правильные ECU request/response IDs и отсутствие роста CAN errors; при ответах нескольких модулей target не должен меняться.
2. Сравнить physical Mode 03/07/0A с независимым проверенным сканером, не стирая коды.
3. Проверить single-frame; multi-frame проверять только при реально достаточном числе кодов либо на bench ECU/simulator.
4. Создать безопасный тестовый pending/confirmed code только штатной диагностической процедурой; не имитировать пропуски под высокой нагрузкой.
5. Подтвердить красное предупреждение GC9A01, текущий список и history table web UI; дать pending-коду исчезнуть, перезагрузить ESP32 и проверить его восстановление из LittleFS/NVS как historical, а не current.
6. Выполнить controlled power-cut проверки DTC journal в начале/середине/конце append и при ротации; подтвердить fallback на newest valid LittleFS/NVS record и bounds 20/60 секунд.
7. На simulator/bench вызвать NRC 0x78 с последующим final response, девять pending, final NRC 0x22, timeout, malformed и oversized/truncated ответ каждой pre-clear категории; не должно быть retransmit Mode 04/другого запроса во время pending, а запрещённый Mode 04 не должен появиться в CAN trace.
8. Попытаться clear при ненулевой скорости, работающем двигателе и низком/неизвестном напряжении — Mode 04 не должен появиться в CAN trace.
9. При engine OFF, ignition ON сохранить код и freeze-frame внешним сканером, затем осознанно выполнить clear; в CAN trace подтвердить строго `03→07→0A→durable checkpoint→01/0D→01/0C→01/42→04`, positive `0x44`, readiness reset, контрольное `03→07→0A` даже при paused polling и немедленный post-clear checkpoint.
10. Проверить, что permanent code не заявляется как стёртый.
11. После исправления причины пройти штатный drive cycle и подтвердить отсутствие возврата кода.

До прохождения пунктов 1–10 бинарник 0.4.1 не считать аппаратно подтверждённым для стирания DTC. Отозванный 0.4.0 не устанавливать.

## 9. Автоматические проверки

- `tools/test_obd_diagnostics.py` — 13 ASan/UBSan host-групп: форматирование, MIL latch, PID 0C/multi-ECU lock, single/multi-frame ISO-TP, NRC 0x78, P2*/absolute deadline, bounded pending, final NRC classification, truncation, transient history/restore, mandatory pre/post-clear preservation и safety-gated Mode 04;
- `tools/test_storage_recovery.py` — explicit LE/CRC snapshot, dirty-only 20/60 mirror, reboot, torn/partial/corrupt-readback boundaries, LittleFS-only/NVS-only checkpoint и dual failure;
- `tools/check_dtc_integration.py` — статическая связь scheduler, engine binding, P2*/absolute timing, durable checkpoints до и после Mode 04, dashboard, API, подтверждений и embedded UI;
- browser fixture — текущий `P0301`, исчезнувший historical `P0302`, состояние хранилищ, предупреждение misfire, scan header и точное clear confirmation body;
- PlatformIO `esp32s3_n16r8` — полная firmware compile/link/size проверка.
