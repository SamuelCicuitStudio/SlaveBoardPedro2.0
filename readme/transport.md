# Transport Layer Specification (Current)

Transport sits above the unchanged `EspNowManager` and below application modules owned by `Device` (Motor, Shock, Switch/Reed, Fingerprint, Power, Sleep). Modules never touch MAC addresses or ESP-NOW APIs; they only see transport messages.

## Frame Format
- Wire = Header (11 bytes) + Payload.
- Header fields (fixed order):
  - `version` (u8, must be 1)
  - `msgId` (u16, per-sender counter, wraps allowed)
  - `srcId` (u8 logical sender)
  - `destId` (u8 logical destination, 0xFF = broadcast)
  - `module` (u8 enum below)
  - `type` (u8: 0=Request, 1=Response, 2=Event, 3=Command)
  - `opCode` (u8 module-specific)
  - `flags` (u8: bit0=ackRequired, bit1=isResponse, bit2=isError, bits3-7=0)
  - `payloadLen` (u8, header+payload <= 200)
  - `crc8` (u8, poly 0x07 over header bytes except `crc8`)
- Payload is binary, module/opCode-specific. Max total frame = 200 bytes.

## Status Codes (u8)
0=OK, 1=INVALID_PARAM, 2=UNSUPPORTED, 3=BUSY, 4=DENIED, 5=PERSIST_FAIL, 6=APPLY_FAIL, 7=TIMEOUT, 8=CRC_FAIL, 9=DUPLICATE.

## Modules and Opcodes (mandatory map)
### Module 0x01 Device
- 0x01 SetConfigMode (Req/Cmd). Payload: none. Resp: status.
- 0x02 StateQuery (Req). Resp: status + state struct.
- 0x03 ConfigStatus (Req). Resp: status + configured(u8).
- 0x04 Arm (Req/Cmd). Resp: status.
- 0x05 Disarm (Req/Cmd). Resp: status.
- 0x06 Reboot (Req/Cmd). Resp: status.
- 0x07 CapsSet (Req/Cmd). Payload: capability bits. Resp: status.
- 0x08 CapsQuery (Req). Resp: status + capability bits.
- 0x09 StateReport (Event). Payload: state struct.
- 0x0A PairingInit (Req/Cmd). Payload: masterId (6 bytes MAC) + token if used. Resp: status.
- 0x0B PairingStatus (Req). Resp: status + configured(u8) + masterId(6).
- 0x0C NvsWrite (Req/Cmd). Payload: keyId(u8) + value bytes. Resp: status.  
  Current bool `keyId` map:  
  - 1 = `ARMED_STATE` (armed/disarmed)  
  - 2 = `LOCK_STATE` (locked/unlocked)  
  - 3 = `HAS_OPEN_SWITCH_KEY` (Open button present)  
  - 4 = `HAS_SHOCK_SENSOR_KEY` (Shock sensor present)  
  - 5 = `HAS_REED_SWITCH_KEY` (Reed/door sensor present)  
  - 6 = `HAS_FINGERPRINT_KEY` (Fingerprint sensor present)  
  - 7 = `LOCK_EMAG_KEY` (lock driver mode: `false`=screw, `true`=electromagnet)
- 0x0D Heartbeat/Ping (Req). Resp: status + uptime(u32) + seq(u16).
- 0x0E UnlockRequest (Event). Payload: none.
- 0x0F AlarmRequest (Event). Payload: reason(u8: 0=breach,1=shock).
- 0x10 DriverFar (Event). Payload: none.
- 0x11 LockCanceled (Event). Payload: critical(u8: 1=critical,0=low).
- 0x12 AlarmOnlyMode (Event). Payload: critical(u8: 1=critical,0=low).
- 0x13 Breach (Event). Payload: state(u8: 1=set,0=clear).
- 0x14 CriticalPower (Event). Payload: pct(u8).
- 0x15 CancelTimers (Req/Cmd). Resp: status.
- 0x16 SetRole (Req/Cmd). Payload: role(u8). Resp: status.
- 0x17 Ping (Req) alias of Heartbeat. Resp: status + uptime(u32) + seq(u16).

