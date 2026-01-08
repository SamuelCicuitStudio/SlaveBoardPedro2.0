# Master ESP-NOW Layer (implementation notes for slave)

This document summarizes how the master ESP-NOW layer is implemented in
`MasterFixLast/src/now` and related code, plus the policy readmes in
`MasterFixLast/readme`. It is intended as the compatibility checklist for the
slave ESP-NOW layer. The slave should reuse the same `CommandAPI.hpp` contract.

## Scope and sources
- Code: `MasterFixLast/src/now/*` (NowConfig, NowProtocol, NowCore*, NowPipeline*,
  NowTransport*, NowManager*).
- Supporting: `MasterFixLast/src/api/CommandAPI.hpp`,
  `MasterFixLast/src/application/SecurityKeys.hpp`,
  `MasterFixLast/src/application/ConfigNvs.hpp`,
  `MasterFixLast/src/application/DeviceManager.cpp`.
- Policies: `MasterFixLast/readme/espnowPolicy.md`,
  `MasterFixLast/readme/security.md`,
  `MasterFixLast/readme/securityNow.md`,
  `MasterFixLast/readme/livenessPolicy.md`,
  `MasterFixLast/readme/behaviorSlave.md`,
  `MasterFixLast/readme/espnow_device_requirements.md`.

## Command vocabulary and wire format
- All ESP-NOW frames are ASCII strings defined in `CommandAPI.hpp`.
- Command or event format: `"0xNN"` or `"0xNN:<payload>"`.
- `ESPNowManager` parses only the first 4 chars as hex code (`0xNN`) and
  optionally a payload after the first `:` in the buffer.
- Master RX parse buffer is 64 bytes; keep responses and payloads short
  (<= 63 bytes total including `0xNN` and payload).
- `NOW_CMD_MAX_LEN` default is 16 for outgoing command strings. Slave should
  keep command echoes and payload formats compact.

## File map and responsibilities (master)
- `NowConfig.hpp`: constants (queue lengths, timeouts) and the `Slave` state
  struct split into persisted vs RAM-only fields.
- `NowProtocol.hpp`: request, target, event, snapshot, and liveness structs
  used by the transport layer.
- `NowCore.*`: state owner for all slaves, liveness tracking, driver
  near/far, door safety timers, pending commands, and capability ownership.
- `NowPipeline.*`: ESP-NOW transport (init, peers, send/recv).
- `NowTransport.*`: request queue + worker thread; translates requests to
  command strings and enforces master-local endpoint rules.
- `NowManager.*` (ESPNowManager): glue between ESP-NOW callbacks, pipeline,
  and the core/transport.
- `SecurityKeys.hpp`: deterministic PMK/LMK generation (SHA256-based).
- `ConfigNvs.hpp`: NVS key names for master channel, slave slots, owners, etc.

## Data model (NowConfig + NowProtocol)
### Slave slots
Two arrays are owned by the master:
- `sideSlaves[NOW_MAX_SIDE_SLAVES]`
- `rearSlaves[NOW_MAX_REAR_SLAVES]`

Persisted per-slot fields (NVS):
- `macAddress` (string "AA:BB:CC:DD:EE:FF")
- `motorDirection`, `lockElectroMag`, `isAlarmType`
- `lmkHex` (32 hex chars, 16 bytes)
- `batteryLevel`
- `lockState`, `motionSensorTriggerState`, `armedState`
- `forceLockActive`, `lowBatNotify`
- `isConfigured`, `slotOccupied`, `requestUnlock`
- `lockCanceled`, `alarmOnlyMode`, `testMode`
- Capability flags: `capOpenSwitch`, `capShockSensor`, `capReedSwitch`,
  `capFingerprint`

RAM-only per-slot fields:
- Liveness: `alive`, `lastSeenEpochMs`, `missSweeps`
- Battery runtime: `liveBatteryPct`, `lastBatteryUpdateMs`, `criticalBattery`
- Door safety: `unlockStartMs`, `waitingDoorOpen`, `openedSinceUnlock`,
  `doorClosedAtMs`, `pendingAutoRelock`, `doorOpenAlertSent`
