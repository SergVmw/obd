# Field findings from 0.4.2-diag1 — 2026-10-07

## Captured state

Photographs taken at approximately 88–98 seconds uptime showed:

- TWAI `running`, driver ready, 500000 bit/s;
- TX/RX queue approximately `1 / 0` after the physical connection improved;
- TEC/REC `208 / 0`;
- bus error count `281`;
- arbitration-lost count `270`;
- retained TX `855`, retained RX `114`;
- cumulative software/alert error count `1181`;
- no accepted OBD response, no engine ECU lock and more than 1000 request timeouts;
- instantaneous GPIO16/CTX and GPIO17/CRX levels changed between captures, so the earlier CRX LOW observation was not a permanent static-low condition.

Retained CAN examples:

```text
TX 0x7DF  02 01 0C 00 00 00 00 00
RX 0x7DF  02 01 01 00 00 00 00 00
RX 0x7E8  03 41 0F 3E AA AA AA AA
RX 0x7E9  02 47 00 AA AA AA AA AA
```

## Conclusions

1. The WCMCU-230 receive path, vehicle CAN pair and 500 kbit/s timing are at least partially working: structurally valid 11-bit OBD traffic is received.
2. `0x7E8 03 41 0F 3E` is a valid Mode 01 PID 0F response. The firmware intentionally cannot lock the engine ECU from it; lock remains restricted to the first valid PID 0C response.
3. An RX frame with identifier `0x7DF` is evidence of another functional OBD requester (or an unexpected echo). The H2 Gauge records its own requests as TX, not RX. Likely candidates are another ELM/diagnostic adapter, an LPG controller configured for active OBD polling, telematics or another CAN diagnostic node.
4. The received Mode 07 response `47 00` also did not originate from automatic H2 Gauge diagnostics because the engine ECU was not locked and the DTC operation was idle.
5. Concurrent diagnostic traffic can explain unmatched request/response PIDs, timeouts and elevated arbitration/bus errors. All other OBD clients should be disconnected for the next controlled capture.
6. The high cumulative `CAN errors` value includes software enqueue failures after the TWAI TX queue becomes congested. It is not equivalent to the instantaneous hardware bus-error counter.

## LPG finding and diag1 defect

The captures showed:

```text
GPIO5 rawLevel=0
activeLow=true
electricalActive=true
candidateElectricalActive=false
stableElectricalActive=false
```

The raw/electrical result is valid and indicates that the PC817 input reached the ESP32 as active-low LPG ON. Candidate/stable were stale in `0.4.2-diag1`: automatic service mode returned before calling `LpgValveInput::update()`.

`0.4.2-diag2` corrects this by servicing the real LPG debounce state before the service-loop return. It also exposes separate counts for RX `0x7DF` functional requests and raw `0x7E8…0x7EF` replies.

A follow-up field observation showed that both official 0.4.2 and diag2 still displayed `UNKNOWN` fuel without PID 0C/RPM even while GPIO5 proved LPG ON. The user selected valve-authoritative fallback. `0.4.2-diag3` therefore additionally reports `LPG` for stable active GPIO5 and `95` for stable inactive GPIO5 when valve input is enabled; reliable stopped RPM retains `OFF` priority.

## Next controlled test

1. Remove the WCMCU-230 onboard R2 (`121`, 120 ohm) when connected as a stub to the already terminated vehicle CAN.
2. Disconnect all ELM327/scanners and diagnostic applications.
3. Test once with the BRC controller CAN connection temporarily disconnected, or otherwise prove whether BRC is the source of RX `0x7DF` requests.
4. Fully reboot the ESP32; clearing the CAN snapshot does not clear the TWAI TX queue or boot-cumulative software error counter.
5. With `0.4.2-diag3`, record the first 10–15 seconds and compare:
   - RX `0x7DF` count;
   - raw `0x7E8…0x7EF` replies;
   - TEC/REC, bus errors and arbitration lost;
   - PID 0C response and engine ECU lock;
   - GPIO5 raw/candidate/debounce/stable while LPG is switched OFF and ON.

## Follow-up capture — 2026-10-08

`0.4.2-diag3` installed successfully in APP1; APP0 retained diag1. The ordinary web status now displayed `LPG` while ECU remained unavailable, confirming the valve-authoritative display fallback.

The OBD form showed:

```text
maxRequestsPerSecond = 5
obdTimeoutMs = 120
```

Thus the rate reduction was applied, but the requested 500 ms timeout was not yet applied.

With no ELM327 connected, the aggregated CAN monitor repeatedly showed another RX functional requester and valid ECU replies. Representative frames included:

```text
RX 0x7DF  02 01 07 00 00 00 00 00
RX 0x7E8  03 41 07 72 AA AA AA AA
RX 0x7DF  02 01 01 00 00 00 00 00
RX 0x7E8  06 41 01 00 07 E1 00 AA
RX 0x7DF  01 03 00 00 00 00 00 00
RX 0x7E8  02 43 00 AA AA AA AA AA
RX 0x7E9  02 43 00 AA AA AA AA AA
```

The traffic proves that another active OBD master remains on the vehicle with ELM disconnected. It requests monitor status, fuel trims and DTC service data and receives valid responses from 0x7E8/0x7E9. The vehicle owner confirmed that the LPG installation requests the fuel-correction PIDs, so the observed external Mode 01 fuel-trim traffic (in particular PID 06/07 when present) is attributed to the BRC S32 Evo. This does not by itself prove that every observed external request, such as Mode 03 or PID 01, comes from BRC, although BRC is now the primary candidate. Passive head-unit control traffic alone would not normally use functional OBD ID 0x7DF.

The H2 monitor continued to show its own queued TX 0x7DF requests (for example PID 0B and PID 44), but no accepted PID 0C response or engine lock. Because the monitor records successful queue insertion rather than CAN-level completion, the physical H2 TX path still requires confirmation from TWAI TEC/error/queue counters.

Next non-invasive step: set timeout to 500 ms, save, fully reboot, avoid manual DTC scans, then capture the diagnostic root TWAI/OBD/CAN sections for 30 seconds.