Device state struct (little endian bytes):
- armed(u8), locked(u8), doorOpen(u8), breach(u8), motorMoving(u8)
- battPct(u8), powerMode(u8), powerBand(u8: 0=good,1=low,2=critical), configMode(u8), configured(u8), sleepPending(u8)
- uptimeMs(u32), role(u8: 0=lock,1=alarm), motionEnabled(u8)

Capability bits: bit0=OpenSwitch, bit1=Shock, bit2=Reed, bit3=Fingerprint.

### Module 0x02 Motor (lock boards)
- 0x01 Lock (Req/Cmd). Resp: status.
- 0x02 Unlock (Req/Cmd). Resp: status.
- 0x03 PulseCCW (Req/Cmd). Resp: status.
- 0x04 PulseCW (Req/Cmd). Resp: status.
- 0x05 MotorDone (Event/Resp). Payload: status(u8) + locked(u8). Replaces ACK_LOCKED/ACK_UNLOCKED.

### Module 0x03 Shock
- 0x01 Enable (Req/Cmd). Resp: status.
- 0x02 Disable (Req/Cmd). Resp: status.
- 0x03 Trigger (Event). Payload: none.

### Module 0x04 Switch/Reed
- 0x01 DoorEdge (Event). Payload: doorOpen(u8: 1=open,0=closed).
- 0x02 OpenRequest (Event). Payload: none.

### Module 0x05 Fingerprint
- 0x01 VerifyOn (Req/Cmd). Resp: status.
- 0x02 VerifyOff (Req/Cmd). Resp: status.
- 0x03 Enroll (Req/Cmd). Payload: slot(u16). Resp: status.
- 0x04 DeleteId (Req/Cmd). Payload: slot(u16). Resp: status.
- 0x05 ClearDb (Req/Cmd). Resp: status.
- 0x06 QueryDb (Req). Resp: status + count(u16) + cap(u16).
- 0x07 NextId (Req). Resp: status + slot(u16).
- 0x08 AdoptSensor (Req/Cmd). Resp: status.
- 0x09 ReleaseSensor (Req/Cmd). Resp: status.
- 0x0A MatchEvent (Event). Payload: id(u16) + confidence(u8).
- 0x0B Fail/Busy/NoSensor/Tamper (Event). Payload: reason(u8: 0=match_fail,1=no_sensor,2=busy,3=tamper).
- 0x0C EnrollProgress (Event). Payload: stage(u8 1..8), slot(u16), status(u8 0=OK,1=FAIL/TIMEOUT).

### Module 0x06 Power
- 0x01 BatteryQuery (Req). Resp: status + pct(u8) + powerMode(u8).
- 0x02 LowBatt (Event). Payload: pct(u8).
- 0x03 CriticalBatt (Event). Payload: pct(u8).

### Module 0x07 Sleep
- 0x01 SleepNow (Req/Cmd). Resp: status.

## Core Components (implementation)
- `TransportPort` holds:
  - TX queues: `txHigh_` (ackRequired/control) and `txLow_` (telemetry).
  - RX queue: `rxQueue_` (FIFO). RX and TX are independent; `tick()` drains RX first, then TX/retries.
  - Pending map for ackRequired retries (`maxRetries`, `retryMs` from Config).
  - Dedup buffer `(srcId,msgId)` of size `dedupEntries` to drop duplicates.
  - Handler table keyed by module.
  - Public API: `send`, `registerHandler`, `onReceiveRaw`, `drainRxQueue`, `tick`, `setSelfId`.
- `Serializer` encodes/decodes header+payload and enforces CRC/length.
- Auto-ACK: for ackRequired Requests, TransportPort sends an OK Response if the handler does not respond itself.
- Dedup: any duplicate `(srcId,msgId)` is dropped with no handler call.

## Send Path
1) Caller builds `TransportMessage` with module/type/opCode/payload and sets `flags` bit0 if a response is required.
2) TransportPort sets `msgId`, `srcId`, and `payloadLen`, then enqueues to `txHigh_` (ackRequired) or `txLow_`.
3) `tick()` pops TX (high before low), serializes, and calls the provided radio `sendFn`.
4) For ackRequired non-responses, a Pending entry is created and retried until `maxRetries`; on timeout, `onAckTimeout` is invoked on the module handler.