- Motion escalation: `motionWindowStartMs`, `motionTriggerCount`
- Fingerprint: `fpFailCount`
- Pending command/QoS: `pendingCmd`, `pendingAck`, `pendingActive`,
  `pendingSentMs`, `pendingRetries`
- Comm stats: last tx/rx/ack, retry counts, counters

### Requests and events
`NowProtocol.hpp` defines:
- `RequestType`: SendCommand, PairSlave, RemoveSlave, RestartSlave, EnterTestMode,
  ResetNowCore, ClearPeers, SyncFromNvs, ClearAlarm, ToggleDisarm, all fingerprint
  ops, capability set/query, snapshot/liveness, driver near/far.
- `TargetType`: SideIndex, RearIndex, Mac, AllSide, AllRear, All.
- `EventType`: RxNotification, CommandResult, SnapshotReady, LivenessReady.
- `Snapshot` and `Liveness` structures used by DeviceManager to refresh UI state.

## Task model and RX path
- `NowCore::StartTraffic()` creates:
  - `rxQueue_` (len `NOW_RX_QUEUE_LEN`, default 16)
  - `rxTask_` ("now_rx_worker")
  - `pingTask_` ("now_ping")
- ESP-NOW receive callback path:
  1) `NowPipeline::onRecv_` calls `NowCore::HandleRx`.
  2) `HandleRx` updates liveness for known MACs, then enqueues `RxMessage`.
  3) `rxWorkerLoop_` dequeues and calls `rxWorkerHandler_`.
  4) `ESPNowManager::onDataReceivedWorker_` parses the string, updates Core,
     and forwards to Transport.
- Pairing uses `StartRxOnly()` (no ping task).

## Pipeline layer (NowPipeline)
- `Begin(secure, channel)`:
  - If channel is 0, read `MASTER_CHANNEL_KEY` from NVS.
  - `esp_wifi_set_channel(channel)`.
  - `esp_now_init()`; registers send/recv callbacks.
  - If secure, sets PMK from `MASTER_PMK_KEY` or generates new.
- `AddPeersFromCore()`:
  - Converts `macAddress` string to bytes.
  - Generates LMK if missing, stores in NVS, converts hex to bytes.
  - Adds peer with `encrypt=true` when secure.
- `SendToMac()`:
  - If target is a known slave, records TX stats in Core.
  - Calls `esp_now_send`.
- `onRecv_()` always routes through `NowCore::HandleRx`.

## Transport layer (NowTransport)
- Owns request queue, event queue, and a worker task.
- `Enqueue()` pushes requests for the worker to serialize into commands.
- `processRequest_()` handles all request types:
  - Sends commands, handles pairing, removes peers, resets core, etc.
  - Builds snapshot/liveness by copying Core state into local cache.
- `NotifyRx()` is called from ESPNowManager for every received frame:
  - Updates pairing ACK state (ACK_PAIR_INIT).
  - On `ACK_REMOVED` or `ACK_FACTORY_RESET`, removes the slave slot and peer.
  - Emits `EventType::RxNotification` to the event queue.
- `scanPendingRemoveAcks_()` detects timed-out remove/factory-reset and emits a
  failure event without deleting the slot.

## Pairing flow (master)
### Preconditions
- Pairing is allowed only in Config Mode (policy docs).
- Target slot is chosen by UI/DeviceManager; master-local slot cannot be paired.

### Steps (from `NowTransport::sendPairInit_`)
1) Start unencrypted ESP-NOW on the configured channel.
2) Add the target MAC as a temporary unencrypted peer.
3) Start RX-only worker to listen for `ACK_PAIR_INIT`.
4) Send the unencrypted init string:
   `PAIR_INIT:v1:code=PAIR_INIT:chan=<MASTER_CHANNEL>`
5) Wait for `ACK_PAIR_INIT` from the same MAC (timeout
   `NOW_REMOVE_ACK_TIMEOUT_MS`, default 15000 ms).
6) Tear down unencrypted ESP-NOW.
7) Wait `NOW_PAIR_INIT_SECURE_DELAY_MS` (default 3000 ms).
8) Restart secure ESP-NOW, set PMK, add all peers from NVS, start traffic.

