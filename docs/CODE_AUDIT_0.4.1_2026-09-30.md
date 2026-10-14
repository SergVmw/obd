# Проверка замечаний ревизии и дополнительный аудит 0.4.1

Дата: 2026-09-30. Проверено состояние дерева `/home/user/H2-Gauge` версии
`0.4.1` на момент аудита. Аудит сфокусирован на OBD/DTC, Mode 04, persistence,
OTA diagnostics и CRC из предоставленного списка замечаний.

## Статус после аудита

Все три подтверждённых production-дефекта исправлены отдельным release 0.4.2;
бинарники и SHA-256 0.4.1 не подменялись. Исправления:

- per-response `authoritative = !assemblyTruncated`: truncated prefix добавляет
  увиденные DTC, но не снимает presence с unseen tail; API требует
  `!state.truncated` и три статуса `Complete`;
- `RuntimePersistence::factoryReset()` после обязательной успешной очистки
  старых namespaces принимает `journalOk || nvsOk`;
- отдельный private `postClearScanScheduled_` сохраняет deadline, равный нулю,
  а проверка остаётся до pause-guard automatic polling.

Regression-тесты 0.4.2 покрывают код в отброшенном хвосте и новый код в
префиксе, API invariant, NVS put failure после успешного clear, exact-zero
rollover и официальный Mode 04 path `7F 04 78` → `44`.

## Итог

Большинство исходных замечаний относилось к состоянию до проверявшейся 0.4.1 и уже было исправлено.
Подтверждённых актуальных ошибок высокой критичности в перечисленных местах не
осталось. Дополнительная проверка обнаружила одну новую ошибку средней
критичности в обработке усечённого DTC-ответа, один сохраняющийся риск выбора
engine ECU и две низкоприоритетные проблемы.

На этапе выявления дефектов файлы прошивки не изменялись. Последующее исправление
production-кода выпущено новой версией 0.4.2, без замены бинарников 0.4.1 in-place.

## Статус исходных замечаний

### NRC 0x78 ResponsePending — исправлено

- `src/obd_diagnostics.cpp` отделяет NRC `0x78` от финальных NRC и продолжает тот
  же запрос без retransmit.
- P2*=5000 мс, абсолютный предел=15000 мс, максимум восемь pending.
- `include/obd_request_timing.h` использует wrap-safe elapsed arithmetic и не
  сдвигает абсолютную эпоху при повторном pending.
- `src/obd_client.cpp` обновляет inactivity epoch только для принятого кадра и
  переключает таймер на P2* по отдельному событию state machine.
- NRC `0x11/0x12` остаются `unsupported`, прочие финальные NRC —
  `negative_response`.

Host-тест покрывает pending/final response, count bound, P2*, absolute deadline
и rollover. Во время аудита последовательность `7F 04 78 -> 44` сначала прошла
во временном ASan/UBSan probe; в 0.4.2 она закреплена в официальном Mode 04
regression и завершается без повторной передачи команды.

### Контракт checkpoint — исправлено

- `RuntimePersistence::checkpoint()` возвращает успех при read-back хотя бы
  одной запрошенной durable-копии.
- `DtcHistoryPersistence::checkpoint()` имеет ту же семантику.
- LittleFS-only, NVS-only, corrupt readback и отказ обеих копий выполняются на
  реальном production-коде persistence под ASan/UBSan.
- Health каждого backend остаётся раздельным и не маскируется общим успехом.

### Выбор engine ECU — точный старый дефект исправлен, общий риск остаётся

Произвольный первый ответ/NRC больше не может выбрать ECU. Lock выполняется
только после структурно валидного positive Mode 01 PID 0C; затем PID 01,
03/07/0A/04 и обычная telemetry принимаются только от immutable response ID.

Однако выбор всё ещё производится по **первому валидному PID 0C**. Если TCM или
другой модуль действительно поддерживает PID 0C и отвечает раньше ECM, он может
стать immutable target. Неудачный последующий physical PID 01 не снимает lock и
не запускает перебор кандидатов. Это документированный текущий алгоритм, но его
надёжность для Haval должна быть подтверждена CAN capture/аппаратной приёмкой.
Для полностью общего решения нужен bounded сбор кандидатов и physical
qualification (как минимум PID 01 + Mode 03), либо подтверждённый приоритет
response ID конкретного автомобиля.

### Окно после positive Mode 04 — исправлено консервативно

Positive `0x44` удаляет только текущие RAM stored/pending entries, но не очищает
`DtcHistoryEntry::lastPresentKinds`. Поэтому обрыв питания до post-clear scan
восстановит последнее подтверждённое присутствие, а не ложно объявит код
исчезнувшим. Через 1,5 с выполняется manual post-clear 03/07/0A, после которого
main немедленно делает durable checkpoint. Timeout/NRC/unsupported категории не
очищает её последнее подтверждённое presence.

### Post-clear при paused polling — намеренный контракт, не текущая ошибка

`periodicScanAllowed=false` блокирует только автоматические periodic scans.
Явно запрошенный scan/clear и обязательный post-clear tail продолжаются в
service mode. Комментарии `obd_diagnostics.cpp` и `obd_client.cpp`, state machine
и тест теперь согласованы.

### Дублирование persistence — остаётся техническим долгом

