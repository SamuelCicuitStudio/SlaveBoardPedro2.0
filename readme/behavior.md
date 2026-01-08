# README — LLM Guide for Device Behavior (Lock vs. Alarm)

This document teaches an LLM how to **reason about and describe** the device’s runtime behavior **without contradiction**. It encodes authoritative rules, precedence, and outputs for every mode/state.

---

## 0) Canonical Concepts & Precedence (authoritative)

### Core state variables

- **Role**: `Lock` vs `Alarm` (selected by `IS_SLAVE_ALARM`).
- **Pairing**: `Paired` (`DEVICE_CONFIGURED=true`) vs `Unpaired` (`false`, ESPNOW stays in
  pairing mode and accepts pairing traffic).
- **Arming**: `Armed`/`Disarmed` is controlled by `CMD_ARM_SYSTEM` / `CMD_DISARM_SYSTEM`
  (ACKs: `ACK_ARMED` / `ACK_DISARMED`). **Motion trigger enable/disable** is separate
  (`CMD_ENABLE_MOTION` / `CMD_DISABLE_MOTION`).
- **Config Mode**: special, **paired-only**, RAM-latched test mode until reboot.
- **Battery band**: `Good`, `Low`, `Critical`.
- **Capabilities (Lock role only, per HAS\_\* flags)**: motor, open button, fingerprint, reed switch, shock sensor.
  **Alarm role** always has: reed + shock **only** (motor/open/fingerprint disabled).

### Absolute precedence (apply from top to bottom; higher rules override lower)

1. **Battery safety** (Critical/Low policies and motor disable)
2. **Config Mode** (security gating off; role capability gating still applies)
3. **Role capability gating** (Alarm role: no motor/open/fingerprint)
4. **Arming** (security features active only when Armed; forced off in Config Mode)
5. **Pairing** (normal transport exists only when Paired; unpaired uses pairing traffic only)

> If a question seems ambiguous, **resolve via precedence** first, then the role/mode definitions below.

### Transport vs local behavior

- **Paired**: normal events/commands flow on transport.
- **Unpaired**: ESPNOW remains in **pairing mode** and **does** exchange pairing traffic
  (PAIR_INIT/ACK, config status). Normal command/event traffic is suppressed.

---

## 1) Roles & Capabilities (fixed)

### Lock role (`IS_SLAVE_ALARM=false`)

- Considers: **motor**, **open button** (if present), **fingerprint** (if present), **reed**, **shock**.
- Can drive the motor **unless disabled by battery policy**.
- Security (shock/breach) runs only when **Armed** (and not in Config Mode).

### Alarm role (`IS_SLAVE_ALARM=true`)

- Considers only: **reed** + **shock**.
- **Motor/open/fingerprint are disabled** (commands return UNSUPPORTED; signals ignored).
- **Breach definition (both roles):** when **Armed**, any **reed transition to open** -> breach (no lock-state dependency).

---

## 2) Pairing State (fixed)

### Unpaired (`DEVICE_CONFIGURED=false`)

- **Pairing transport only** (PAIR_INIT/ACK, config status). No normal events/commands.
- Advertising LED may blink; device sleeps per battery policy.
- **Lock role only**: if battery is **Good**, the **open button may actuate the motor** (local control).
  If battery is **Low** or **Critical**, motor is disabled (see §5).

### Paired (`DEVICE_CONFIGURED=true`)

- Transport handlers active; commands/events flow normally.
- LED shows ready/paired state.

---

## 3) Config Mode (paired-only; RAM-latched until reboot)

**Entry/Exit**

- Entered via explicit master command; commonly right after pairing for setup/testing.
- Stays active for the **entire boot**; cleared only by **reboot** (power-cycle or master reboot).

**Effect**

- **Security gating is OFF**: device behaves **as if Disarmed** for alarm purposes.
- **Role capability gating remains**: Alarm role still cannot use motor/open/fingerprint.
- **Reporting in Config Mode**

  - **Reed**: report `DoorOpen` / `DoorClosed` edges.
  - **Shock/Motion**: report **shock trigger** events for visibility (only when motion is enabled).
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
- **Breach rule (both roles)**: _reed->open while Armed_ -> breach (no lock-state dependency).

---

## 5) Battery Policy (authoritative)

### Bands & effects (both roles, Paired & Unpaired)

- **Low battery**

  - **Motor is disabled** (Lock role enters **AlarmOnlyMode** overlay).
  - **Paired**: emit required overlays/events to master, then **sleep**.
  - **Unpaired**: pairing traffic only; sleep after any local disable actions (e.g., fingerprint off).