### Slot configuration (after init ACK)
- Store MAC + LMK in NVS.
- Save role/capability flags from pairing config.
- Add secure peer with derived LMK.
- Send lock driver mode (if lock role) and capability set commands.
- Reboot the slave (`CMD_REBOOT`).
- Mark owners dirty so capability ownership is re-evaluated.

## ACK mapping and pending commands
### Expected ACK codes for pending commands
`NowCore::expectedAckForCmd_()` sets the expected ACK for commands that are
marked pending (retain-on-fail). The slave must ACK with these codes:
- `CMD_LOCK_SCREW (0x01)` -> `ACK_LOCKED (0xA0)`
- `CMD_UNLOCK_SCREW (0x02)` -> `ACK_UNLOCKED (0xA1)`
- `CMD_REBOOT (0x05)` -> `ACK_REBOOT (0xB8)`
- `CMD_FACTORY_RESET (0x06)` -> `ACK_FACTORY_RESET (0xBF)`
- `CMD_ARM_SYSTEM (0x0A)` -> `ACK_ARMED (0x95)`
- `CMD_DISARM_SYSTEM (0x0B)` -> `ACK_DISARMED (0x96)`
- `CMD_FORCE_LOCK (0x0C)` -> `ACK_FORCE_LOCKED (0xAA)`
- `CMD_FORCE_UNLOCK (0x0D)` -> `ACK_FORCE_UNLOCKED (0xAB)`
- `CMD_REMOVE_SLAVE (0x16)` -> `ACK_REMOVED (0xAF)`
- `CMD_ENTER_TEST_MODE (0x17)` -> `ACK_TEST_MODE (0x9F)`
- `CMD_LOCK_EMAG_ON (0x29)` -> `ACK_LOCK_EMAG_ON (0xA8)`
- `CMD_LOCK_EMAG_OFF (0x2A)` -> `ACK_LOCK_EMAG_OFF (0xA9)`
- Capability set commands (0x20..0x27) -> `ACK_CAP_SET (0xAD)`
- `CMD_CAPS_QUERY (0x28)` -> `ACK_CAPS (0xAE)`

### ACKs that clear pending (even on error)
When a pending command is active, the master clears it if any of these arrive:
- The expected ACK, or one of:
  - `ACK_ERR_MAC (0x9B)`
  - `ACK_ERR_POLICY (0x9C)`
  - `ACK_UNINTENDED (0x9D)`
  - `ACK_LOCK_CANCELED (0xBA)`
  - `ACK_ALARM_ONLY_MODE (0xBB)`
  - `ACK_DRIVER_FAR (0xBC)`

### Remove/factory-reset timeout
If `CMD_REMOVE_SLAVE` or `CMD_FACTORY_RESET` does not ACK within
`NOW_REMOVE_ACK_TIMEOUT_MS` or exceeds `NOW_REMOVE_ACK_RETRY_LIMIT`,
the master reports failure and keeps the slot and peer intact.

## ACK detection (ESPNowManager)
`ESPNowManager::isAckCode_()` treats these as ACKs:
- Explicit list: 0x90..0x96, 0x9A..0x9F, 0xA0..0xA9, 0xAA, 0xAB, 0xAD,
  0xAE, 0xAF, 0xB8, 0xBA, 0xBB, 0xBC, 0xBF.
- Fingerprint ranges: 0xC2..0xC9, 0xCA..0xCF, 0xD0..0xD5.

Any ACK updates comm stats and is passed to NowCore and NowTransport.

## Core runtime behavior (NowCore)
### Liveness and battery sweep
Implemented in `NowCoreTraffic.cpp` and described in
`readme/livenessPolicy.md`.
- Heartbeat command: `CMD_HEARTBEAT_REQ (0x11)` with ACK `ACK_HEARTBEAT (0x91)`.
- Battery sweep: `CMD_BATTERY_LEVEL (0x03)`; slave replies with
  `EVT_BATTERY_PREFIX (0xB1:<pct>)`.
- Missed liveness increments `missSweeps`; offline when threshold reached.
- Battery marked unknown when stale or many misses.
- All-slaves-offline triggers GSM alert when at least one slave is paired.

