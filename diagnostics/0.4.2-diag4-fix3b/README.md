# H2 Gauge 0.4.2-fix3b — one confirmed read-only OBD probe

Fix3b is based on the pinned firmware source revision `5fcac9fcee67b22ebab1f6064255cfa5f7558ae2` plus the diagnostic overlay in `hardware-diagnostics.patch`.

**Target:** ESP32-S3-WROOM-1-N16R8 (`esp32s3-n16r8`)  
**Firmware:** `h2-gauge-v0.4.2-fix3b-esp32s3-n16r8.bin`  
**Size:** 1,227,392 bytes  
**SHA-256:** `c105eed1be445f7e07e33c2cac9670043375fd727e14f0f815693d9bcbf2f509`  
**Manifest:** `0.4.2-fix3b / esp32s3-n16r8`

## Safety/operating sequence

1. On an **isolated bench only**, physically disconnect CANH and CANL from the vehicle while leaving the SN65HVD230 powered and connected to GPIO16/TXD and GPIO17/RXD. Tick the isolated-bench confirmation and run the TWAI self-test. It transmits three `0x555` frames in `TWAI_MODE_NO_ACK`, checks self-reception, and restores the normal 500 kbit/s driver.
2. A successful self-test sets a RAM-only PASS latch. A reboot clears it, so repeat the isolated bench test after every reboot. After PASS, wait at least 10 seconds of passive observation.
3. On a parked vehicle, tick the separate on-car confirmation and manually start the probe. It emits exactly one read-only single-shot frame: `0x7E0 02 01 0C 00 00 00 00 00` (Mode 01 PID 0C). Hardware/software automatic retries are disabled; there is no DTC clear, control command, or automatic OBD polling.
4. A bus error, TX failure, TEC increase, or arbitration loss locks out further vehicle probes until reboot. Investigate the physical TX path before any new attempt; after reboot the isolated bench self-test is required again.

The raw GPIO16/TWAI_TX LOW readback remains a warning after bench PASS, not a standalone blocking criterion. It is only a digital pad readback and is not a substitute for checking GPIO16/TXD with an oscilloscope. The isolated self-test does not establish ECU ACK or validate the vehicle harness. **Do not run the self-test with CANH/CANL connected to a vehicle.** No fix3b self-test or on-car probe has been run on physical hardware as part of this build.

## Build and checks

Rebuild from the pinned base with:

```sh
tools/build_diag4_fix3b.sh
```

The script builds the diagnostic environment, validates the ESP32-S3 manifest, writes the binary, and refreshes `SHA256SUMS.txt`. The source HTML is `hardware-diagnostics.html`; `hardware_diagnostics_gz.h` is the embedded compressed copy. The diagnostic overlay does not replace release binaries.

Verified in this build: PlatformIO environment `esp32s3_n16r8_diag` succeeded; the firmware manifest/image check passed; OBD diagnostics tests (15 groups), DTC integration checks, OTA transport checks, and `git diff --check` passed. `check_packaged_artifacts.py` was not applicable to this diagnostic-only build because it expects the separate release environment output under `.pio/build/esp32s3_n16r8/`.
