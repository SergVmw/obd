# H2PatchedWebServer

Project-local copy of Arduino-ESP32 `libraries/WebServer` from tag `2.0.17`,
commit `5e19e086c43d0fa5e5a596497ff8f11a0a43f6c2`.

Upstream: <https://github.com/espressif/arduino-esp32/tree/2.0.17/libraries/WebServer>

The original source headers and LGPL notices are retained. The H2 Gauge patch
is intentionally small and confined to raw-body parsing in `src/Parsing.cpp`:

1. the requested chunk length is
   `min(HTTP_RAW_BUFLEN, Content-Length - totalSize)`;
2. a local receive loop consumes only currently available bytes and feeds the
   `loopTask` watchdog on every pass;
3. transport idle time is bounded to two seconds, and the actual parser-client
   reference receives the same defensive socket/Stream timeout.

The first change prevents waiting for bytes outside a non-aligned HTTP body.
The receive loop also prevents a stalled or slow-trickling transfer from holding
one `Stream::readBytes()` call beyond H2 Gauge's five-second watchdog interval.
Calling `server.client().setTimeout()` from an
application callback is not equivalent because Arduino-ESP32 2.0.17 returns
that client by value.

`platformio.ini` uses `lib_ignore = WebServer`, so PlatformIO selects this
library without modifying its global framework package. `tools/check_ota_transport.py`
checks the patch shape and final-fragment invariant.