### Driver near/far
`NowCoreSecurity.cpp`:
- Driver near:
  - Stop keypad task.
  - Send `CMD_DISABLE_MOTION` to all slaves.
  - Clears `motionSensorTriggerState` in RAM.
- Driver far:
  - GSM alert once.
  - Auto-rearm if system was disarmed.
  - Start keypad task if enabled.

### Arming/disarming
`ToggleDisarm()`:
- If disarming:
  - Clear alarm state, stop buzzer.
  - Send `CMD_DISARM_SYSTEM` + `CMD_DISABLE_MOTION`.
  - Update RAM armed/motion states.
- If re-arming:
  - Only if all doors are closed.
  - In Lock Master: also send `CMD_LOCK_SCREW`.
  - Send `CMD_ARM_SYSTEM` + `CMD_ENABLE_MOTION`.
  - Update RAM states.

### Door safety (Lock Master)
From `NowCoreTraffic.cpp`:
- After unlock, if no door-open within `LOCK_TIMEOUT_KEY + NOW_DOOR_OPEN_GRACE_MS`,
  master auto-relocks.
- After door closes post-unlock, relock after `AUTO_LOCK_AFTER_CLOSE_KEY`.
- If door stays open too long, master warns (GSM + buzzer).

### Shock/motion escalation
From `NowCoreSecurity.cpp`:
- `EVT_MTRTTRG (0xB4)` increments a 10s window.
- 1st/2nd triggers: warning sound; 3rd: alarm ON + buzzer.

### Event handling
`Core::HandleEvent()` reacts to:
- `ACK_UNLOCKED (0xA1)` and lock ACKs to manage timers and door state.
- `EVT_REED (0xB9:<0|1>)` for door open/close.
- `EVT_LWBT (0xB2)` and `EVT_HGBT (0xB3)` for battery band.
- `EVT_BATTERY_PREFIX (0xB1:<pct>)` updates live battery.
- `EVT_CRITICAL (0x98)` marks critical battery and triggers alerts.
- `EVT_FP_MATCH (0xC0)` and `EVT_FP_FAIL (0xC1)`.
- `EVT_GENERIC (0x9E)` used as unlock request (open button).
- `EVT_BREACH (0x97)` triggers alarm + GSM alerts.

## Capability ownership (Core validation)
Ownership is per side and per capability (Reed, Motion, Open).
`validateOwners_()`:
- Eligibility requires: slot occupied, configured, not critical battery,
  not offline, correct master mode (lock vs alarm), not lockCanceled, and
  capability flag true.
- Selection: alive first, higher battery next, lowest index tie-breaker.
- When ownership changes, master sends capability enable/disable commands.
- Master-local endpoint uses the highest slot index for its side and is
  excluded from normal pairing/removal.

## Master-local endpoint rules (Transport)
`sendCommandToTarget_()` blocks lock/unlock/force lock for the master-local
slot. The slave layer never sees master-local commands; this is a master-side
guard.

## Security and keys
`SecurityKeys.hpp` and `readme/security.md` define deterministic keys:
- PMK = SHA256(AP_MAC + "PMK-V1") first 16 bytes (hex-encoded).
- LMK = SHA256(AP_MAC + SLAVE_MAC + "LMK-V1") first 16 bytes (hex-encoded).
- Keys are stored as 32 uppercase hex chars.
- No keys are sent over the air; only the pairing init message is plaintext.

## DeviceManager integration (master)
- DeviceManager is the only owner that starts/stops ESP-NOW and runs the
  NowTransport event loop.
- Normal Mode: ESPNOW starts only when at least one slave is paired.
- Config Mode: ESPNOW runs even with zero peers.
- Events are filtered by policy before being forwarded to UI.

## Slave-side obligations for compatibility
The slave ESP-NOW layer must implement the following to match the master:

### Pairing and channel
- Listen for the unencrypted init string:
  `PAIR_INIT:v1:code=PAIR_INIT:chan=<N>`.
- Reply with `ACK_PAIR_INIT (0xA2)` immediately.
- Store master MAC from the sender address.
- Set ESPNOW channel to `<N>` and rejoin secure mode after the master delay.

