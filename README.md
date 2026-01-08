# Smart Lock / Alarm Slave Firmware (Behavior First)

This firmware runs a **slave board** that can act as:

- **Lock role**: motorized lock with optional open button and fingerprint sensor.
- **Alarm role**: reed + shock only (no motor/open/fingerprint).

The code is organized around clear behavior rules. This README focuses on **what
the device does**; for protocol details see the readme folder at the end.

---

## Behavior Overview (authoritative)

### Core state variables

- **Role**: `Lock` or `Alarm` (`IS_SLAVE_ALARM` in `src/api/Config.hpp`).
- **Pairing**: `Paired` if `DEVICE_CONFIGURED=true`, else `Unpaired`.
- **Arming**: `ARMED_STATE` (set by `CMD_ARM_SYSTEM` / `CMD_DISARM_SYSTEM`).
- **Motion enabled**: `MOTION_TRIG_ALARM` (set by `CMD_ENABLE_MOTION` / `CMD_DISABLE_MOTION`).
- **Config Mode**: temporary, RAM-only test mode (cleared on reboot).
- **Battery band**: `Good / Low / Critical` (from `PowerManager` with debounce + grace).

### Precedence (highest wins)

1. **Battery policy** (Low/Critical disables motor, can force sleep).
2. **Config Mode** (security logic forced off; role gating still applies).
3. **Role gating** (Alarm role never uses motor/open/fingerprint).
4. **Arming** (security logic only when armed).
5. **Pairing** (normal transport only when paired).

---

## Roles and Capability Gating

### Lock role (`IS_SLAVE_ALARM=false`)

- Uses motor, open button, fingerprint (if present), reed, shock.
- Motor driver mode comes from NVS (`LOCK_EMAG_KEY`).
- Open button can request unlock from master (paired) or drive motor locally (unpaired + Good battery).

### Alarm role (`IS_SLAVE_ALARM=true`)

- Uses **only reed + shock**.
- Motor/open/fingerprint are **hard disabled**:
  - Motor commands return `ACK_ERR_POLICY`.
  - Fingerprint commands return `ACK_ERR_POLICY`.
  - Capability bitmap is clamped to reed+shock only.

---

## Pairing and Config Mode

### Unpaired behavior (`DEVICE_CONFIGURED=false`)

- ESPNOW stays in **pairing mode**.
- Only **pairing traffic** is accepted/produced:
  - `PAIR_INIT:code=PAIR_INIT:chan=<n>` and `CMD_CONFIG_STATUS`.
- All other commands are ignored.
- **No normal transport events** are sent.
- **Lock role only**: open button can locally unlock **only** when battery is Good.

### Pairing flow

1. Master sends `PAIR_INIT` (plaintext) with channel.
2. Slave stores master MAC and channel, sets:
   - `DEVICE_CONFIGURED=true`
   - `ARMED_STATE=false`
   - `MOTION_TRIG_ALARM=false`
3. Slave sends `ACK_PAIR_INIT`, then switches to secure ESPNOW and sends:
   - `ACK_CONFIGURED` + one battery report (`EVT_BATTERY_PREFIX:<pct>`).

### Config Mode (paired-only, RAM-latched)

Entered by `CMD_ENTER_TEST_MODE` (`0x17`).

Effects:

- **Security logic forced off** (acts like disarmed).
- **Shock triggers still report** if motion is enabled.
- **No AlarmRequest** and **no breach** while in Config Mode.
- Cleared only by reboot.

---

## Security Behavior

### Shock / Motion

- Shock events are gated by **motion enabled** (`MOTION_TRIG_ALARM`).
- If **Armed** and **not in Config Mode**:
  - Shock trigger sends `EVT_MTRTTRG` and `AlarmRequest(reason=shock)`.
- If **Disarmed** or **Config Mode**:
  - Shock trigger sends **only** `EVT_MTRTTRG`.

### Breach (both roles)

- **Breach rule**: `Armed` and **reed opens** -> breach.
- Sends:
  - `AlarmRequest(reason=breach)` and `Breach(set)`.
  - Clears on reed close.
- Breach does **not** depend on lock state.

### Driver far reminder (Lock role)

When paired + armed + door open + unlocked:

- Periodically emits `ACK_DRIVER_FAR` + `Device.DriverFar` event.

---

## Battery Policy and Sleep

Battery policy runs in `Device::enforcePowerPolicy_()` and uses:

- **Band confirm**: band changes only after being stable for `BATTERY_BAND_CONFIRM_MS`.
- **Grace window**: Low/Critical waits `LOW_CRIT_GRACE_MS` before sleep.
- **Sleep pending**: if sleep is delayed (e.g., motor settling), the device emits:
  - `EVT_SLEEP_PENDING` and later `EVT_SLEEP_PENDING_CLEAR`.

### Low battery

- Motor disabled; lock role enters **AlarmOnlyMode** overlay.
- **Paired**: sends `LockCanceled`, `AlarmOnlyMode`, `Power LowBatt`, then sleeps.
- **Unpaired**: no transport; sleeps after grace.

### Critical battery

- Motor disabled; minimal activity.
- **Paired**: sends `LockCanceled`, `AlarmOnlyMode`, `CriticalPower`, `Power CriticalBatt`, then sleeps.
- **Unpaired**: deep sleep after grace.

---

## Transport and CommandAPI Mapping

Two layers operate together:

1. **Binary transport** (`src/radio/Transport.*`) for module/opcode messages.
2. **Text CommandAPI** (`src/api/CommandAPI.hpp`) for compatibility with the master.

The ESPNOW manager bridges both ways:

- Text `CMD_*` -> injected transport requests.
- Transport responses/events -> `ACK_*` / `EVT_*` strings.

See `readme/transport.md` for opcode tables and state struct format.

---

## Fingerprint (Lock role only)

Fingerprint is **optional** and only active when:

- `IS_SLAVE_ALARM=false`, and
- `HAS_FINGERPRINT_KEY=true`.

Key behaviors:

- Verify loop is started/stopped by `CMD_FP_VERIFY_ON/OFF`.
- Matches emit `EVT_FP_MATCH` (id, confidence).
- Enrollment and DB ops are master-driven (see `readme/fingerprint.md`).
- Alarm role always rejects FP commands with `ACK_ERR_POLICY`.

---

## Build and Flash

PlatformIO:

```bash
platformio run
platformio run --target upload
```

---

## Further Reading (source of truth)

- `readme/behavior.md` (authoritative behavior spec)
- `readme/transport.md` (transport protocol and opcode tables)
- `readme/device.md` (device implementation notes)
- `readme/fingerprint.md` (fingerprint wiring and flows)