- **Critical battery**

  - **Motor is disabled**; device minimizes activity.
  - **Paired**: emit `CriticalPower` + overlays, then **sleep immediately**.
  - **Unpaired**: **deep sleep immediately**.

### Push-button behavior at Low/Critical (Lock role only)

- **Critical** (Paired or Unpaired): **never** drives motor.

  - If **Paired**: may transmit `UnlockRequest/OpenRequest` briefly, then immediate sleep.
  - If **Unpaired**: brief wake/LED only, then sleep (pairing traffic only).

- **Low**:

  - **Unpaired**: motor **disabled**; no actuation.
  - **Paired**: may send request/overlays; motor stays **disabled**; device sleeps after sending overlays.

> **AlarmOnlyMode overlay** (Lock role, Low/Critical) is **not** the same thing as the **Alarm role**. They are unrelated concepts.

---

## 6) Event Semantics (when Paired)

> The following are **transported only when Paired**. Locally, LEDs/logs may mirror some events.

### Common (both roles)

- **Door/Reed**: emit `DoorOpen` / `DoorClosed` edges and include door state in `StateReport`.
- **Shock/Motion**:

  - **Motion enabled** (`CMD_ENABLE_MOTION`) gates all shock events.
  - **Armed** (and not in Config Mode) + motion enabled: emit **Shock Trigger** **and** raise `AlarmRequest(reason=shock)`.
  - **Disarmed or Config Mode** + motion enabled: emit **Shock Trigger only** (no `AlarmRequest`).

- **StateReport**: include at minimum
  `role`, `armed`, `door`, `breach`, `batt_level`, `power_band`, `configMode`, `shock_enabled`
  (and `locked` when in Lock role).
- **Power overlays** (Low/Critical):
  `AlarmOnlyMode` (Lock role only), `LockCanceled` (if a motor action was blocked), `CriticalPower`, and `Power LowBatt/CriticalBatt` as applicable.

### Lock role — specifics