### Secure keys (must match master)
- Derive PMK and LMK exactly as in `SecurityKeys.hpp` using AP MACs.
- Use uppercase hex strings if storing.

### Required command responses (ACKs)
Implement ACKs listed in the "Expected ACK codes" section above.
At minimum, the slave must respond with:
- `ACK_LOCKED`, `ACK_UNLOCKED`, `ACK_FORCE_LOCKED`, `ACK_FORCE_UNLOCKED`
  for lock operations it accepts.
- `ACK_LOCK_CANCELED` or `ACK_ALARM_ONLY_MODE` when a lock action is refused
  by policy (low/critical battery, alarm-only mode).
- `ACK_REBOOT` and `ACK_FACTORY_RESET` for reboot/reset.
- `ACK_ARMED` / `ACK_DISARMED` for arm commands.
- `ACK_TEST_MODE` for `CMD_ENTER_TEST_MODE`.
- `ACK_CAP_SET` and `ACK_CAPS` (with payload) for capability commands.
- `ACK_REMOVED` for remove/unpair.
- `ACK_HEARTBEAT` for `CMD_HEARTBEAT_REQ`.
- `ACK_CONFIGURED` / `ACK_NOT_CONFIGURED` for `CMD_CONFIG_STATUS`.

### Required events and payload formats
These directly affect master behavior:
- `EVT_REED (0xB9:<0|1>)` door open/close.
- `EVT_MTRTTRG (0xB4)` shock trigger.
- `EVT_BATTERY_PREFIX (0xB1:<pct>)` battery report (0..100).
- `EVT_LWBT (0xB2)` low battery.
- `EVT_HGBT (0xB3)` battery back to good.
- `EVT_CRITICAL (0x98)` critical battery.
- `EVT_GENERIC (0x9E)` open button or unlock request.
- `EVT_BREACH (0x97)` breach/tamper.
- `EVT_FP_MATCH (0xC0)` and `EVT_FP_FAIL (0xC1)` when fingerprint is used.

### Capability report format
`ACK_CAPS` payload must be:
`"O{0|1}S{0|1}R{0|1}F{0|1}"`
Example: `0xAE:O1S1R1F0`.

### Error responses
When a command is not allowed, the slave should send one of:
- `ACK_ERR_POLICY (0x9C)` for policy denies.
- `ACK_UNINTENDED (0x9D)` for invalid context.
- `ACK_ERR_MAC (0x9B)` for MAC/auth mismatch.

These are treated by the master as terminal responses for pending commands.

### Battery and liveness cadence
- Respond to every heartbeat (`CMD_HEARTBEAT_REQ`) with `ACK_HEARTBEAT`.
- Respond to battery sweeps with `EVT_BATTERY_PREFIX` promptly.
- Send `EVT_LWBT`, `EVT_HGBT`, and `EVT_CRITICAL` on transitions.

### Timing and payload limits
- Keep payloads short (master RX buffer is 64 bytes).
- Use only ASCII in payloads.

## Notes for slave implementation planning
- The master relies on liveness, battery, and capability ownership for
  security behavior; do not skip these responses.
- Master retains critical commands and retries every `NOW_QOS_RETRY_MS`
  (default 3000 ms). Expect duplicate commands and handle idempotently.
- The master treats the highest slot per side as reserved for a master-local
  endpoint; slave pairing should not target that slot.

## Quick reference (master constants)
Defaults from `NowConfig.hpp`:
- `NOW_RX_MAX_LEN` 250
- `NOW_RX_QUEUE_LEN` 16
- `NOW_PING_INTERVAL_MS` 5000
- `NOW_PING_TIMEOUT_MS` 15000
- `NOW_PING_MISS_THRESHOLD` 3
- `NOW_QOS_RETRY_MS` 3000
- `NOW_REMOVE_ACK_TIMEOUT_MS` 15000
- `NOW_REMOVE_ACK_RETRY_LIMIT` 3
- `NOW_LIVENESS_TARGET_MS` 2000
- `NOW_TRAFFIC_TICK_MS` 200
