# README - Guide for Device Behavior (Lock vs. Alarm)

This document teaches an LLM how to **reason about and describe** the device's runtime behavior **without contradiction**. It encodes authoritative rules, precedence, and outputs for every mode/state.

---

## Key behavior summary (authoritative)

- **Breach (paired, Good battery)**: Lock role -> `ARMED_STATE=true` + `LOCK_STATE=true` + door open; Alarm role -> `ARMED_STATE=true` + door open (lock state ignored).
- **Open button while armed**: the press is still reported to the master, but the slave never unlocks locally.
- **Disarm vs clear**: `CMD_DISARM_SYSTEM` prevents new alarm escalation; an active alarm/breach is cleared only by `CMD_CLEAR_ALARM`.
- **Breach persistence**: breach state is latched in NVS and survives reboot until the master clears it. The `AlarmRequest` event is sent only when breach is first detected; after reboot the breach flag remains set and is reported in state responses until cleared.
- **Driver near/far**: the master uses `CMD_DISABLE_MOTION` (driver near) and `CMD_ENABLE_MOTION` (driver far); driver near must not be treated as a full disarm.
- **Config Mode / Test Mode**: security off (no breach or `AlarmRequest`), but diagnostic events still flow; fingerprint verify remains active (match/fail streamed).
- **Fingerprint enrollment**: stage-by-stage feedback and waits for the user (place -> lift -> place again); verify and enroll must not overlap.
- **Fingerprint (lock role, unpaired boot)**: if the sensor is already adopted (secret password works), the slave releases it to default so the master can decide adoption after pairing; verify stays off after release.
- **Battery Low/Critical**: suppress `AlarmRequest` and new breach; Lock role emits `LockCanceled`/`AlarmOnlyMode` and disables fingerprint verify; Alarm role does not emit `LockCanceled`/`AlarmOnlyMode`; motor commands are still accepted so the master must block them.
- **Battery band confirmation**: Low/Critical effects apply only after `BATTERY_BAND_CONFIRM_MS` stable; before that, treat as Good (breach/AlarmRequest can still fire).

## 0) Canonical Concepts & Precedence (authoritative)

### Core state variables

- **Role**: `Lock` vs `Alarm` (selected by `IS_SLAVE_ALARM`).
- **Pairing**: `Paired` (`DEVICE_CONFIGURED=true`) vs `Unpaired` (`false`, ESPNOW stays in
  pairing mode and accepts pairing traffic).
- **Arming**: `Armed`/`Disarmed` is controlled by `CMD_ARM_SYSTEM` / `CMD_DISARM_SYSTEM`
  (ACKs: `ACK_ARMED` / `ACK_DISARMED`). **Motion trigger enable/disable** is separate
  (`CMD_ENABLE_MOTION` / `CMD_DISABLE_MOTION`).
- **Config Mode / Test Mode**: special, **paired-only**, RAM-latched until reboot (entered via `CMD_ENTER_TEST_MODE`, acknowledged by `ACK_TEST_MODE`).
- **Battery band**: `Good`, `Low`, `Critical`.
- **Capabilities (Lock role only, per HAS\_\* flags)**: motor, open button, fingerprint, reed switch, shock sensor.
  **Alarm role** always has: reed + shock **only** (motor/open/fingerprint disabled). Alarm role ignores `HAS_OPEN_SWITCH_KEY` and never arms the open switch input or wake source. Caps are forced to **reed+shock** even if PairInit/CapsSet/NvsWrite attempts to change them.

### Absolute precedence (apply from top to bottom; higher rules override lower)

1. **Battery safety** (Critical/Low overlays + sleep scheduling; alarm requests/breach suppressed; master blocks motor in Low/Critical)
2. **Config Mode** (security gating off; role capability gating still applies)
3. **Role capability gating** (Alarm role: no motor/open/fingerprint)
4. **Arming** (security features active only when Armed; forced off in Config Mode)
5. **Pairing** (normal transport exists only when Paired; unpaired uses pairing traffic only)

> If a question seems ambiguous, **resolve via precedence** first, then the role/mode definitions below.

### Transport vs local behavior

- **Paired**: normal events/commands flow on transport.
- **Unpaired**: ESPNOW remains in **pairing mode** and **does** exchange pairing traffic
  (`PairInit`/`ACK_PAIR_INIT`, config status). Normal command/event traffic is suppressed.
  Pair init payload is **caps (u8) + seed (u32, big-endian)** in a binary `PairInit`
  frame (`frameType=NOW_FRAME_PAIR_INIT`); the slave must reply with a `ResponseMessage`
  opcode `ACK_PAIR_INIT` before secure pairing/config traffic proceeds. No capability
  ACK is required. On `PairInit` unicast, the slave adds the master as a temporary
  unencrypted peer to send `ACK_PAIR_INIT`. After the ACK is **delivered OK**, it
  waits **300 ms**, removes that peer, derives the LMK from the master MAC + seed +
  `"LMK-V1"`, and re-adds the master encrypted (no ESP-NOW restart). The capability
  flags in `PairInit` are applied **after** `ACK_PAIR_INIT` is confirmed OK.