## Receive Path
1) Radio callback calls `onReceiveRaw`.
2) Serializer validates header/length/CRC. Invalid frames drop.
3) Dedup check; duplicates drop.
4) Responses complete pending by `msgId`.
5) Message is queued into `rxQueue_`.
6) `drainRxQueue()` (called at the start of `tick()`) dispatches FIFO to the module handler and issues auto-ACK if needed.
7) Read-only queries must respond immediately in the handler: Heartbeat/Ping, ConfigStatus, CapsQuery, StateQuery, PairingStatus, FP QueryDb, FP NextId.

## Wiring (device)
- `TransportManager` owns `EspNowAdapter` and `TransportPort` (selfId=2). `tick()` is called from `Device::loop()`. `onRadioReceive` is hooked from `EspNowManager::onDataReceived`.
- Handlers registered: `DeviceHandler`, `FingerprintHandler`, `MotorHandler` (or stub on alarm-only), `ShockHandler`.
- Device emits Events: door edges/state, motor done, unlock requests, alarm requests (breach/shock), driver-far, LockCanceled, AlarmOnlyMode, Breach set/clear, CriticalPower/Power low, shock trigger.
- Fingerprint emits Match/Fail/Broadcast/BUSY/NoSensor/Tamper events, EnrollProgress, Adopt/Release, DB info/NextId responses. Commands are handled in `FingerprintHandler`.
- DeviceHandler covers all Device opcodes listed above, including pairing, config mode, arm/disarm, reboot, caps set/query, cancel timers, role, NVS writes, heartbeat/ping/state/caps queries.

## ESP-NOW CommandAPI Bridge (two-way)
- Master keeps speaking `CMD_*` / `ACK_*` per `CommandAPI.h`; ESP-NOW radio is unchanged.
- RX path:
  - Parsed CMDs → transport Requests: config mode, arm/disarm, reboot/reset, caps set/query, set role, cancel timers, pairing init/status, motor lock/unlock/diag, shock enable/disable, all FP commands (verify on/off, enroll/delete/clear, query DB, next ID, adopt/release).
  - Edge-handled (immediate ACK on ESP-NOW, no transport mutation): `CMD_STATE_QUERY`→`ACK_STATE`, `CMD_HEARTBEAT_REQ`/`CMD_PING`→`ACK_HEARTBEAT/ACK_PING`, `CMD_CONFIG_STATUS`→`ACK_CONFIGURED/ACK_NOT_CONFIGURED`, `CMD_BATTERY_LEVEL`→`ACK_BATTERY_PREFIX:<pct>`.
  - CMD parsing remains enabled (`legacyCmdsEnabled=true`); no Device direct calls are made—state-changing ops are injected into transport.
- TX path:
  - Any transport message with `destId=1` is intercepted and translated to the matching `ACK_*` string, then sent over ESP-NOW: Device state/heartbeat/caps/config/pairing, UnlockRequest/AlarmRequest/DriverFar/LockCanceled/AlarmOnly/CriticalPower/Breach, MotorDone → `ACK_LOCKED/ACK_UNLOCKED`, Shock Trigger → `ACK_MTRTTRG`, all FP events/responses → `ACK_FP_*`/`ACK_FINGERPRINT_*`.
  - If not translatable, the raw transport frame falls back to ESP-NOW send.
- Both directions still traverse transport queues (rxQueue_/txHigh_/txLow_) to keep TX/RX independent.

## IDs and Peers
- Logical IDs: master=1, self(slave)=2. Peer resolver maps `destId` to master MAC in NVS; broadcasts use destId=0xFF.

## Health and Security
- CRC8 on every frame; bad CRC is counted as `CRC_FAIL` and dropped.
- Uses existing ESP-NOW peering/encryption (unchanged).
- Track send/fail/retry counters and last success per peer when available.

## Testing Hooks
- In-memory loopback TransportPort for unit tests.
- Deterministic timers/backoff for retry tests.
- Fuzz harness for bad headers/length/CRC to ensure safe drops.
