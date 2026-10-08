# H2 Gauge 0.4.2-fix3a — isolated TWAI + SN65HVD230 self-test

This is a corrected rebuild of fix3. The fix3 page displayed `action confirmation header required` when starting self-test; the request had the bench checkbox token but omitted the server's `X-H2G-Action: twai-self-test` header. That response happened before the TWAI driver was changed, so **the self-test did not run and sent no frames**.

Fix3a includes that request header and has a distinct firmware manifest (`0.4.2-fix3a`) and overlay ID (`0.4.2-diag4-fix3a`) so it cannot be confused with the previous image.

**Target:** ESP32-S3-WROOM-1-N16R8 (`esp32s3-n16r8`)  
**Image:** `h2-gauge-v0.4.2-fix3a-esp32s3-n16r8.bin`  
**Build base:** `5fcac9fcee67b22ebab1f6064255cfa5f7558ae2`  
**Size:** 1,226,320 bytes  
**SHA-256:** `3ad9b25327ba74dca017acb3f45ae042f027e4a7c55fb1635bd70a0ce29c9e0f`

## Safety and self-test

**Do not run the self-test with CANH or CANL connected to the vehicle.** Disconnect both from the vehicle while keeping the SN65HVD230 powered and connected to GPIO16/TXD and GPIO17/RXD. The test emits three ID `0x555` frames in `TWAI_MODE_NO_ACK` with self-reception requested, checks the echoed frames, then restores the normal 500 kbit/s driver.

A pass supports the conclusion that the local TWAI/GPIO/transceiver path works; it does not test the vehicle harness or ACK from an ECU. A failure means check GPIO16-to-TXD, VCC/GND, RS, RXD-to-GPIO17, soldering, and then consider transceiver replacement. The self-test has not been validated on physical hardware yet.

The car-facing OBD probe remains blocked when GPIO16 reads LOW at idle. Given the previously attached data, do not run it on the vehicle until the TX path is inspected.

## Build checks

Build from the pinned base with `tools/build_diag4_fix3a.sh`. The diagnostic HTML source and embedded gzip must match. The source patch is isolated from the official release environment.
