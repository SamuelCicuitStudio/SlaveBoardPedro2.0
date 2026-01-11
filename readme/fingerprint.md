# Fingerprint Integration (R503) and Transport Bridge

This document describes how the fingerprint subsystem is wired, how commands/events flow through transport and ESP-NOW, and what behaviors are enforced.

## Hardware and role gating
- Sensor: R503 over UART (pins `R503_RX_PIN`, `R503_TX_PIN`, baud `R503_BAUD_RATE`).
- Powered/used only in lock role (`IS_SLAVE_ALARM=false`) and when `HAS_FINGERPRINT_KEY=true`.
- In alarm role or when HAS_FINGERPRINT is false, the Fingerprint object is not constructed; all FP commands return UNSUPPORTED and no FP events are emitted.

## Initialization
- `Fingerprint::begin()` probes the sensor without adopting (no password change). If trusted and present, it starts verify mode.
- `refreshCapabilities_()` in Device gates FP creation; alarm role forces FP off.

## Command handling (transport → FP)
- FP module opcodes (transport):
  - `0x01 VerifyOn`: start verify loop (5 Hz). Status: OK/BUSY/DENIED.
  - `0x02 VerifyOff`: stop verify loop. Status: OK.
  - `0x03 Enroll slot(u16)`: start enrollment task for slot. Status: OK/BUSY/DENIED.
  - `0x04 DeleteId slot(u16)`: delete template. Status + slot echoed.
  - `0x05 ClearDb`: wipe all templates. Status.
  - `0x06 QueryDb`: Response status + count(u16) + capacity(u16).
  - `0x07 NextId`: Response status + next free slot(u16).
  - `0x08 AdoptSensor`: claim virgin sensor (set secret PW). Status OK/APPLY_FAIL.
  - `0x09 ReleaseSensor`: set password to default (0). Status OK/APPLY_FAIL.
- Handled by `FingerprintHandler`; responses carry status plus optional data.

## Events (FP → transport → ESP-NOW)
- `0x0A MatchEvent`: id(u16), confidence(u8).
- `0x0B Fail/Busy/NoSensor/Tamper`: reason(u8) 0=match_fail,1=no_sensor,2=busy,3=tamper.
- `0x0C EnrollProgress`: stage(u8 1..8), slot(u16), status(u8 0=OK,1=FAIL/TIMEOUT).
- Adopt/Release results are sent as status events with op `0x08/0x09` + status.
- These events are emitted by `FingerprintScanner` via `sendFpEvent_()` and translated by ESP-NOW bridge to CommandAPI ACKs.

## Enrollment flow (ENROLL slot)
- Stages emitted as EnrollProgress:
  1 START, 2 CAP1, 3 LIFT, 4 CAP2, 5 STORING, 6 OK, 7 FAIL, 8 TIMEOUT.
- Enrollment task stops verify loop, walks two captures, builds/stores model, restarts verify if trusted.

## Verify loop
- Runs ~5 Hz when enabled and sensor is trusted.
- On match: MatchEvent with id/confidence; LED overlay FP_MATCH.
- On no match: Fail event (reason=match_fail); LED overlay FP_FAIL.
- Tamper handling: if sensor responds with wrong password, emits tamper (reason=3) at most every 20s; no unlock.

## ESP-NOW / CommandAPI bridge
- CMD_FP_* tokens are parsed by ESP-NOW and injected into transport (VerifyOn/Off, Enroll/Delete/Clear, QueryDb, NextId, Adopt/Release).
- Transport Responses/Events to destId=1 are mapped to CommandAPI ACKs:
  - Match → `ACK_FINGERPRINT_MATCH`
  - Fail → `ACK_FINGERPRINT_FAIL`
  - NoSensor → `ACK_FP_NO_SENSOR`
  - Busy → `ACK_FP_BUSY`
  - Tamper → `ACK_ERR_TOKEN`
  - Enroll stages → `ACK_FP_ENROLL_*` (START/CAP1/LIFT/CAP2/STORING/OK/FAIL/TIMEOUT)
  - QueryDb → `ACK_FP_DB_INFO`
  - NextId → `ACK_FP_NEXT_ID`
  - DeleteId → `ACK_FP_ID_DELETED`
  - ClearDb → `ACK_FP_DB_CLEARED`
  - Adopt/Release → `ACK_FP_ADOPT_OK/FAIL`, `ACK_FP_RELEASE_OK/FAIL`
- In alarm role or when FP absent, these commands return UNSUPPORTED and no ACKs are emitted from FP paths.

## Persistence and config
- No FP templates are stored in NVS; the sensor flash holds templates.
- HAS_FINGERPRINT_KEY gates FP usage; low battery (unpaired) may disable FP locally to save power.

## Safety/robustness
- No local unlock: FP match only requests unlock from master (no motor action locally).
- Verify/enroll tasks are mutually exclusive; adopt/release stops all FP tasks before changing passwords.
- Tamper/no-sensor conditions are throttled to avoid spamming.

## Related CommandAPI updates
- Shock internal probe failures return `ACK_SHOCK_INT_MISSING (0xD9)`; see `readme/device.md` and `readme/transport.md`.
