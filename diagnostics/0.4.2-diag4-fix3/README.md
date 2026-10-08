# H2 Gauge 0.4.2-fix3 — TWAI + SN65HVD230 self-test

**Target:** ESP32-S3-WROOM-1-N16R8 (`esp32s3-n16r8`), Arduino-ESP32 2.0.17 / ESP-IDF 4.4.x.

**Firmware manifest:** `0.4.2-fix3`  
**Diagnostic overlay ID:** `0.4.2-diag4-fix3`  
**Image:** `h2-gauge-v0.4.2-fix3-esp32s3-n16r8.bin`  
**Size:** 1,226,384 bytes  
**SHA-256:** `80c8742dcaed9aa7dd8881c2cfee35183ebb1619b340d20b05cb2a0cf1f73336`

This is an app image for the existing OTA updater, not a bootloader or merged factory image. It does not replace the official `0.4.2` release.

## What changed for the field evidence

The supplied diagnostics showed a queued `0x7E0` frame followed by `tx_failed +1`, `bus_error +1`, and `TEC +8`, while RX traffic continued. The diagnostics also sampled GPIO16/TWAI_TX LOW while the controller was otherwise idle.

Fix3 therefore:

- separates `txDriverAccepted` (`twai_transmit()` returned `ESP_OK`) from actual controller completion (`TWAI_ALERT_TX_SUCCESS`);
- marks a probe as `tx_failed` immediately when bus-error/TX-failed/TEC deltas appear, instead of displaying the misleading `no_response · TX ok`;
- blocks the car-facing OBD probe while GPIO16 reads LOW at idle and displays a warning to inspect GPIO16 → SN65HVD230 TXD before retrying;
- removes display/backlight pin rows from the diagnostic page and hardware JSON;
- adds a separate, explicitly confirmed **bench-only** TWAI self-test.

## Bench-only self-test — disconnect the car first

**Do not run this test with CANH/CANL connected to the vehicle.** The self-test transmits three test frames with ID `0x555`; self-reception does not mean that no electrical frames are placed on CANH/CANL.

1. Power down or otherwise safely isolate the device from the vehicle CAN wiring. Disconnect **both CANH and CANL** from the vehicle while leaving the SN65HVD230 connected to ESP32 GPIO16/17.
2. Keep the transceiver powered and its TXD/RXD/RS wiring intact. For a bench bus, use a suitable local CANH–CANL termination; do not reconnect the vehicle during the test.
3. Open the diagnostic page, tick the explicit isolated-bench confirmation, and run **TWAI + SN65HVD230 self-test**.
4. The firmware temporarily installs TWAI in `TWAI_MODE_NO_ACK`, sends three standard frames with `self=1`, checks that each matching frame is received, then restores the normal 500 kbit/s diagnostic driver.
5. Record sent/received counts, GPIO16/GPIO17 readbacks, and whether the normal TWAI driver was restored.

A **PASS** is strong evidence that the TWAI driver, GPIO-matrix routes, and local TX/RX path through the connected transceiver can operate together. It does not validate the car harness, vehicle-side termination, the vehicle connector, or ACK from an ECU. A **FAIL** localizes the problem to the ESP32/TWAI/GPIO/transceiver path but does not by itself distinguish a solder joint from a failed SN65HVD230. Probe GPIO16, SN65HVD230 pin 1 (TXD), CANH/CANL, pin 4 (RXD), and GPIO17 with a scope/logic analyzer to isolate the exact segment.

Espressif describes the same No-Ack + self-reception approach as its TWAI self-test for checking the controller's connections to an external transceiver: <https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32s3/api-reference/peripherals/twai.html>.

## Car-facing OBD probe

The separate read-only physical-address probe remains manual and single-shot. Fix3 refuses to start it if GPIO16 reads LOW while idle, and it stops immediately on TX-failed, bus-error, or TEC growth. **Given the field snapshot's GPIO16 LOW and bus error, do not run this probe again on the vehicle until the TX line and transceiver have been checked.**

## Build and verification

Base source revision: `5fcac9fcee67b22ebab1f6064255cfa5f7558ae2`. Apply `hardware-diagnostics.patch` to that clean revision and build environment `esp32s3_n16r8_diag`.

Validated in this workspace:

- diagnostic firmware build: PASS;
- normal release environment build: PASS;
- `tools/test_obd_diagnostics.py`: 15 groups passed;
- manifest check: `0.4.2-fix3 / esp32s3-n16r8`;
- image SHA-256: as listed above.

The self-test has been compiled, **not run on physical hardware**. Run it only on the isolated bench setup described above.