- **PMK** is always set on ESPNOW init using the shared `#define` (even for unencrypted
  pairing traffic), so the stack never relies on a default PMK.

---

## 1) Roles & Capabilities (fixed)

### Lock role (`IS_SLAVE_ALARM=false`)

- Considers: **motor**, **open button** (if present), **fingerprint** (if present), **reed**, **shock**.
- Motor runs only on master commands; the slave does **not** block motor commands on Low/Critical (master must enforce).
- Security (shock/breach) runs only when **Armed** (and not in Config Mode).

### Alarm role (`IS_SLAVE_ALARM=true`)

- Considers only: **reed** + **shock**.
- **Motor/open/fingerprint are disabled** (commands return UNSUPPORTED; signals ignored).
- **Breach definition**:
  - **Lock role**: Armed + `LOCK_STATE=true` + door open -> breach.
  - **Alarm role**: Armed + door open -> breach (lock state ignored).
  - **Operational note**: in Alarm role the master must disarm before opening; any door open while Armed is a breach.

---

## 2) Pairing State (fixed)

### Unpaired (`DEVICE_CONFIGURED=false`)

- **Pairing transport only** (`PairInit`/`ACK_PAIR_INIT`, config status). No normal events/commands.
  Pair init must carry **caps (u8) + seed (u32, big-endian)** so the slave can
  derive the LMK, switch the peer to encrypted mode, and apply hardware caps after
  `ACK_PAIR_INIT` is confirmed OK.
- **Unpaired wake overrides**:
  - Lock role: open button wake is always armed so the button can wake the device.
  - Alarm role: external shock pin wake is always armed so motion can wake the device.
- After pairing, wake sources follow the master-provided caps (Alarm role still forces reed+shock).
- Advertising LED may blink; device sleeps per battery policy.
- **Lock role only**: if battery is **Good** (confirmed band), the **open button may actuate the motor** locally and toggles lock/unlock based on `LOCK_STATE`.
  If battery is **Low** or **Critical**, local motor is disabled (see section 5).
- **Fingerprint (lock role)**: on boot, if the sensor is already adopted, the slave releases it to default (no auto-adopt). Verify remains off until the master issues Adopt/Verify commands after pairing.

### Paired (`DEVICE_CONFIGURED=true`)

- Transport handlers active; commands/events flow normally.
- LED shows ready/paired state.

---

## 3) Config Mode (paired-only; RAM-latched until reboot)

**Entry/Exit**

- Entered via `CMD_ENTER_TEST_MODE` (ACK `ACK_TEST_MODE`); commonly right after pairing for setup/testing.
- Stays active for the **entire boot**; cleared only by **reboot** (power-cycle or master reboot).

**Effect**

- **Security gating is OFF**: device behaves **as if Disarmed** for alarm purposes.
- **Role capability gating remains**: Alarm role still cannot use motor/open/fingerprint.
- **Reporting in Config Mode**

  - **Reed**: report `DoorOpen` / `DoorClosed` edges.
  - **Shock/Motion**: report **shock trigger** events for visibility (always in Config Mode; motion enable is ignored).
  - **Fingerprint (Lock role only)**: verify remains active for diagnostics (match/fail is streamed); enrollment still streams stages when requested.
  - **Never raise breach** and **never raise AlarmRequest** in Config Mode.
  - State/heartbeat/ping/caps queries/caps set work normally.

**Commands in Config Mode**

- All standard commands accepted **subject to battery policy and role gating**.
- **Alarm role**: motor commands are ignored/UNSUPPORTED (even if received).

---

## 4) Arming (fixed)

- **Armed** iff the master sends `CMD_ARM_SYSTEM` (ACK `ACK_ARMED`); **Disarmed** iff
  the master sends `CMD_DISARM_SYSTEM` (ACK `ACK_DISARMED`).
- **Motion trigger enable/disable** (`CMD_ENABLE_MOTION` / `CMD_DISABLE_MOTION`) is
  **not** the same as arming/disarming.
- **Security logic runs only when Armed** (shock/breach), **except** in Config Mode where security is forced off.
- **Breach rules**:
  - **Lock role**: _door open while Armed_ -> breach **only if** `LOCK_STATE=true` (expected locked).
  - **Alarm role**: _door open while Armed_ -> breach (lock state ignored).
  - Applies only when **Paired** and the **confirmed** band is **Good**; during the band-confirmation window, breach/AlarmRequest can still fire.

---

## 5) Battery Policy (authoritative)

### Master-facing quick reference (Paired, Config Mode off)

**Lock role**

