# Fingerprint Integration (R503) and Transport Bridge

This document describes how the fingerprint subsystem is wired, how commands/events flow through transport and ESP-NOW, and what behaviors are enforced.

## Behavior requirements

- Enrollment must be step-by-step and wait for user actions (place -> lift -> place again).
- Verify and enrollment are mutually exclusive; verify is paused during enrollment and resumed after if it was running and the sensor is trusted.
- Adopt/Release are explicit master commands; the slave performs the action and replies with an ACK when complete (no extra broadcast event).
- If tampered (wrong password), fingerprint is disabled and reported to the master until Adopt/Release is performed.
- In Test Mode (`CMD_ENTER_TEST_MODE`), verify can run for diagnostics and match/fail is streamed to the master.
- On boot, the sensor is probed to determine adoption state; when **unpaired**, an adopted sensor is automatically **released** to default so the master can decide adoption after pairing.
- The per-device fingerprint password is **derived from the slave MAC** at boot and cached in RAM (not stored in NVS).

## Hardware and role gating

- Sensor: R503 over UART (pins `R503_RX_PIN`, `R503_TX_PIN`, baud `R503_BAUD_RATE`).
- Powered/used only in lock role (`IS_SLAVE_ALARM=false`) and when `HAS_FINGERPRINT_KEY=true`.
- In alarm role or when HAS_FINGERPRINT is false, the Fingerprint object is not constructed; all FP commands return UNSUPPORTED and no FP events are emitted.

## Initialization

- `Fingerprint::begin()` probes the sensor without adopting (no password change). If trusted and present, it starts verify mode.
- Adoption detection is implicit:
  - **Derived password accepted** -> adopted/trusted.
  - **Default password accepted** -> unadopted/virgin (treated as untrusted).
  - **Neither accepted** -> missing or foreign-locked (treated as tampered/untrusted).
- If `DEVICE_CONFIGURED=false` (unpaired) **and** the sensor is adopted (secret accepted), the slave releases it to the default password at boot and keeps verify off.
- The derived password is `Trunc32(HMAC-SHA256(SECRET_KEY, slaveMac || "FP-V1"))`, with 0 reserved for the factory default password.
- `refreshCapabilities_()` in Device gates FP creation; alarm role forces FP off.

## Command handling (transport -> FP)

- FP module opcodes (transport):
  - `0x01 VerifyOn`: start verify loop (~5 Hz). Status: OK/BUSY/DENIED.
  - `0x02 VerifyOff`: stop verify loop. Status: OK.
  - `0x03 Enroll slot(u16)`: start enrollment task for slot. Status: OK/BUSY/DENIED.
  - `0x04 DeleteId slot(u16)`: delete template. Status + slot echoed.
  - `0x05 ClearDb`: wipe all templates. Status.
  - `0x06 QueryDb`: Response status + count(u16) + capacity(u16).
  - `0x07 NextId`: Response status + next free slot(u16).
  - `0x08 AdoptSensor`: claim virgin sensor (set secret PW). Status OK/APPLY_FAIL.
  - `0x09 ReleaseSensor`: set password to default (0). Status OK/APPLY_FAIL.
- Handled by `FingerprintHandler`; responses carry status plus optional data.

## Events (FP -> transport -> ESP-NOW)

- `0x0A MatchEvent`: id(u16), confidence(u8).
- `0x0B Fail/Busy/NoSensor/Tamper`: reason(u8) 0=match_fail,1=no_sensor,2=busy,3=tamper.
- `0x0C EnrollProgress`: stage(u8 1..8), slot(u16), status(u8 0=OK,1=FAIL/TIMEOUT).

## Enrollment flow (ENROLL slot)

- Stages emitted as EnrollProgress:
  1 START, 2 CAP1, 3 LIFT, 4 CAP2, 5 STORING, 6 OK, 7 FAIL, 8 TIMEOUT.
- Enrollment stops verify, performs two captures, builds/stores the model, then resumes verify if it was running and the sensor is trusted.
- The master/UI should guide the user based on the stage stream and only re-enable verify once enrollment is complete.

## Verify loop

- Runs ~5 Hz when enabled and the sensor is trusted.
- On match: MatchEvent with id/confidence.
- On no match: Fail event (reason=match_fail) is throttled to avoid spam.
- Tamper handling:
  - If the sensor responds with the wrong password, fingerprint is disabled and reported to the master (reason=3) at a throttled rate.
  - When tampered, all FP commands are denied except `AdoptSensor` and `ReleaseSensor` (so the master can recover the sensor).

## ESP-NOW / CommandAPI bridge

- CMD_FP_* tokens are parsed by ESP-NOW and injected into transport (VerifyOn/Off, Enroll/Delete/Clear, QueryDb, NextId, Adopt/Release).
- Transport Responses/Events to `destId=1` are mapped to CommandAPI opcodes:
  - Match -> `EVT_FP_MATCH` (payload: id u16 + conf u8)
  - Fail  -> `EVT_FP_FAIL`
  - NoSensor -> `ACK_FP_NO_SENSOR`
  - Busy -> `ACK_FP_BUSY`
  - Tamper -> `ACK_ERR_TOKEN`
  - Enroll stages -> `ACK_FP_ENROLL_*` (START/CAP1/LIFT/CAP2/STORING/OK/FAIL/TIMEOUT)
  - QueryDb -> `ACK_FP_DB_INFO`
  - NextId -> `ACK_FP_NEXT_ID`
  - DeleteId -> `ACK_FP_ID_DELETED`
  - ClearDb -> `ACK_FP_DB_CLEARED`
  - Adopt/Release -> `ACK_FP_ADOPT_OK/FAIL`, `ACK_FP_RELEASE_OK/FAIL`
- Adopt/Release are master-issued commands; the slave performs the action and reports the ACK when done (no separate broadcast event).

## Persistence and config

- No FP templates are stored in NVS; the sensor flash holds templates.
- HAS_FINGERPRINT_KEY gates FP usage; low battery (unpaired) may disable FP locally to save power.

## Safety/robustness

- No local unlock: FP match only notifies the master (no motor action locally).
- Verify/enroll tasks are mutually exclusive; adopt/release stops all FP tasks before changing passwords.
- Tamper/no-sensor conditions are throttled to avoid spamming.
