# Device Implementation Overview

This summarizes how the slave implements lock vs. alarm roles, transport/ESP-NOW wiring, and the exact behaviors enforced in code.

## Roles and capability gating
- Lock role (`IS_SLAVE_ALARM=false`): HAS_* gates decide open button, shock, reed, fingerprint. Motor and FP are constructed only if present.
- Alarm role (`IS_SLAVE_ALARM=true`): motor/open/fingerprint are hard-disabled; shock and reed are forced enabled. Motor/FP objects are not constructed, motor commands are stubbed (UNSUPPORTED), and motor-style events are suppressed.

## Pairing and transport
- Unpaired (`DEVICE_CONFIGURED=false`): no transport events; pairing handled by ESP-NOW.
  Pair-init is a binary `PairInit` frame (`frameType=NOW_FRAME_PAIR_INIT`) carrying
  **caps (u8) + seed (u32, big-endian)**. The slave must reply with a
  `ResponseMessage` using opcode `ACK_PAIR_INIT` before secure pairing proceeds
  (no capability ACK). On PairInit unicast, the slave adds the master as a temporary
  unencrypted peer and sends `ACK_PAIR_INIT`. After the ACK is **delivered OK**, it
  waits **300 ms**, removes that peer, then derives the LMK from master MAC + seed +
  "LMK-V1" and re-adds the master in encrypted mode (no ESP-NOW restart). The caps
  flags in `PairInit` are applied **after** `ACK_PAIR_INIT` is confirmed OK.
- PMK is set on ESPNOW init from the shared `#define` for both encrypted and
  unencrypted phases (no reliance on default PMK).
- Paired: transport manager wired to ESP-NOW; handlers registered (Device, Shock,
  Motor stub if alarm role, Fingerprint if present).

## Command handling (via transport)
- Device: config mode, arm/disarm, reboot, caps set/query, pairing init/status, state/heartbeat/ping, cancel timers, set role, limited NVS bool writes (armed bit, HAS_* presence flags, and `LOCK_EMAG_KEY` for screw vs electromagnet mode).
- Motor: lock/unlock/pulse (ignored/stubbed in alarm role).
- Shock: enable/disable, sensor type/threshold, LIS2DHTR config. Internal type requests probe the LIS2DHTR; on failure it returns `ACK_SHOCK_INT_MISSING (0xD9)` and reverts to external. On success, it sets `HAS_SHOCK_SENSOR_KEY=true` automatically.
- Fingerprint: verify on/off, enroll/delete/clear, query DB/next ID, adopt/release (only if FP present and lock role).

## Event/reporting
- Door/Reed (hasReed_): DoorEdge + StateReport on edges. MotorDone-style events emitted only in lock role; suppressed in alarm role. ACK_LOCKED/ACK_UNLOCKED still sent for visibility.
- Shock (hasShock_, motion enabled; Config Mode always reports): Shock Trigger; AlarmRequest(reason=shock) only when armed.
- Breach (paired, armed, locked, door open): AlarmRequest(reason=breach) + Breach set/clear; cleared on door close.
- DriverFar: lock role only, paired+armed+doorOpen+!locked, rate-limited.
- Open button (lock role, hasOpenSwitch_): OpenRequest + UnlockRequest; no local motor when paired. In critical power, short TX window then sleep.
- Fingerprint (lock role with FP): match/fail/tamper/busy/no-sensor events, enroll progress, adopt/release, DB info/next ID. No FP activity in alarm role.
- Low/Critical power (paired): emit LockCanceled (critical flag), AlarmOnlyMode, CriticalPower, Power Low/Critical events then sleep. Unpaired critical → deep sleep; unpaired low → disable FP then sleep.

## Battery and sleep
- `enforcePowerPolicy_()` drives low/critical overlays and sleep. SleepTimer serviced each loop. Critical/unpaired uses deep sleep; paired uses sleep timer after emitting required events.

## ESP-NOW / CommandAPI bridge
- `CommandMessage` frames (frameType `NOW_FRAME_CMD`) are parsed by ESP-NOW and
  mapped to transport Requests; immediate Responses are emitted for state/heartbeat/
  ping/config status/battery as `ResponseMessage` with the corresponding `ACK_*` opcode.
- Transport Responses/Events to destId=1 are translated to `ResponseMessage` frames
  with `opcode` set to the appropriate `ACK_*` or `EVT_*` value and sent over ESP-NOW;
  otherwise raw transport frames are sent.

## Divergences between roles
- Alarm role: no motor/open/FP; no MotorDone or OpenRequest events; DriverFar suppressed. Shock/reed/breach and power overlays still reported.
- Lock role: all peripherals allowed per HAS_*; motor and FP active if present; DriverFar and open button flows active.

## Reset/logs/LEDs
- Reboot command sets RESET flag; next boot follows normal policy. LED overlays used for edges (door, shock, breach, FP events). Logging unchanged by transport. 
