# H2 Gauge 0.3.8 — расчёты и pin-to-pin netlist

Этот документ фиксирует электрические соединения для текущей платы **ESP32-S3 DevKitC-1 compatible с модулем ESP32-S3-WROOM-1-N16R8**. Графические листы старой ревизии удалены из `docs`; актуальным источником подключения являются этот netlist и [`wiring.md`](wiring.md). Для LDR добавлена отдельная [принципиальная схема](ambient-light-circuit.svg).

> Это reference-схема инженерного прототипа, а не сертифицированная автомобильная плата. До постоянной установки необходимы тепловая, EMC, ISO 7637-2/ISO 16750-2 и натурная проверки.

## 1. Состав устройства

### Коробка питания возле OBD

- `J201` — OBD-II;
- `F201` — предохранитель 1 A;
- `D201` — SMCJ30CA;
- `U201` — TPS26600PWP;
- `L201/C204/C205` — выходной фильтр;
- `A201` — MP1584, выход 5.00 В;
- `D202…D205`, `R213/R214`, `U203` — изолированный вход клапана LPG.

### Коробка дисплея

- `A301` — ESP32-S3 DevKitC-1 N16R8;
- `U301` — SN65HVD230 рядом с ESP32-S3;
- `J302` — GC9A01;
- `SW301` — единственная кнопка MODE/WAKE;
- `Q301/Q302` — опциональный high-side ключ BLK только для подходящего силового входа; при подтверждённом штатном logic-BLK не нужен;
- `LDR301`, `R307/R308`, `C306` — вход освещённости ADC1;
- pull-up и RC-фильтр выхода PC817.

## 2. OBD-II

| J201 pin | Сеть | Соединение |
|---:|---|---|
| 16 | `VBAT_OBD` | F201 1 A, затем входная защита |
| 4 | `CHASSIS_GND` | отдельный провод до общей точки |
| 5 | `PWR_RTN` | входной возврат TPS26600 |
| 6 | `CAN_H` | витая пара до U301 pin 7 |
| 14 | `CAN_L` | витая пара до U301 pin 6 |

Дополнительный терминатор 120 Ω — `DNP`. Если он установлен на готовом модуле трансивера, удалить или разомкнуть.

## 3. Защищённое питание

### Силовой путь

```text
J201.16 VBAT_OBD
 → F201 1 A
 → VBAT_FUSED
 → D201 SMCJ30CA + C201 100 нФ/100 В + C202 1 мкФ film/100 В
 → U201 TPS26600PWP
 → C204 10 мкФ/63 В
 → L201 22 мкГн, Isat ≥ 1 A
 → C205 47 мкФ/63 В
 → A201 MP1584
 → C206 100 мкФ/10 В + C207 100 нФ
 → +5V_PROTECTED
```

MP1584 предварительно настроить на `5.00 В` без подключённой ESP32-S3.

### TPS26600PWP

| Pin | Имя | Соединение |
|---:|---|---|
| 1, 2 | IN | `VBAT_FUSED` |
| 3 | UVLO | узел R201/R202 |
| 4, 13 | NC | NC |
| 5 | OVP | узел R202/R203 |
| 6 | MODE | `PWR_RTN`, active current limiting + auto-retry |
| 7 | SHDN | R204 100 кОм к IN |
| 8 | RTN | `PWR_RTN` |
| 9 | GND | `GND_PROTECTED` |
| 10 | IMON | NC |
| 11 | ILIM | R205 16.2 кОм к `PWR_RTN` |
| 12 | dVdT | C203 22 нФ к `PWR_RTN` |
| 14 | FLT | NC |
| 15, 16 | OUT | C204/L201 и вход MP1584 |
| EP | PowerPAD | полигон `PWR_RTN`, тепловые vias |

**RTN pin 8 нельзя напрямую соединять с GND pin 9.** UVLO, OVP, MODE, ILIM, dVdT и PowerPAD возвращаются в `PWR_RTN`; нагрузка и выходные конденсаторы — в `GND_PROTECTED`.

### UVLO/OVP

```text
IN — R201 487 кОм — UVLO — R202 90.9 кОм — OVP — R203 30.1 кОм — RTN
```

При `VTH≈1.19 В`:

```text
RΣ = 608.0 кОм
VOV ≈ 1.19 × 608.0 / 30.1 ≈ 24.03 В
VUV rising ≈ 1.19 × 608.0 / (90.9 + 30.1) ≈ 5.98 В
VUV falling ≈ 5.53 В
```

### Ограничение тока

```text
RILIM = 16.2 кОм
ILIM ≈ 12 / 16.2 ≈ 0.74 A
```