- **Motor control**:

  - On accepted lock/unlock, drive motor and emit **`MotorDone(locked=1/0)`** on completion.
  - **Never** emit `MotorDone` just because the door/reed changed; door visibility uses `DoorOpen`/`DoorClosed` + `StateReport`.
  - When **Low**/**Critical**, motor actions are **canceled**; emit `LockCanceled` and (for Low/Critical) `AlarmOnlyMode` as applicable.

- **Breach (Armed)**: same as Alarm role - **reed->open** -> `AlarmRequest(reason=breach)`; set/clear `breach` in `StateReport`.

### Alarm role — specifics

- **Breach (Armed)**: same as Lock role - any **reed->open** transition -> `AlarmRequest(reason=breach)`; set/clear `breach`.
- **Motor/open/fingerprint**: commands return **UNSUPPORTED**; no related events are generated.
- Any `locked` field (if present) is **informational only** and **does not** affect breach.

---

## 7) Commands (when Paired)

- **Device/system**: pairing init/status (`CMD_CONFIG_STATUS`), battery level (`CMD_BATTERY_LEVEL`), arm/disarm, reboot, enter config mode, caps set/query, state/heartbeat/ping, sync, cancel timers, set role, test mode (`CMD_ENTER_TEST_MODE`), remove/unpair (`CMD_REMOVE_SLAVE`), factory reset (`CMD_FACTORY_RESET`), whitelisted NVS bool writes.
- **Alarm control**: `CMD_CLEAR_ALARM` clears alarm/breach state (`ACK_ALARM_CLEARED` / `EVT_ALARM_CLEARED`).
- **Motion trigger**: `CMD_ENABLE_MOTION` / `CMD_DISABLE_MOTION` (separate from arm/disarm).
- **Motor (Lock role only)**: lock/unlock (`CMD_LOCK_SCREW` / `CMD_UNLOCK_SCREW`), force lock/unlock; subject to **battery policy** (disabled at Low/Critical).
- **Lock driver mode (Lock role only)**: `CMD_LOCK_EMAG_ON` / `CMD_LOCK_EMAG_OFF`.
- **Capabilities**: `CMD_CAP_*` toggles (open/shock/reed/fp) and `CMD_CAPS_QUERY` bitmap.
- **Alarm role**: motor commands are ignored/UNSUPPORTED in all modes (including Config Mode).
- **Fingerprint (Lock role only)**: verify/enroll/delete/clear/adopt/release; disabled if Low/Critical or absent by caps.

---

## 8) Sleep & Wake

- `SleepTimer` runs in all modes.
- **Wake sources**: reed, shock (both roles), open button (Lock role), transport (paired or pairing mode).
- Battery bands may force immediate sleep after required transmissions (Paired) or deep sleep (Unpaired Critical).

---

## 9) Mode Matrices (normative)

### A) Role × Pairing × Config × Battery

| Role  | Paired | Config Mode | Battery      | Motor                        | Shock Reports                                   | Breach                | Transport                     |
| ----- | ------ | ----------- | ------------ | ---------------------------- | ----------------------------------------------- | --------------------- | ----------------------------- |
| Lock  | No     | N/A         | Good         | **Allowed via button**       | Local only                                      | Local only            | **Pairing only**              |
| Lock  | No     | N/A         | Low          | **Disabled**                 | Local only                                      | Local only            | **Pairing only**              |
| Lock  | No     | N/A         | Critical     | **Disabled**                 | Local only                                      | Local only            | **Pairing only**              |
| Lock  | Yes    | No          | Good         | Allowed by commands          | Armed: Shock+AlarmRequest; Disarmed: Shock-only | **Armed & reed->open** | **Yes**                       |
| Lock  | Yes    | **Yes**     | Good         | Allowed by commands          | **Shock-only** (diagnostic)                     | **No breach**         | **Yes**                       |
| Lock  | Yes    | Any         | **Low**      | **Disabled** (AlarmOnlyMode) | Reported; **no AlarmRequest**                   | **No breach**         | **Yes**, then sleep           |
| Lock  | Yes    | Any         | **Critical** | **Disabled**                 | Reported; **no AlarmRequest**                   | **No breach**         | **Yes**, then immediate sleep |
| Alarm | No     | N/A         | Any          | N/A                          | Local only                                      | Local only            | **Pairing only**              |
| Alarm | Yes    | No          | Good         | N/A                          | Armed: Shock+AlarmRequest; Disarmed: Shock-only | **Armed & reed->open** | **Yes**                       |
| Alarm | Yes    | **Yes**     | Good         | N/A                          | **Shock-only** (diagnostic)                     | **No breach**         | **Yes**                       |
| Alarm | Yes    | Any         | **Low**      | N/A                          | Reported; **no AlarmRequest**                   | **No breach**         | **Yes**, then sleep           |
| Alarm | Yes    | Any         | **Critical** | N/A                          | Reported; **no AlarmRequest**                   | **No breach**         | **Yes**, then immediate sleep |

### B) Push-button (Lock role only)

| Pairing  | Battery  | Result of press                                                            |
| -------- | -------- | -------------------------------------------------------------------------- |
| Unpaired | Good     | **Drives motor** (lock/unlock); local LED/log only; pairing traffic continues. |
| Unpaired | Low      | **No motor** (disabled); pairing traffic continues.                        |
| Unpaired | Critical | Brief wake/LED only; **no motor**; pairing traffic continues.              |
| Paired   | Good     | May submit Open/UnlockRequest; motor allowed by command path and battery.  |
| Paired   | Low      | Request/overlays allowed; **motor disabled**; device sleeps after sending. |
| Paired   | Critical | Request allowed, then **immediate sleep**; **no motor**.                   |

### C) Safe-to-sleep rules (all roles)

- Even when battery band is **Low** or **Critical**, the device does **not** enter sleep/deep-sleep
  until two conditions are satisfied:
  1. The band has been **stable** for at least `BATTERY_BAND_CONFIRM_MS` (anti-flicker), and
  2. The global **Low/Critical grace window** (`LOW_CRIT_GRACE_MS` ≈ 60s) has elapsed **and**
     no critical operations are in progress (notably, **motor is not moving**).
- If battery transitions into Low/Critical **while the motor is already closing/opening the door**,
  the motor is allowed to **finish that motion**; sleep is scheduled for the earliest moment after
  the grace window when `Device::canSleepNow_()` would be true (motor stopped, no critical flows).

---

## 10) Logging & LEDs

- LED overlays accompany major edges (door, shock, breach, enroll, motor complete) when power permits.
- Logging follows the system logger; normal transport on/off strictly follows pairing state (pairing traffic remains when unpaired).

---

## 11) Names to keep distinct (do not conflate)

- **Alarm role** (a **product configuration**: reed + shock only).
- **AlarmOnlyMode** (a **Lock-role battery overlay** when motor is disabled due to Low/Critical power).
  These are unrelated; an answer must never treat them as the same thing.

---

## 12) Answering Patterns (LLM formatting rules)

When asked “what happens if…”, **always**:

1. **Normalize inputs** explicitly:
   `Role=…, Paired=…, ConfigMode=…, Battery=…, Armed=…`
2. **Apply precedence** in order (§0) and call out any overrides (e.g., “Battery=Critical → motor disabled regardless of role/armed.”).
3. **State outputs** in this order, omitting irrelevant ones:

   - **Transport**: `sends/does not send` + which events (e.g., `DoorOpen`, `Shock Trigger`, `AlarmRequest(reason=breach|shock)`, overlays).
   - **Motor**: `drives/disabled/canceled` + `MotorDone` semantics (Lock role only).
   - **Breach**: `set/cleared/not evaluated` and why (armed/config/role).
   - **Sleep**: whether it sleeps immediately/after TX/normal.

4. **Avoid speculation**: if a detail is unspecified (e.g., exact debounce), say “implementation-standard; not specified here”.

### Examples

**Example 1 — Alarm role, Paired, Armed, Good battery, reed opens**

- Inputs: Role=Alarm, Paired=Yes, ConfigMode=No, Battery=Good, Armed=Yes
- Precedence: none override.
- Result: Transport sends `DoorOpen`, raises `AlarmRequest(reason=breach)`, sets `breach=1` in `StateReport`. Motor N/A.

**Example 2 — Lock role, Unpaired, Good battery, user presses button**

- Inputs: Role=Lock, Paired=No, ConfigMode=N/A, Battery=Good, Armed=any
- Precedence: Unpaired -> pairing traffic only (no normal events/commands).
- Result: Button **drives motor** locally; pairing traffic only; local LEDs/logs update.

**Example 3 — Lock role, Paired, Low battery, unlock command**

- Inputs: Role=Lock, Paired=Yes, ConfigMode=No, Battery=Low, Armed=any
- Precedence: Battery Low ⇒ motor disabled.
- Result: Motor action **canceled**; send overlays (`AlarmOnlyMode`, `LockCanceled`, `Power LowBatt`), then **sleep**. No `AlarmRequest`.

**Example 4 — Any role, Paired, Config Mode, shock triggers**

- Inputs: Role=Alarm|Lock, Paired=Yes, ConfigMode=Yes, Battery=Good, Armed=Yes
- Precedence: Config Mode ⇒ security off.
- Result: Send **Shock Trigger only** (diagnostic), **no AlarmRequest**, **no breach**.

---

## 13) Quick FAQ for the LLM

- **Q: Do we ever send transport while Unpaired?**
  **A: Yes, pairing traffic only.** Unpaired means **pairing-only transport**; normal events/commands are suppressed. Local LEDs/logs (and in Lock role, local motor via button if battery is Good) still apply.

- **Q: Can Config Mode force motor in Alarm role?**
  **A: No.** Config Mode disables **security**, not **role gating**. Alarm role never has motor/open/fingerprint.

- **Q: What is the breach rule in Alarm role?**
  **A: Armed & reed->open = breach.** No lock-state dependency.

- **Q: What is AlarmOnlyMode?**
  **A:** A **Lock-role** battery overlay (Low/Critical) meaning motor is disabled. Not related to Alarm role.

---

## 14) Compliance checklist (for your own answers)

Before sending an answer, verify:

- [ ] You listed **inputs** and applied **precedence**.
- [ ] You respected **role gating** and **battery** rules.
- [ ] You didn’t conflate **Alarm role** with **AlarmOnlyMode**.
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
  **exactly one row** of the matrix in **§9.A** applies; answers must not describe mixtures of rows.
- Any wording like "**may** drive motor" or "**may** send event" is shorthand for  
  "**will** do this **whenever all the preconditions in that row are satisfied**"; there is no randomness or hidden policy.
- When `Paired=false`, `ConfigMode` is still an internal flag but has **no observable effect** on normal behavior; only **pairing traffic** is present. Externally, treat this as `ConfigMode=No`.

### 15.2 Implementation anchors (mapping spec → code)

These are here to make firmware changes straightforward and unambiguous.

- **Role**: compile-time `IS_SLAVE_ALARM` and `Device::isAlarmRole_`. Alarm role forces `hasOpenSwitch_=false`, `hasFingerprint_=false`, `hasShock_=true`, `hasReed_=true` inside `Device::refreshCapabilities_()`.
- **Pairing (Paired/Unpaired)**: NVS key `DEVICE_CONFIGURED` read via `Device::isConfigured_()`. `Paired=Yes` iff this key is `true`. When `Paired=No`, ESPNOW stays in pairing mode and accepts pairing traffic.
- **Armed/Disarmed**: controlled by `CMD_ARM_SYSTEM` / `CMD_DISARM_SYSTEM` and
  acknowledged via `ACK_ARMED` / `ACK_DISARMED`. Do **not** equate this with
  motion trigger enable/disable (`CMD_ENABLE_MOTION` / `CMD_DISABLE_MOTION`).
- **Motion enabled**: NVS key `MOTION_TRIG_ALARM` toggled by
  `CMD_ENABLE_MOTION` / `CMD_DISABLE_MOTION`. It gates all shock-trigger events.
- **Locked state**: NVS key `LOCK_STATE` read via `Device::isLocked_()`. This does **not** gate breach; breach uses the shared reed-open rule.
- **Config Mode**: `EspNowManager::configMode_` toggled by `EspNowManager::setConfigMode()`, mirrored into `Device::configModeActive_` by `Device::updateConfigMode_()`. All security paths must use `configModeActive_` to gate behavior.
- **Door open/closed**: `SwitchManager::isDoorOpen()` (backed by a fast IRQ on `REED_SWITCH_PIN` plus polling) read via `Device::isDoorOpen_()`. When `hasReed_==false`, the effective door is treated as **always closed** for breach/shock logic.
- **Open button (Lock role only)**: `SwitchManager::isOpenButtonPressed()` (backed by a fast IRQ on `OPEN_SWITCH_PIN` plus polling) read from `Device::pollInputsAndEdges_()`. A **single rising edge per physical press** is used to drive behavior. When Paired and allowed by battery policy, this rising edge generates `OpenRequest` (Switch/Reed op=0x02) and `UnlockRequest` (Device op=0x0E); in Unpaired/Good-battery bench mode it drives a **local unlock task only** (pairing traffic still active; no normal events).
- **Shock sensor**: `ShockSensor::isTriggered()` now uses a hardware interrupt on `SHOCK_SENSOR1_PIN` to latch fast edges and is read via `Device::shockSensor` when `hasShock_==true` and `motorMoving==false`. In **Unpaired** bench mode it still detects shocks and logs/overlays locally but emits **no normal transport events** (pairing traffic only). In **Paired** mode, every trigger emits a Shock Trigger event, and raising `AlarmRequest(reason=shock)` is additionally gated by `effectiveArmed==true` (so in Config Mode only the Shock Trigger event is sent).
- **Breach flag**: `EspNowManager::breach` (`Now->breach`) is the single source of truth: `0` = no breach, `1` = active breach. `Breach(set/clear)` events and the `breach` field in the state struct must mirror this flag.
- **Battery bands**: `PowerManager::getPowerMode()` vs `CRITICAL_POWER_MODE` and `%` from `PowerManager::getBatteryPercentage()`. `Low` is defined as `< LOW_BATTERY_PCT` while not critical; `Critical` is `powerMode == CRITICAL_POWER_MODE`.
- **Battery policy enforcement**: `Device::enforcePowerPolicy_()` is the only place that:
  - Sends `LockCanceled`, `AlarmOnlyMode`, `CriticalPower`, `Power LowBatt/CriticalBatt` overlays/events.
  - Decides between `SleepTimer::goToSleep()` (Paired) and deep sleep (`Device::enterCriticalSleepUnpaired_()`) in critical/unpaired paths.
- Maintains a `sleepPending_` flag and exposes it in the Device state struct and via `EVT_SLEEP_PENDING` / `EVT_SLEEP_PENDING_CLEAR` so the master knows when Low/Critical sleep is scheduled but deferred (e.g., while a motor motion finishes) and when that pending state has been cancelled.
- **Event wiring**:
  - `DoorEdge` + `StateReport` are emitted from `Device::handleStateTransitions_()` on every door edge when Paired.
  - Shock `Trigger` + `AlarmRequest(reason=shock)` are emitted from `Device::pollInputsAndEdges_()` when shock conditions are met.
  - Breach `AlarmRequest(reason=breach)` + `Breach(set/clear)` come from `Device::raiseBreachIfNeeded_()` / `Device::clearBreachIfClosed_()`.
  - `DriverFar` events (and `ACK_DRIVER_FAR`) are generated only when Lock role, Paired, `effectiveArmed=true`, door open, and `locked=false`, with a minimum interval of `DRIVER_FAR_ACK_MS`.
- **Sleep & wake**: `SleepTimer` is serviced from `Device::loop()`; all paths that send Low/Critical overlays or critical open-button requests must either call `SleepTimer::goToSleep()` or `Device::enterCriticalSleepUnpaired_()` as described in §5 and §8.