Runtime и DTC persistence по-прежнему существенно дублируют segment scan,
append/readback, NVS mirror, rotation и recovery. Это не текущая runtime-ошибка,
но уже привело к различию factory-reset semantics, описанному ниже. Рефакторинг
в общий journal engine целесообразен отдельно, после фиксации fault-injection
тестами всех различий кодеков и размеров сегментов.

### Низкие замечания

- Flow Control теперь создаётся только если после First Frame остаются байты.
  DLC=8 для classic CAN ISO-TP FC является корректным padding-контрактом.
- `payload` и `length` проверяются до `payload[0]`; мёртвого `length < 1` после
  разыменования больше нет.
- Ошибки аргумента/порядка OTA state machine не выставляют
  `storageHealthy=false`; это происходит только при невозможности открыть или
  записать NVS.
- CRC32 переведён на 16-entry nibble table: два lookup вместо восьми побитовых
  шагов на байт. Повторная проверка payload остаётся намеренной защитой от
  повреждения; generation-cache не реализован.
- Static grep gates всё ещё существуют, но NRC/DTC, persistence и OTA diagnostics
  дополнительно тестируются через скомпилированный production-код. Полного host
  исполнения Arduino `WebServer`/TWAI/ESP OTA драйвера нет, поэтому hardware
  acceptance всё ещё обязательна.

## Подтверждённая в 0.4.1 ошибка средней критичности — исправлена в 0.4.2

### Усечённый DTC response ошибочно считался авторитетным для history

Связанные места:

- `src/obd_diagnostics.cpp`: после `responseExpected_ > kResponseCapacity`
  выставляется `state_.truncated`, но затем вызывается обычный
  `replaceCategory()`;
- `replaceCategory()` всегда вызывает `updateHistoryCategory()`;
- `updateHistoryCategory()` очищает `lastPresentKinds` у каждого ранее
  присутствовавшего кода, которого нет в сохранённой части payload;
- `src/service_portal.cpp` вычисляет `history.currentStateKnown` только по трём
  статусам `Complete` и не проверяет `!state.truncated`.

В результате код, находившийся в невместившемся хвосте ответа, может быть ложно
помечен исчезнувшим. Web API одновременно может сообщить, что current state
подтверждён.

Ошибка воспроизведена отдельным ASan/UBSan-тестом на production
`src/obd_diagnostics.cpp`: первый scan сохранил P0301, второй получил полный
97-байтный ISO-TP payload, где P0301 находился в хвосте после 47 нулевых DTC-пар.
96-байтный bounded buffer не сохранил последнюю пару, после чего текущий код
очистил `lastPresentKinds`, хотя P0301 присутствовал в полном ECU response.

Небезопасный Mode 04 при этом не отправляется: pre-clear gate отдельно проверяет
`state_.truncated`. Ошибка относится к достоверности normal/history display и
последующей persistence.

Рекомендуемое исправление:

1. Передавать в history update признак `authoritative = !assemblyTruncated`.
2. При неавторитетном ответе добавлять/обновлять фактически увиденные коды, но
   не очищать отсутствие ранее известных кодов.
3. Добавить `&& !state.truncated` в `history.currentStateKnown`.
4. Сохранить regression-тест с кодом в невместившемся хвосте.
5. Аналогично считать current state неизвестным при переполнении 32-entry списка.

Все пять пунктов реализованы и закреплены regression/static gates в 0.4.2.

## Дополнительные низкоприоритетные проблемы 0.4.1 — исправлены в 0.4.2

### Несогласованный результат RuntimePersistence::factoryReset

В 0.4.1 `runtime_persistence.cpp` возвращал:

```cpp
(journalOk || nvsOk) && nvsOk
```

после успешного удаления старого NVS. Это алгебраически требовало новую NVS-копию,
даже если новый reset sequence уже был записан и прочитан из LittleFS. Возможный
результат — HTTP 500 после фактически durable journal reset при отказе повторной
NVS записи. В 0.4.2 сохранены обязательные `latestValid_`, `filesCleared` и
`nvsCleared`, а финальное требование исправлено на `journalOk || nvsOk`; сценарий
успешного clear и отказа последующего NVS put закреплён fault injection.

### Sentinel `postClearScanDueAt_ == 0` на точной границе millis rollover

В 0.4.1 post-clear deadline хранился как `now + 1500`, а ноль одновременно
означал «не запланировано». Если сумма ровно переполнялась в ноль, автоматический
tail не стартовал до ручного scan/reboot. Вероятность — одна миллисекунда
примерно раз в 49,7 суток непрерывной работы. В 0.4.2 добавлен отдельный private
boolean schedule active; exact-zero rollover проходит behavioral regression, а
manual continuation по-прежнему проверяется до automatic polling guard.

## Выполненные проверки

На этапе аудита успешно прошли исходные 13 OBD-групп, storage/OTA suites и
static gates; отдельный truncation probe ожидаемо воспроизвёл ошибку 0.4.1.
После исправления 0.4.2 успешно выполнены:

- `python tools/test_obd_diagnostics.py` — 15 ASan/UBSan групп;
- `python tools/check_dtc_integration.py`;
- `python tools/test_storage_recovery.py` с новым factory-reset fault injection;
- `python tools/check_storage_integration.py`;
- `python tools/test_ota_diagnostics.py` — 13 групп;
- `python tools/check_ota_transport.py`;
- brightness/persistence snapshot, static/font/browser gates, clean firmware
  build, manifest, package checksums, app/build equality и factory layout.

Truncation и Mode 04 pending probes перенесены в официальный suite.