Для ESP32-S3 с активным Wi‑Fi, TFT и подсветкой нужно измерить реальный пиковый ток. Если 0.74 A недостаточно, нельзя просто уменьшать RILIM без проверки F201, TPS26600, L201, MP1584, разъёмов, проводов и теплового режима.

## 4. Межблочный разъём

Рекомендуемый `J202`, шесть контактов:

| Pin | Сеть | Назначение |
|---:|---|---|
| 1 | `+5V_PROTECTED` | питание ESP32-S3 DevKitC-1 через 5V |
| 2 | `POWER_GND` | силовой возврат |
| 3 | `CAN_H` | витая пара |
| 4 | `CAN_L` | витая пара |
| 5 | `LPG_SENSE` | open-collector выход PC817 |
| 6 | `SIGNAL_GND` | возврат LPG signal |

Рекомендуемые пары:

```text
J202.1 / J202.2 = +5V / POWER_GND
J202.3 / J202.4 = CAN-H / CAN-L
J202.5 / J202.6 = LPG_SENSE / SIGNAL_GND
```

`POWER_GND` и `SIGNAL_GND` соединяются в общей точке коробки дисплея. Отдельный 3.3-вольтовый провод не прокладывается.

## 5. ESP32-S3 pin map

| Сеть | A301 GPIO |
|---|---:|
| `BUTTON_WAKE` | GPIO4 |
| `LPG_SENSE` | GPIO5 |
| `LIGHT_ADC` | GPIO6 / ADC1_CH5 |
| `BL_PWM` | GPIO7 |
| `TFT_CS` | GPIO10 |
| `TFT_DC` | GPIO11 |
| `TFT_RST` | GPIO12 |
| `TFT_MOSI` | GPIO13 |
| `TFT_SCLK` | GPIO14 |
| `TWAI_TX` | GPIO16 |
| `TWAI_RX` | GPIO17 |

GPIO19/20 зарезервированы для native USB. GPIO35/36/37 заняты Octal PSRAM. GPIO0/3/45/46 не используются из-за boot/strapping функций. GPIO38 не используется из-за RGB LED распространённой DevKitC-1 v1.1.

## 6. SN65HVD230 рядом с ESP32-S3

| U301 pin | Имя | Соединение |
|---:|---|---|
| 1 | D/TXD | A301 GPIO16 |
| 2 | GND | `GND_PROTECTED` |
| 3 | VCC | локальные `+3V3`, C301 100 нФ и C302 1 мкФ |
| 4 | R/RXD | A301 GPIO17 |
| 5 | Vref | NC |
| 6 | CANL | J202.4 / OBD pin 14 |
| 7 | CANH | J202.3 / OBD pin 6 |
| 8 | Rs | GND, high-speed mode |

C301/C302 расположить непосредственно возле pins 3/2. TX/RX между U301 и ESP32-S3 должны быть короткими. CAN-H/CAN-L от OBD вести витой парой.

## 7. GC9A01

Нумерация ниже соответствует распространённому восьмивыводному модулю; ориентироваться прежде всего по надписям на конкретной плате.

| J302 pin | Сигнал | Соединение |
|---:|---|---|
| 1 | GND | `GND_PROTECTED` |
| 2 | VCC | `+3V3` |
| 3 | SCL/SCLK | GPIO14 |
| 4 | SDA/MOSI | GPIO13 |
| 5 | RES/RST | GPIO12; R301 10 кОм к GND |
| 6 | DC | GPIO11 |
| 7 | CS | GPIO10 |
| 8 | BLK/BL | GPIO7 для подтверждённого штатного active-HIGH logic-входа |

Развязка TFT:

```text
C303 = 100 нФ между 3V3/GND
C304 = 10 мкФ между 3V3/GND
R301 = 10 кОм от TFT RST/GPIO12 к GND
```

## 8. Кнопка MODE/WAKE

```text
+3V3 → R302 10 кОм → BUTTON_WAKE/GPIO4
BUTTON_WAKE/GPIO4 → SW301 → GND
```

Внутренняя `INPUT_PULLUP` также включена программно. Внешняя R302 задаёт состояние во время reset/deep sleep и полезна при длинном проводе. Кнопка active LOW. GPIO4 используется как EXT0 wake source, таймер пробуждения — 30 секунд.

## 9. Вход клапана LPG

`J203` подключается параллельно двум проводам одной катушки BRC. Разные клапаны не объединять.

Полный мост:

```text
VALVE_A → D202 → BRIDGE+
VALVE_B → D203 → BRIDGE+
BRIDGE− → D204 → VALVE_A
BRIDGE− → D205 → VALVE_B
```

Далее:

```text
BRIDGE+
 → R213 2.2 кОм / 0.25 Вт pulse-rated
 → R214 2.2 кОм / 0.25 Вт pulse-rated
 → PC817 pin 1 anode
PC817 pin 2 cathode → BRIDGE−
```