- **Good**: normal behavior (motor by command, breach/AlarmRequest when armed, fingerprint verify active).
- **Low**: emits `LockCanceled` + `AlarmOnlyMode` + `Power LowBatt`; suppresses `AlarmRequest` and new breach; fingerprint verify disabled; motor commands still accepted, so the master must block them; sleeps after grace when safe.
- **Critical**: same as Low, plus `CriticalPower` + `Power CriticalBatt`; sleeps after grace when safe.

**Alarm role**

- **Good**: reed/shock normal; breach/AlarmRequest when armed.
- **Low**: suppresses `AlarmRequest` and new breach; reed/shock events still report if enabled; emits `Power LowBatt` only (no `LockCanceled`/`AlarmOnlyMode`); sleeps after grace when safe.
- **Critical**: same as Low (no `LockCanceled`/`AlarmOnlyMode`), plus `CriticalPower` + `Power CriticalBatt`; sleeps after grace when safe.

> Existing breach state persists across all battery bands until the master sends `CMD_CLEAR_ALARM`.

### Bands & effects (both roles, Paired & Unpaired)

Low/Critical behavior is applied only after the band has been stable for
`BATTERY_BAND_CONFIRM_MS`. During the confirmation window the device behaves
as **Good**, so breach/AlarmRequest can still fire.

- **Low battery**

  - **Paired**:
    - Lock role: emit `LockCanceled` + `AlarmOnlyMode` + `Power LowBatt` on band entry (not re-emitted when transitioning from Critical -> Low).
    - Alarm role: emit `Power LowBatt` only (no `LockCanceled`/`AlarmOnlyMode`; not re-emitted when transitioning from Critical -> Low).
    - Suppress `AlarmRequest` and breach; shock triggers still report if motion is enabled.
    - Fingerprint verify is disabled (Lock role).
    - Motor commands are still accepted by the slave; the **master must avoid sending motor commands** in Low.
    - Sleep after grace once safe (motor not moving).
  - **Unpaired**:
    - Pairing traffic only; open button never drives motor.
    - Fingerprint verify is disabled.
    - Sleep after grace once safe.

- **Critical battery**

  - **Paired**:
    - Lock role: emit `LockCanceled` + `AlarmOnlyMode` + `CriticalPower` + `Power CriticalBatt` on band entry.
    - Alarm role: emit `CriticalPower` + `Power CriticalBatt` only (no `LockCanceled`/`AlarmOnlyMode`).
    - Suppress `AlarmRequest` and breach; shock triggers still report if motion is enabled.
    - Fingerprint verify is disabled (Lock role).
    - Motor commands are still accepted by the slave; the **master must avoid sending motor commands** in Critical.
    - Sleep after grace once safe (motor not moving).
  - **Unpaired**:
    - Deep sleep after grace once safe (pairing traffic only while awake).

### Push-button behavior at Low/Critical (Lock role only)

- **Critical** (Paired or Unpaired): **never** drives motor locally.

  - If **Paired**: sends `OpenRequest`/`UnlockRequest`, allows a short TX window, then sleeps after grace when safe (motor not moving).
  - If **Unpaired**: brief wake/LED only; sleep after grace when safe (pairing traffic only).

- **Low**:

  - **Unpaired**: no motor actuation; pairing traffic only.
  - **Paired**: sends `OpenRequest`/`UnlockRequest`; no local motor; device sleeps after grace when safe.

> **AlarmOnlyMode overlay** (Lock role only) is **not** the same thing as the **Alarm role**. It tells the master to block motor commands.

---

## 6) Event Semantics (when Paired)

> The following are **transported only when Paired**. Locally, LEDs/logs may mirror some events.

### Common (both roles)

- **Door/Reed**: emit `DoorOpen` / `DoorClosed` edges and include door state in `StateReport`.
- **Shock/Motion**:

- **Motion enabled** (`CMD_ENABLE_MOTION`) gates all shock events **except** in Config Mode (shock triggers are always reported there).

  - **Armed** (and not in Config Mode) + motion enabled: emit **Shock Trigger** **and** raise `AlarmRequest(reason=shock)`.
  - **Disarmed** (motion enabled) or **Config Mode** (always): emit **Shock Trigger only** (no `AlarmRequest`).

- **StateReport**: include at minimum
  `role`, `armed`, `door`, `breach`, `batt_level`, `power_band`, `configMode`, `shock_enabled`.
  `locked` is meaningful only in Lock role; Alarm role reports `locked=0`.
- **Power overlays** (Low/Critical):
  `AlarmOnlyMode`/`LockCanceled` (Lock role only), `CriticalPower`, and `Power LowBatt/CriticalBatt` as applicable.

### Lock role - specifics