При 12…14.4 В ожидаемый ток LED приблизительно 2…2.7 мА. Использовать PC817C/EL817C с гарантированным CTR и проверить экземпляр при минимальном напряжении.

Изолированная сторона:

```text
PC817 pin 4 collector → J202.5 LPG_SENSE → GPIO5
+3V3 → R303 10 кОм → GPIO5
GPIO5 → C305 100 нФ → SIGNAL_GND
PC817 pin 3 emitter → J202.6 SIGNAL_GND
```

При включённой катушке GPIO5 получает LOW.

## 10. High-side PWM подсветки

BLK имеющегося дисплея уже соединён с GPIO7; на модуле есть транзистор. Если BLK — его подтверждённый logic-вход, GPIO7 управляет им напрямую и Q301/Q302 не ставятся. Проверить полярность и входной ток; ток LED не должен идти через GPIO.

Ниже reference-вариант только для подходящего силового active-HIGH BLK с ограничением тока; он не является обязательным дополнением к штатному logic-входу:

```text
+3V3 → Q301 AO3401A source pin 2
Q301 drain pin 3 → TFT BLK
Q301 gate pin 1 → R304 100 кОм → +3V3
Q301 gate pin 1 → Q302 MMBT3904 collector pin 3
GPIO7 → R305 4.7 кОм → Q302 base pin 1
Q302 base → R306 100 кОм → GND
Q302 emitter pin 2 → GND
```

GPIO7 HIGH включает Q302, тот опускает gate Q301, и Q301 подаёт 3.3 В на BLK. R304/R306 гарантируют выключенное состояние до настройки GPIO7.

## 11. Flash, PSRAM и USB

Проект собирается как:

```text
environment: esp32s3_n16r8
flash mode: QIO
PSRAM mode: OPI
memory type: qio_opi
flash: 16 MB
PSRAM: 8 MB
```

Native USB использует GPIO19/20. `ARDUINO_USB_MODE=1` и `ARDUINO_USB_CDC_ON_BOOT=1`; serial monitor рассчитан на native USB-C. Первая полная прошивка должна записать bootloader, `partitions_16mb_ota.csv` и приложение.

## 12. Температура

Модуль N16R8 с Octal PSRAM имеет паспортный верхний предел окружающей температуры +65 °C без ECC. Для установки под солнцем требуется измерить температуру внутри корпуса; DevKitC-1 не считается automotive-qualified.

## 13. Проверка перед автомобилем

1. Проверить маркировку N16R8 на модуле.
2. Проверить 5.00 В и 3.3 В мультиметром.
3. Подтвердить в serial log Flash≈16 МБ и PSRAM≈8 МБ.
4. Проверить TFT и резистор RST 10 кОм.
5. Проверить кнопку GPIO4 и wake.
6. Проверить питание U301 3.3 В и отсутствие терминатора 120 Ω.
7. Проверить CAN сначала на стоящем автомобиле.
8. Проверить LPG-вход лабораторным напряжением, затем осциллографом на автомобиле.
9. Проверить падение 5 В по межблочному кабелю при Wi‑Fi и полной подсветке.
10. Провести тепловой тест закрытого корпуса.

## 14. Datasheet

- ESP32-S3-WROOM-1/1U: <https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf>
- ESP32-S3-DevKitC-1: <https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html>
- TPS2660: <https://www.ti.com/lit/ds/symlink/tps2660.pdf>
- SN65HVD230: <https://www.ti.com/lit/ds/symlink/sn65hvd230.pdf>

## Вход освещённости LDR (config schema 6)

[Принципиальная схема](ambient-light-circuit.svg). Устанавливается в коробке дисплея:

```text
+3V3 → LDR301 → LIGHT_DIV
LIGHT_DIV → R307 22 кОм → GND
LIGHT_DIV → R308 1 кОм → LIGHT_ADC / A301 GPIO6 (ADC1_CH5)
LIGHT_ADC → C306 100 нФ → GND
```

`V_LIGHT_DIV = 3.3 × 22 кОм / (R_LDR + 22 кОм)`. Светлее → ADC выше. R308/C306 возле A301; никаких автомобильных 12 В или 5 В на LIGHT_ADC. Номинал 22 кОм уточнить по реальному сопротивлению LDR и полезному диапазону ADC. Датчик не должен видеть собственную подсветку. GPIO6/ADC1 используется и при активном Wi-Fi; рабочее разрешение 12 бит, attenuation 11 dB.

Один пассивный делитель не даёт достоверного определения обрыва. Defaults оставляют «Всегда день», автоматика включается после проверки монтажа. [Алгоритм, пороги и автокалибровка](BRIGHTNESS_GUIDE.md).