- **Motor control**:

  - On accepted lock/unlock, drive motor and emit **`MotorDone(locked=1/0)`** on completion.
  - **Never** emit `MotorDone` just because the door/reed changed; door visibility uses `DoorOpen`/`DoorClosed` + `StateReport`.
  - When **Low**/**Critical**, the slave still accepts motor commands; the master must avoid sending them. `LockCanceled`/`AlarmOnlyMode` overlays are emitted on band entry.

- **Breach (Armed)**: when **Paired** and the **confirmed** band is **Good**, `LOCK_STATE=true`, and the door is open (reed), raise `AlarmRequest(reason=breach)` and set `breach` in `StateReport` (cleared only by `CMD_CLEAR_ALARM`).

### Alarm role - specifics

- **Breach (Armed)**: when **Paired** and the **confirmed** band is **Good**, door open while Armed -> `AlarmRequest(reason=breach)`; `breach` stays set until `CMD_CLEAR_ALARM` (lock state ignored).
- **Operational note**: the master must disarm before opening; opening while Armed always triggers breach.
- **Motor/open/fingerprint**: commands return **UNSUPPORTED**; no related events are generated.
- **StateReport**: `locked` is always **0** (lock state is ignored in Alarm role).

---

## 7) Commands (when Paired)

- **Device/system**: pairing init/status (`CMD_CONFIG_STATUS`), battery level (`CMD_BATTERY_LEVEL`), arm/disarm, reboot, enter config mode, caps set/query, state/heartbeat/ping, sync, cancel timers, set role, test mode (`CMD_ENTER_TEST_MODE`), remove/unpair (`CMD_REMOVE_SLAVE`), factory reset (`CMD_FACTORY_RESET`), whitelisted NVS bool writes.
- **Alarm control**: `CMD_CLEAR_ALARM` clears alarm/breach state (`ACK_ALARM_CLEARED` / `EVT_ALARM_CLEARED`).
- **Motion trigger**: `CMD_ENABLE_MOTION` / `CMD_DISABLE_MOTION` (separate from arm/disarm).
- **Shock config**: `CMD_SET_SHOCK_SENSOR_TYPE`, `CMD_SET_SHOCK_SENS_THRESHOLD`, `CMD_SET_SHOCK_L2D_CFG`.
- **Motor (Lock role only)**: lock/unlock (`CMD_LOCK_SCREW` / `CMD_UNLOCK_SCREW`), force lock/unlock; **master must block** in Low/Critical (slave does not block).
- **Lock driver mode (Lock role only)**: `CMD_LOCK_EMAG_ON` / `CMD_LOCK_EMAG_OFF`.
- **Capabilities**: `CMD_CAP_*` toggles (open/shock/reed/fp) and `CMD_CAPS_QUERY` bitmap.
- **Alarm role**: motor commands are ignored/UNSUPPORTED in all modes (including Config Mode).
- **Fingerprint (Lock role only)**: verify/enroll/delete/clear/adopt/release; disabled if Low/Critical or absent by caps.

---

## 8) Sleep & Wake

- `SleepTimer` runs in all modes.
- **Wake sources**: reed, shock (both roles), open button (Lock role only; ignored in Alarm role even if configured), transport (paired or pairing mode).
  In Unpaired mode, wake sources are forced for convenience: open button wake is always enabled in Lock role, and shock wake is always enabled in Alarm role (external pin). After pairing, wake sources follow the master-provided caps.
- Battery bands may force immediate sleep after required transmissions (Paired) or deep sleep (Unpaired Critical).

---

## 9) Mode Matrices (normative)

### A) Role x Pairing x Config x Battery

| Role  | Paired | Config Mode | Battery      | Motor                         | Shock Reports                                   | Breach                             | Transport                     |
| ----- | ------ | ----------- | ------------ | ----------------------------- | ----------------------------------------------- | ---------------------------------- | ----------------------------- |
| Lock  | No     | N/A         | Good         | **Allowed via button** (wake armed) | Local only                                 | **Not evaluated**                  | **Pairing only**              |
| Lock  | No     | N/A         | Low          | **Disabled** (wake armed)          | Local only                                 | **Not evaluated**                  | **Pairing only**              |
| Lock  | No     | N/A         | Critical     | **Disabled** (wake armed)          | Local only                                 | **Not evaluated**                  | **Pairing only**              |
| Lock  | Yes    | No          | Good         | Allowed by commands           | Armed: Shock+AlarmRequest; Disarmed: Shock-only | **Armed & locked & door open**     | **Yes**                       |
| Lock  | Yes    | **Yes**     | Good         | Allowed by commands           | **Shock-only** (diagnostic)                     | **Suppressed (Config Mode)**       | **Yes**                       |
| Lock  | Yes    | Any         | **Low**      | Master-gated (no local block) | Reported; **no AlarmRequest**                   | **Suppressed (existing persists)** | **Yes**, then sleep           |
| Lock  | Yes    | Any         | **Critical** | Master-gated (no local block) | Reported; **no AlarmRequest**                   | **Suppressed (existing persists)** | **Yes**, then immediate sleep |
| Alarm | No     | N/A         | Any          | N/A                           | Local only (shock wake armed)                  | **Not evaluated**                  | **Pairing only**              |
| Alarm | Yes    | No          | Good         | N/A                           | Armed: Shock+AlarmRequest; Disarmed: Shock-only | **Armed & door open**              | **Yes**                       |
| Alarm | Yes    | **Yes**     | Good         | N/A                           | **Shock-only** (diagnostic)                     | **Suppressed (Config Mode)**       | **Yes**                       |
| Alarm | Yes    | Any         | **Low**      | N/A                           | Reported; **no AlarmRequest**                   | **Suppressed (existing persists)** | **Yes**, then sleep           |
| Alarm | Yes    | Any         | **Critical** | N/A                           | Reported; **no AlarmRequest**                   | **Suppressed (existing persists)** | **Yes**, then immediate sleep |

### A2) Explicit role/pairing/battery cases (Config Mode off)

The cases below are exhaustive for Role x Pairing x Battery. They assume `ConfigMode=No` and
`hasReed_=true`. Battery band refers to the **confirmed** band after `BATTERY_BAND_CONFIRM_MS`;
during confirmation the device behaves as **Good** (breach/AlarmRequest can still fire).
If `ConfigMode=Yes`, security is forced off (no `AlarmRequest`/breach) while pairing rules and
transport availability still apply.
Disarm stops new escalation but does **not** clear an active breach; only `CMD_CLEAR_ALARM` clears it.

#### Lock role

- **Paired + Good**

  - Transport: normal events/commands.
  - Door: `DoorOpen`/`DoorClosed` + `StateReport` on edges.
  - Breach: if Armed and `LOCK_STATE=true` and door open -> `AlarmRequest(reason=breach)` + `Breach set` (latched until `CMD_CLEAR_ALARM`).
  - Shock: if motion enabled and Armed -> Shock Trigger + `AlarmRequest(reason=shock)`; if Disarmed -> Shock Trigger only.
  - Motor/Open: commands drive motor; open button sends Open/Unlock requests (no local motor).
  - Fingerprint: verify active; enroll on command.

- **Paired + Low**

  - Transport: normal events/commands + power overlays (`LockCanceled`, `AlarmOnlyMode`, `Power LowBatt`).
  - Breach/AlarmRequest: suppressed; existing breach remains latched.
  - Shock: Shock Trigger only (if enabled).
  - Motor/Open: slave still accepts motor commands; master must block them; open button still sends requests.
  - Fingerprint: disabled; no verify.

- **Paired + Critical**

  - Transport: normal events/commands + overlays (`LockCanceled`, `AlarmOnlyMode`, `CriticalPower`, `Power CriticalBatt`).
  - Breach/AlarmRequest: suppressed; existing breach remains latched.
  - Shock: Shock Trigger only (if enabled).
  - Motor/Open: slave still accepts motor commands; master must block them; open button still sends requests.
  - Fingerprint: disabled; no verify.
  - Sleep: short TX window then sleep after grace when safe.

- **Unpaired + Good**

  - Transport: pairing-only.
  - Breach/AlarmRequest: not evaluated (no transport).
  - Door/Shock: local LED/log only.
  - Motor/Open: open button toggles lock/unlock based on current `LOCK_STATE`; no transport.
  - Fingerprint: if adopted, auto-release to default on boot; no transport.
  - Sleep: normal.

- **Unpaired + Low**

  - Transport: pairing-only.
  - Breach/AlarmRequest: not evaluated.
  - Door/Shock: local only.
  - Motor/Open: no local motor.
  - Fingerprint: disabled.
  - Sleep: after grace when safe.

- **Unpaired + Critical**
  - Transport: pairing-only.
  - Breach/AlarmRequest: not evaluated.
  - Door/Shock: local only.
  - Motor/Open: no local motor.
  - Fingerprint: disabled.
  - Sleep: deep sleep after grace when safe.

#### Alarm role

- **Paired + Good**

  - Transport: normal events/commands.
  - Door: `DoorOpen`/`DoorClosed` + `StateReport` on edges.
  - Breach: if Armed and door open -> `AlarmRequest(reason=breach)` + `Breach set` (latched until `CMD_CLEAR_ALARM`). Master must disarm before opening.
  - Shock: if motion enabled and Armed -> Shock Trigger + `AlarmRequest(reason=shock)`; if Disarmed -> Shock Trigger only.
  - Motor/Open/Fingerprint: unsupported/ignored.

- **Paired + Low**

  - Transport: normal events/commands + `Power LowBatt` (no `LockCanceled`/`AlarmOnlyMode` in Alarm role).
  - Breach/AlarmRequest: suppressed; existing breach remains latched.
  - Shock: Shock Trigger only (if enabled).
  - Motor/Open/Fingerprint: unsupported/ignored.
  - Sleep: after grace when safe.

- **Paired + Critical**

  - Transport: normal events/commands + `CriticalPower` + `Power CriticalBatt` (no `LockCanceled`/`AlarmOnlyMode` in Alarm role).
  - Breach/AlarmRequest: suppressed; existing breach remains latched.
  - Shock: Shock Trigger only (if enabled).
  - Motor/Open/Fingerprint: unsupported/ignored.
  - Sleep: short TX window then sleep after grace when safe.

- **Unpaired + Good**

  - Transport: pairing-only.
  - Breach/AlarmRequest: not evaluated.
  - Door/Shock: local only.
  - Motor/Open/Fingerprint: unsupported/ignored.
  - Sleep: normal.

- **Unpaired + Low**

  - Transport: pairing-only.
  - Breach/AlarmRequest: not evaluated.
  - Door/Shock: local only.
  - Motor/Open/Fingerprint: unsupported/ignored.
  - Sleep: after grace when safe.

- **Unpaired + Critical**
  - Transport: pairing-only.
  - Breach/AlarmRequest: not evaluated.
  - Door/Shock: local only.
  - Motor/Open/Fingerprint: unsupported/ignored.
  - Sleep: deep sleep after grace when safe.

### B) Push-button (Lock role only)

| Pairing  | Battery  | Result of press                                                                |
| -------- | -------- | ------------------------------------------------------------------------------ |
| Unpaired | Good     | **Drives motor** (toggle lock/unlock based on `LOCK_STATE`); local LED/log only; pairing traffic continues. |
| Unpaired | Low      | **No motor** (disabled); pairing traffic continues.                            |
| Unpaired | Critical | Brief wake/LED only; **no motor**; sleep after grace when safe.                |
| Paired   | Good     | Sends Open/UnlockRequest; **no local motor**; master decides on motor.         |
| Paired   | Low      | Sends requests; **no local motor**; sleep after grace when safe.               |
| Paired   | Critical | Sends requests, short TX window; sleep after grace when safe; **no local motor**. |

### C) Safe-to-sleep rules (all roles)

- Even when battery band is **Low** or **Critical**, the device does **not** enter sleep/deep-sleep
  until two conditions are satisfied:
  1. The band has been **stable** for at least `BATTERY_BAND_CONFIRM_MS` (anti-flicker), and
  2. The global **Low/Critical grace window** (`LOW_CRIT_GRACE_MS` ~ 60s) has elapsed **and**
     no critical operations are in progress (notably, **motor is not moving**) and
     the **door is closed** (reed not open).
- If battery transitions into Low/Critical **while the motor is already closing/opening the door**,
  the motor is allowed to **finish that motion**; sleep is scheduled for the earliest moment after
  the grace window when `Device::canSleepNow_()` would be true (motor stopped, no critical flows).

---

## 10) Logging & LEDs

- LED overlays accompany major edges (door, shock, breach, enroll, motor complete) when power permits.
- Logging follows the system logger; normal transport on/off strictly follows pairing state (pairing traffic remains when unpaired).

### Boot/User button tap behavior (local only)

- **Single tap**: prints the device MAC to serial (debug helper).
- **Triple tap** (within `TAP_WINDOW_MS`): toggles RGB LED feedback **off/on** to reduce battery usage.
  - This is a **local-only** toggle; it does not affect pairing state or transport behavior.

---

## 11) Names to keep distinct (do not conflate)

- **Alarm role** (a **product configuration**: reed + shock only).
- **AlarmOnlyMode** (a **battery overlay** indicating Low/Critical; emitted only in Lock role where the master should block motor commands).
  These are unrelated; an answer must never treat them as the same thing.

---

## 12) Answering Patterns (LLM formatting rules)

When asked "what happens if...", **always**:

1. **Normalize inputs** explicitly:
   `Role=..., Paired=..., ConfigMode=..., Battery=..., Armed=..., Locked=...`
2. **Apply precedence** in order (section 0) and call out any overrides (e.g., "Battery=Critical -> alarm requests/breach suppressed and master blocks motor; sleep scheduled.").
3. **State outputs** in this order, omitting irrelevant ones:

   - **Transport**: `sends/does not send` + which events (e.g., `DoorOpen`, `Shock Trigger`, `AlarmRequest(reason=breach|shock)`, overlays).
   - **Motor**: `drives/disabled/canceled` + `MotorDone` semantics (Lock role only).
   - **Breach**: `set/cleared/not evaluated` and why (armed/config/role).
   - **Sleep**: whether it sleeps immediately/after TX/normal.

4. **Avoid speculation**: if a detail is unspecified (e.g., exact debounce), say "implementation-standard; not specified here".

### Examples

**Example 1 - Alarm role, Paired, Armed, Good battery, door opens**

- Inputs: Role=Alarm, Paired=Yes, ConfigMode=No, Battery=Good, Armed=Yes, Locked=any (ignored)
- Precedence: none override.
- Result: Transport sends `DoorOpen`, raises `AlarmRequest(reason=breach)`, sets `breach=1` in `StateReport`. Motor N/A.

**Example 2 - Lock role, Unpaired, Good battery, user presses button**

- Inputs: Role=Lock, Paired=No, ConfigMode=N/A, Battery=Good, Armed=any
- Precedence: Unpaired -> pairing traffic only (no normal events/commands).
- Result: Button **drives motor** locally; pairing traffic only; local LEDs/logs update.

**Example 3 - Lock role, Paired, Low battery, unlock command**

- Inputs: Role=Lock, Paired=Yes, ConfigMode=No, Battery=Low, Armed=any
- Precedence: Battery Low => alarm requests/breach suppressed; master should not send motor commands.
- Result: Slave emits overlays (`AlarmOnlyMode`, `LockCanceled`, `Power LowBatt`), then sleeps after grace when safe. If the master still sends a motor command, the slave will execute it.

**Example 4 - Any role, Paired, Config Mode, shock triggers**

- Inputs: Role=Alarm|Lock, Paired=Yes, ConfigMode=Yes, Battery=Good, Armed=Yes
- Precedence: Config Mode => security off.
- Result: Send **Shock Trigger only** (diagnostic), **no AlarmRequest**, **no breach**.

---

## 13) Quick FAQ for the LLM

- **Q: Do we ever send transport while Unpaired?**
  **A: Yes, pairing traffic only.** Unpaired means **pairing-only transport**; normal events/commands are suppressed. Local LEDs/logs (and in Lock role, local motor via button if battery is Good) still apply.

- **Q: Can Config Mode force motor in Alarm role?**
  **A: No.** Config Mode disables **security**, not **role gating**. Alarm role never has motor/open/fingerprint.

- **Q: What is the breach rule in Alarm role?**
  **A: Armed & door open = breach.** Lock state is ignored because Alarm role has no motor.

- **Q: What is AlarmOnlyMode?**
  **A:** A **battery overlay** emitted on Low/Critical in **Lock role only**; it tells the master to block motor commands.

---

## 14) Compliance checklist (for your own answers)

Before sending an answer, verify:

- [ ] You listed **inputs** and applied **precedence**.
- [ ] You respected **role gating** and **battery** rules.
- [ ] You didn't conflate **Alarm role** with **AlarmOnlyMode**.
- [ ] You only emitted **normal transport** when **Paired** (pairing traffic allowed when Unpaired).
- [ ] You avoided breach/AlarmRequest in **Config Mode**.
- [ ] Lock role never emits `MotorDone` from door edges.
- [ ] Push-button behavior matches **battery** and **pairing** tables.

---

## 15) Interpretation & implementation anchors

### 15.1 Interpretation (no optional behavior)

- Every core variable (`Role`, `Paired`, `ConfigMode`, `Battery band`, `Armed`) is **binary** at runtime; there is **no third state** (for example no "partially armed").
- For any given tuple  
  `Role, Paired, ConfigMode, Battery, Armed`  
  **exactly one row** of the matrix in **section 9.A** applies; answers must not describe mixtures of rows.
- Any wording like "**may** drive motor" or "**may** send event" is shorthand for  
  "**will** do this **whenever all the preconditions in that row are satisfied**"; there is no randomness or hidden policy.
- When `Paired=false`, `ConfigMode` is still an internal flag but has **no observable effect** on normal behavior; only **pairing traffic** is present. Externally, treat this as `ConfigMode=No`.

### 15.2 Implementation anchors (mapping spec -> code)

These are here to make firmware changes straightforward and unambiguous.

- **Role**: compile-time `IS_SLAVE_ALARM` and `Device::isAlarmRole_`. Alarm role forces `hasOpenSwitch_=false`, `hasFingerprint_=false`, `hasShock_=true`, `hasReed_=true` inside `Device::refreshCapabilities_()`.
- **Pairing (Paired/Unpaired)**: NVS key `DEVICE_CONFIGURED` read via `Device::isConfigured_()`. `Paired=Yes` iff this key is `true`. When `Paired=No`, ESPNOW stays in pairing mode and accepts pairing traffic.
- **Armed/Disarmed**: controlled by `CMD_ARM_SYSTEM` / `CMD_DISARM_SYSTEM` and
  acknowledged via `ACK_ARMED` / `ACK_DISARMED`. Do **not** equate this with
  motion trigger enable/disable (`CMD_ENABLE_MOTION` / `CMD_DISABLE_MOTION`).
- **Motion enabled**: NVS key `MOTION_TRIG_ALARM` toggled by
  `CMD_ENABLE_MOTION` / `CMD_DISABLE_MOTION`. It gates all shock-trigger events.
- **Locked state**: NVS key `LOCK_STATE` read via `Device::isLocked_()`. It gates breach only in **Lock role**; Alarm role ignores lock state for breach and reports `locked=0` in state payloads.
- **Breach state**: NVS key `BREACH_STATE` stores the latched breach flag and is loaded on boot; it is cleared only by `CMD_CLEAR_ALARM`.
- **Config Mode**: `EspNowManager::configMode_` toggled by `EspNowManager::setConfigMode()`, mirrored into `Device::configModeActive_` by `Device::updateConfigMode_()`. All security paths must use `configModeActive_` to gate behavior.
- **Door open/closed**: `SwitchManager::isDoorOpen()` (backed by a fast IRQ on `REED_SWITCH_PIN` plus polling) read via `Device::isDoorOpen_()`. When `hasReed_==false`, the effective door is treated as **always closed** for breach/shock logic.
- **Open button (Lock role only)**: `SwitchManager::isOpenButtonPressed()` (backed by a fast IRQ on `OPEN_SWITCH_PIN` plus polling) read from `Device::pollInputsAndEdges_()`. A **single rising edge per physical press** is used to drive behavior. When Paired and allowed by battery policy, this rising edge generates `OpenRequest` (Switch/Reed op=0x02) and `UnlockRequest` (Device op=0x0E); in Unpaired/Good-battery bench mode it toggles **local lock/unlock** based on `LOCK_STATE` (pairing traffic still active; no normal events). In Critical, after a short TX window, sleep is scheduled by the power policy (grace + motor not moving). Alarm role ignores the open switch entirely (no input, no wake source).
- **User/Boot button taps**: `SwitchManager::handleBootTapHold_()` detects taps on `USER_BUTTON_PIN`. A **single tap** prints the MAC to serial; a **triple tap** toggles RGB LED feedback off/on.
- **Shock sensor**: `ShockSensor::isTriggered()` uses `SHOCK_SENSOR1_PIN` for interrupts and selects **external GPIO** or **internal LIS2DHTR** via `SHOCK_SENSOR_TYPE_KEY`. LIS2DHTR config is stored in `SHOCK_L2D_*` keys and applied at boot and when updated. In **Unpaired** bench mode it still detects shocks and logs/overlays locally but emits **no normal transport events** (pairing traffic only). In **Paired** mode, every trigger emits a Shock Trigger event, and raising `AlarmRequest(reason=shock)` is additionally gated by `effectiveArmed==true` (Config Mode still sends Shock Trigger only). The LIS2DHTR and MAX17055 share the I2C bus; `I2CBusManager` mediates bus init/reset so one device doesn't reset the other. When switching to internal, the slave probes the LIS2DHTR; if it is missing, it falls back to external and returns `ACK_SHOCK_INT_MISSING (0xD9)`. On success, it sets `HAS_SHOCK_SENSOR_KEY=true` automatically.
- **Breach flag**: `EspNowManager::breach` (`Now->breach`) is the single source of truth: `0` = no breach, `1` = active breach. The `breach` field in the state struct must mirror this flag, and it is cleared only by `CMD_CLEAR_ALARM`.
- **Battery bands**: raw band comes from `PowerManager::getPowerMode()` and `%` from `PowerManager::getBatteryPercentage()` with `Low` defined as `< LOW_BATTERY_PCT` while not critical and `Critical` as `powerMode == CRITICAL_POWER_MODE`. The band is **confirmed** into `effectiveBand_` only after `BATTERY_BAND_CONFIRM_MS` stable; Low/Critical behavior uses the confirmed band.
- **Battery policy enforcement**: `Device::enforcePowerPolicy_()` is the only place that:
  - Sends `LockCanceled`/`AlarmOnlyMode` (Lock role only) plus `CriticalPower`, `Power LowBatt/CriticalBatt` overlays/events (not tied to a specific motor command).
  - Schedules sleep (`SleepTimer::goToSleep()` when Paired, deep sleep when unpaired/critical).
  - Does **not** block motor commands; the master is responsible for motor gating in Low/Critical.
- Maintains a `sleepPending_` flag and exposes it in the Device state struct and via `EVT_SLEEP_PENDING` / `EVT_SLEEP_PENDING_CLEAR` so the master knows when Low/Critical sleep is scheduled but deferred (e.g., while a motor motion finishes) and when that pending state has been cancelled.
- **Event wiring**:
  - `DoorEdge` + `StateReport` are emitted from `Device::handleStateTransitions_()` on every door edge when Paired.
  - Shock `Trigger` + `AlarmRequest(reason=shock)` are emitted from `Device::pollInputsAndEdges_()` when shock conditions are met.
  - Breach `AlarmRequest(reason=breach)` + `Breach(set)` come from `Device::raiseBreachIfNeeded_()`. `CMD_CLEAR_ALARM` clears breach state.
  - `DriverFar` events (and `ACK_DRIVER_FAR`) are generated only when Lock role, Paired, `effectiveArmed=true`, door open, and `locked=false`, with a minimum interval of `DRIVER_FAR_ACK_MS`.
- **Sleep & wake**: `SleepTimer` is serviced from `Device::loop()`; all paths that send Low/Critical overlays or critical open-button requests must either call `SleepTimer::goToSleep()` or `Device::enterCriticalSleepUnpaired_()` as described in section 5 and section 8.
