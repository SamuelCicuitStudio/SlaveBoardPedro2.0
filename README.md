# Slave Board Firmware (Smart Lock / Alarm) — User Guide

This firmware runs on a **Slave board** that pairs to a **Master controller** over ESP-NOW.
Once paired, the slave reports status (battery, liveness, sensors) and accepts commands
from the master (lock/unlock, arm/disarm, capabilities, etc.).

There are two hardware/firmware variants:
- **Lock variant**: motorized lock + optional open button + optional fingerprint + sensors.
- **Alarm variant**: sensors only (reed + shock), no motor/open/fingerprint.

For deep technical details (protocol frames/opcodes), see the `readme/` folder at the end.

---

## What You Need

- A powered **Slave board** running this firmware.
- A powered **Master controller** running the matching master firmware.
- The master configuration UI/app (used to pair and configure the slave).

---

## First Boot (What You'll See)

- If the slave is **not paired yet**, it will wait for pairing from the master.
- If the slave is **already paired**, it will reconnect to the saved master after reboot.
- The device will automatically go to sleep after inactivity to save battery (default: ~4 minutes).

---

## Pairing (Step-by-step)

1. Power the slave board and keep it near the master.
2. Put the **master** into pairing mode from the master UI/app.
3. In the master UI/app, select the slave slot and set the capabilities (Open/Shock/Reed/Fingerprint), then press **Pair** (or **Save + Pair**).
4. Watch the slave LED and the master UI:
   - The master will send an init message to the slave.
   - The slave confirms it received it, then both sides switch to the secured link automatically.
5. When pairing is complete, the slave will show the paired/online LED pattern and the master should show "paired/configured".

If pairing fails, keep the devices closer, verify power/battery, and retry from the master UI.

---

## Buttons (On-device)

### USER button (`USER_BUTTON_PIN`)

- **Single tap**: prints the slave MAC address in the serial logs (for identification).
- **Triple tap**: toggles **RGB feedback OFF/ON** (battery saving mode).
  - When OFF, the device continues working normally, only LEDs are disabled.
  - Serial logs confirm the toggle.

### BOOT button (`BOOT_BUTTON_PIN`)

- **Long press**: triggers **factory reset** (clears pairing and settings) and restarts.
- **Triple tap**: reserved for the existing "mode" shortcut on your build (unchanged).

---

## Reset / Unpair (How to remove a slave)

You can remove a slave in two ways:

### A) Remove from the Master UI (recommended)

- Use the master UI "Remove" action for that slave.
- The slave will first send a confirmation back to the master, then erase pairing and restart.
- After the master receives the confirmation, it clears the slot and updates the UI.

### B) Factory reset using the BOOT button

- Hold **BOOT** until the reset is triggered (factory reset).
- This clears pairing data, so you will need to pair again.

---

## LED Guide (Colors + Patterns)

The RGB LED shows two kinds of indications:
- **Background state** (device state)
- **Overlay events** (short flashes/blinks on top of the background)

### Background states

| Meaning | Pattern | Color |
|---|---|---|
| Boot / Init | steady | dim gray `#0A0A0A` |
| Waiting for pairing | rainbow animation | (cycling) |
| Paired + master online | double heartbeat | green `#00B43C` |
| Paired + master offline | blink | indigo `#5D00FF` |
| Sleep | mostly off + rare heartbeat | deep blue `#1A2E80` |

### Overlay events (short indications)

| Event | Pattern | Color |
|---|---|---|
| Door opened | quick blink | tangerine `#FF7A00` |
| Door closed | short flash | teal `#00A7A7` |
| Shock detected | quick blink | hot pink `#FF007F` |
| Breach / intrusion | fast blink | orange-red `#FF3B00` |
| Low battery | slow blink | orange `#FF9500` |
| Critical battery | heartbeat | red `#FF0000` |

### Fingerprint enrollment overlays (Lock variant only)

| Enrollment step | Pattern | Color |
|---|---|---|
| Start enroll | blink | cobalt `#004DFF` |
| Lift finger | flash | yellow `#FFEA00` |
| Capture 1 | flash | arctic `#00E5FF` |
| Capture 2 | flash | seafoam `#00FFC8` |
| Storing model | blink | periwinkle `#6A6AFF` |
| Enroll OK | flash | turquoise `#26FFDA` |
| Enroll fail | blink | vivid red `#FF1744` |
| Enroll timeout | blink | orange-red `#FF5A00` |

Tip: If you used triple-tap to disable RGB feedback, the device will keep working but won't show these patterns until you enable RGB again.

---

## Fingerprint (Lock variant only)

If your slave has a fingerprint sensor and it's enabled in the master configuration:
- The master UI controls the main flows: enable/disable verify loop, enroll, delete, query DB.
- During enrollment, the slave LED shows the enrollment step colors above.

---

## Power Saving / Sleep

- The slave goes to sleep automatically after inactivity (default ~4 minutes).
- It wakes on configured wake sources (door reed/open button, and some sensors depending on configuration).
- If battery is low/critical, the device may limit actions and sleep more aggressively to protect the battery.

---

## Troubleshooting

- **Pairing doesn't complete**: move the slave closer to the master, ensure both are powered, retry pairing from the master UI.
- **No LED activity**: RGB feedback may be disabled (triple-tap USER button to re-enable).
- **Device seems asleep**: trigger a wake source (door open/close, open button, or power-cycle).

---

## More Documentation (technical)

- `readme/behavior.md` (full behavior spec)
- `readme/master_now_layer.md` (master/slave ESP-NOW layer notes)
- `readme/transport.md` (transport protocol and opcode tables)
- `readme/device.md` (device implementation notes)
- `readme/fingerprint.md` (fingerprint wiring and flows)

---

## Full Behavior Reference (explicit)

This section is intentionally detailed so the slave behavior is unambiguous.

### Core variables (what defines "mode")

- **Role**: Lock variant vs Alarm variant (compile-time / hardware build)
- **Pairing**: Paired vs Unpaired
- **Config Mode**: On/Off (paired-only, cleared by reboot)
- **Battery band**: Good / Low / Critical
- **Armed**: Armed / Disarmed (security behavior)
- **Motion enabled**: On/Off (gates shock reporting)

### Precedence (highest wins)

1. **Battery** (Low/Critical disables motor; may force sleep)
2. **Config Mode** (security logic forced off; still paired)
3. **Role gating** (Alarm variant has no motor/open/fingerprint)
4. **Armed** (security logic runs only when armed)
5. **Pairing** (normal traffic only when paired)

### Pairing vs. normal traffic

- **Unpaired**: pairing traffic only (init + config status). All other commands are ignored.
- **Paired**: normal commands/events enabled; master MAC is checked; secure peer is used.

### Pairing flow (exact)

1. Master sends `PairInit` (unencrypted) with `caps(u8)` + `seed(u32, big-endian)`.
2. Slave adds the master as a temporary **unencrypted** peer and sends `ACK_PAIR_INIT`.
3. Only after the `ACK_PAIR_INIT` delivery is **confirmed OK**, slave waits **300 ms**, then:
   - stores master MAC + caps + derived LMK
   - removes the unencrypted peer
   - re-adds the master as an **encrypted** peer (no ESP-NOW restart)
4. Slave sends a startup bundle: `ACK_CONFIGURED` + `EVT_BATTERY_PREFIX`.

### Mode matrix (Role + Pairing + Config + Battery)

| Role  | Paired | Config Mode | Battery   | Motor behavior | Shock reporting | Breach | Transport |
|------ |--------|-------------|-----------|----------------|-----------------|--------|----------|
| Lock  | No     | N/A         | Good      | Allowed via local button | Local only | Local only | Pairing traffic only |
| Lock  | No     | N/A         | Low       | Disabled | Local only | Local only | Pairing traffic only |
| Lock  | No     | N/A         | Critical  | Disabled | Local only | Local only | Pairing traffic only |
| Lock  | Yes    | No          | Good      | Allowed (commands) | Armed: Shock+AlarmRequest, Disarmed: Shock-only | Armed & reed-open | Normal |
| Lock  | Yes    | Yes         | Good      | Allowed (commands) | Shock-only (diagnostic) | No breach | Normal |
| Lock  | Yes    | Any         | Low       | Disabled (AlarmOnlyMode) | Reported; no AlarmRequest | No breach | Normal, then sleep |
| Lock  | Yes    | Any         | Critical  | Disabled | Reported; no AlarmRequest | No breach | Normal, then sleep |
| Alarm | No     | N/A         | Any       | N/A | Local only | Local only | Pairing traffic only |
| Alarm | Yes    | No          | Good      | N/A | Armed: Shock+AlarmRequest, Disarmed: Shock-only | Armed & reed-open | Normal |
| Alarm | Yes    | Yes         | Good      | N/A | Shock-only (diagnostic) | No breach | Normal |
| Alarm | Yes    | Any         | Low       | N/A | Reported; no AlarmRequest | No breach | Normal, then sleep |
| Alarm | Yes    | Any         | Critical  | N/A | Reported; no AlarmRequest | No breach | Normal, then sleep |

### Push-button matrix (Lock variant only)

| Pairing  | Battery  | What a press does |
|--------- |----------|-------------------|
| Unpaired | Good     | Drives motor locally (bench mode) |
| Unpaired | Low      | No motor (disabled) |
| Unpaired | Critical | No motor; minimal behavior |
| Paired   | Good     | Sends unlock/open request to master (no local motor) |
| Paired   | Low      | Sends request/overlays; motor disabled; then sleep |
| Paired   | Critical | Sends request briefly, then sleep; motor disabled |

### Sleep rules (explicit)

- The device does not sleep immediately on Low/Critical unless:
  - the battery band is stable for `BATTERY_BAND_CONFIRM_MS`, and
  - the Low/Critical grace timer `LOW_CRIT_GRACE_MS` elapsed, and
  - it is safe to sleep (notably: motor not moving/settling).
- When unpaired + critical battery, the device will deep-sleep after the grace window.

### Remove / Unpair (CMD_REMOVE_SLAVE)

- Slave sends `ACK_REMOVED` first while still configured (master MAC + LMK still valid).
- After the ACK is delivered and TX queues drain, slave clears pairing data and resets.
- Master should only clear the slave slot/peer after receiving `ACK_REMOVED`.

### Channel change (CMD_SET_CHANNEL)

- Slave sends `ACK_SET_CHANNEL` first, then persists the new channel, then reboots so it restarts on the new channel.

<!-- BEGIN EMBEDDED README FOLDER DOCS -->
# Embedded readme/ folder docs

This section is a verbatim copy of the documentation files under SlaveBoardFix/readme/ so everything is available in one place.

---

## Source: readme/behavior.md

# README ? Guide for Device Behavior (Lock vs. Alarm)

This document teaches a developer how to **reason about and describe** the device's runtime behavior **without contradiction**. It encodes authoritative rules, precedence, and outputs for every mode/state.

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
- Can drive the motor **unless disabled by battery policy**.
- Security (shock/breach) runs only when **Armed** (and not in Config Mode).

### Alarm role (`IS_SLAVE_ALARM=true`)

- Considers only: **reed** + **shock**.
- **Motor/open/fingerprint are disabled** (commands return UNSUPPORTED; signals ignored).
- **Breach definition (both roles):** when **Armed**, any **reed transition to open** -> breach (no lock-state dependency).

---

## 2) Pairing State (fixed)

### Unpaired (`DEVICE_CONFIGURED=false`)

- **Pairing transport only** (`PairInit`/`ACK_PAIR_INIT`, config status). No normal events/commands.
  Pair init must carry **caps (u8) + seed (u32, big-endian)** so the slave can
  derive the LMK and apply hardware caps after `ACK_PAIR_INIT` is confirmed OK.
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
  - **Shock/Motion**: report **shock trigger** events for visibility (always in Config Mode; motion enable is ignored).
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

- **Motion enabled** (`CMD_ENABLE_MOTION`) gates all shock events **except** in Config Mode (shock triggers are always reported there).
  - **Armed** (and not in Config Mode) + motion enabled: emit **Shock Trigger** **and** raise `AlarmRequest(reason=shock)`.
  - **Disarmed** (motion enabled) or **Config Mode** (always): emit **Shock Trigger only** (no `AlarmRequest`).

- **StateReport**: include at minimum
  `role`, `armed`, `door`, `breach`, `batt_level`, `power_band`, `configMode`, `shock_enabled`
  (and `locked` when in Lock role).
- **Power overlays** (Low/Critical):
  `AlarmOnlyMode` (Lock role only), `LockCanceled` (if a motor action was blocked), `CriticalPower`, and `Power LowBatt/CriticalBatt` as applicable.

### Lock role ? specifics

- **Motor control**:

  - On accepted lock/unlock, drive motor and emit **`MotorDone(locked=1/0)`** on completion.
  - **Never** emit `MotorDone` just because the door/reed changed; door visibility uses `DoorOpen`/`DoorClosed` + `StateReport`.
  - When **Low**/**Critical**, motor actions are **canceled**; emit `LockCanceled` and (for Low/Critical) `AlarmOnlyMode` as applicable.

- **Breach (Armed)**: same as Alarm role - **reed->open** -> `AlarmRequest(reason=breach)`; set/clear `breach` in `StateReport`.

### Alarm role ? specifics

- **Breach (Armed)**: same as Lock role - any **reed->open** transition -> `AlarmRequest(reason=breach)`; set/clear `breach`.
- **Motor/open/fingerprint**: commands return **UNSUPPORTED**; no related events are generated.
- Any `locked` field (if present) is **informational only** and **does not** affect breach.

---

## 7) Commands (when Paired)

- **Device/system**: pairing init/status (`CMD_CONFIG_STATUS`), battery level (`CMD_BATTERY_LEVEL`), arm/disarm, reboot, enter config mode, caps set/query, state/heartbeat/ping, sync, cancel timers, set role, test mode (`CMD_ENTER_TEST_MODE`), remove/unpair (`CMD_REMOVE_SLAVE`), factory reset (`CMD_FACTORY_RESET`), whitelisted NVS bool writes.
- **Alarm control**: `CMD_CLEAR_ALARM` clears alarm/breach state (`ACK_ALARM_CLEARED` / `EVT_ALARM_CLEARED`).
- **Motion trigger**: `CMD_ENABLE_MOTION` / `CMD_DISABLE_MOTION` (separate from arm/disarm).
- **Shock config**: `CMD_SET_SHOCK_SENSOR_TYPE`, `CMD_SET_SHOCK_SENS_THRESHOLD`, `CMD_SET_SHOCK_L2D_CFG`.
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
  2. The global **Low/Critical grace window** (`LOW_CRIT_GRACE_MS` ? 60s) has elapsed **and**
     no critical operations are in progress (notably, **motor is not moving**).
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
- **AlarmOnlyMode** (a **Lock-role battery overlay** when motor is disabled due to Low/Critical power).
  These are unrelated; an answer must never treat them as the same thing.

---

## 12) Answering Patterns (formatting rules)

When asked "what happens if...", **always**:

1. **Normalize inputs** explicitly:
   `Role=..., Paired=..., ConfigMode=..., Battery=..., Armed=...`
2. **Apply precedence** in order (§0) and call out any overrides (e.g., "Battery=Critical ? motor disabled regardless of role/armed.").
3. **State outputs** in this order, omitting irrelevant ones:

   - **Transport**: `sends/does not send` + which events (e.g., `DoorOpen`, `Shock Trigger`, `AlarmRequest(reason=breach|shock)`, overlays).
   - **Motor**: `drives/disabled/canceled` + `MotorDone` semantics (Lock role only).
   - **Breach**: `set/cleared/not evaluated` and why (armed/config/role).
   - **Sleep**: whether it sleeps immediately/after TX/normal.

4. **Avoid speculation**: if a detail is unspecified (e.g., exact debounce), say "implementation-standard; not specified here".

### Examples

**Example 1 ? Alarm role, Paired, Armed, Good battery, reed opens**

- Inputs: Role=Alarm, Paired=Yes, ConfigMode=No, Battery=Good, Armed=Yes
- Precedence: none override.
- Result: Transport sends `DoorOpen`, raises `AlarmRequest(reason=breach)`, sets `breach=1` in `StateReport`. Motor N/A.

**Example 2 ? Lock role, Unpaired, Good battery, user presses button**

- Inputs: Role=Lock, Paired=No, ConfigMode=N/A, Battery=Good, Armed=any
- Precedence: Unpaired -> pairing traffic only (no normal events/commands).
- Result: Button **drives motor** locally; pairing traffic only; local LEDs/logs update.

**Example 3 ? Lock role, Paired, Low battery, unlock command**

- Inputs: Role=Lock, Paired=Yes, ConfigMode=No, Battery=Low, Armed=any
- Precedence: Battery Low ? motor disabled.
- Result: Motor action **canceled**; send overlays (`AlarmOnlyMode`, `LockCanceled`, `Power LowBatt`), then **sleep**. No `AlarmRequest`.

**Example 4 ? Any role, Paired, Config Mode, shock triggers**

- Inputs: Role=Alarm|Lock, Paired=Yes, ConfigMode=Yes, Battery=Good, Armed=Yes
- Precedence: Config Mode ? security off.
- Result: Send **Shock Trigger only** (diagnostic), **no AlarmRequest**, **no breach**.

---

## 13) Quick FAQ

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
  **exactly one row** of the matrix in **§9.A** applies; answers must not describe mixtures of rows.
- Any wording like "**may** drive motor" or "**may** send event" is shorthand for  
  "**will** do this **whenever all the preconditions in that row are satisfied**"; there is no randomness or hidden policy.
- When `Paired=false`, `ConfigMode` is still an internal flag but has **no observable effect** on normal behavior; only **pairing traffic** is present. Externally, treat this as `ConfigMode=No`.

### 15.2 Implementation anchors (mapping spec ? code)

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
- **User/Boot button taps**: `SwitchManager::handleBootTapHold_()` detects taps on `USER_BUTTON_PIN`. A **single tap** prints the MAC to serial; a **triple tap** toggles RGB LED feedback off/on.
- **Shock sensor**: `ShockSensor::isTriggered()` uses `SHOCK_SENSOR1_PIN` for interrupts and selects **external GPIO** or **internal LIS2DHTR** via `SHOCK_SENSOR_TYPE_KEY`. LIS2DHTR config is stored in `SHOCK_L2D_*` keys and applied at boot and when updated. In **Unpaired** bench mode it still detects shocks and logs/overlays locally but emits **no normal transport events** (pairing traffic only). In **Paired** mode, every trigger emits a Shock Trigger event, and raising `AlarmRequest(reason=shock)` is additionally gated by `effectiveArmed==true` (Config Mode still sends Shock Trigger only). The LIS2DHTR and MAX17055 share the I2C bus; `I2CBusManager` mediates bus init/reset so one device doesn't reset the other. When switching to internal, the slave probes the LIS2DHTR; if it is missing, it falls back to external and returns `ACK_SHOCK_INT_MISSING (0xD9)`. On success, the slave sets `HAS_SHOCK_SENSOR_KEY=true` automatically.
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

---

## Source: readme/device.md

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
- Low/Critical power (paired): emit LockCanceled (critical flag), AlarmOnlyMode, CriticalPower, Power Low/Critical events then sleep. Unpaired critical ? deep sleep; unpaired low ? disable FP then sleep.

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

---

## Source: readme/fingerprint.md

# Fingerprint Integration (R503) and Transport Bridge

This document describes how the fingerprint subsystem is wired, how commands/events flow through transport and ESP-NOW, and what behaviors are enforced.

## Hardware and role gating
- Sensor: R503 over UART (pins `R503_RX_PIN`, `R503_TX_PIN`, baud `R503_BAUD_RATE`).
- Powered/used only in lock role (`IS_SLAVE_ALARM=false`) and when `HAS_FINGERPRINT_KEY=true`.
- In alarm role or when HAS_FINGERPRINT is false, the Fingerprint object is not constructed; all FP commands return UNSUPPORTED and no FP events are emitted.

## Initialization
- `Fingerprint::begin()` probes the sensor without adopting (no password change). If trusted and present, it starts verify mode.
- `refreshCapabilities_()` in Device gates FP creation; alarm role forces FP off.

## Command handling (transport ? FP)
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

## Events (FP ? transport ? ESP-NOW)
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
  - Match ? `ACK_FINGERPRINT_MATCH`
  - Fail ? `ACK_FINGERPRINT_FAIL`
  - NoSensor ? `ACK_FP_NO_SENSOR`
  - Busy ? `ACK_FP_BUSY`
  - Tamper ? `ACK_ERR_TOKEN`
  - Enroll stages ? `ACK_FP_ENROLL_*` (START/CAP1/LIFT/CAP2/STORING/OK/FAIL/TIMEOUT)
  - QueryDb ? `ACK_FP_DB_INFO`
  - NextId ? `ACK_FP_NEXT_ID`
  - DeleteId ? `ACK_FP_ID_DELETED`
  - ClearDb ? `ACK_FP_DB_CLEARED`
  - Adopt/Release ? `ACK_FP_ADOPT_OK/FAIL`, `ACK_FP_RELEASE_OK/FAIL`
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

---

## Source: readme/master_now_layer.md

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
- All ESP-NOW frames are binary and use hex opcodes defined in `src/api/CommandAPI.hpp`.
- Common header: `frameType (u8) + opcode (u16, little-endian) + payloadLen (u8)`.
- `frameType` differentiates Command vs Response vs PairInit.
- `ResponseMessage` carries both ACKs and Events; opcode values differentiate them.
- Pair-init is unencrypted and uses the fixed `PairInit` struct (caps + seed_be).

### Wire structures (ESP-NOW)
Canonical definitions live in `src/api/CommandAPI.hpp` (slave side).
```
enum NowFrameType : uint8_t {
  NOW_FRAME_CMD       = 0x01,
  NOW_FRAME_RESP      = 0x02,
  NOW_FRAME_PAIR_INIT = 0x03
};

#pragma pack(push, 1)
struct CommandMessage {
  uint8_t  frameType;  // NOW_FRAME_CMD
  uint16_t opcode;     // little-endian on wire
  uint8_t  payloadLen;
  uint8_t  payload[1];
};

struct ResponseMessage {
  uint8_t  frameType;  // NOW_FRAME_RESP
  uint16_t opcode;     // little-endian on wire (ACK vs EVT by opcode value)
  uint8_t  payloadLen;
  uint8_t  payload[1];
};

struct AckStatePayload { /* see CommandAPI.hpp */ };

struct PairInit {
  uint8_t  frameType;  // NOW_FRAME_PAIR_INIT
  uint8_t  caps;       // bit0=Open, bit1=Shock, bit2=Reed, bit3=Fingerprint
  uint32_t seed_be;    // big-endian on wire
};
#pragma pack(pop)
```

## File map and responsibilities (master)
- `NowConfig.hpp`: constants (queue lengths, timeouts) and the `Slave` state
  struct split into persisted vs RAM-only fields.
- `NowProtocol.hpp`: request, target, event, snapshot, and liveness structs
  used by the transport layer.
- `NowCore.*`: state owner for all slaves, liveness tracking, driver
  near/far, door safety timers, pending commands, and capability ownership.
- `NowPipeline.*`: ESP-NOW transport (init, peers, send/recv).
- `NowTransport.*`: request queue + worker thread; translates requests to
  command frames and enforces master-local endpoint rules.
- `NowManager.*` (ESPNowManager): glue between ESP-NOW callbacks, pipeline,
  and the core/transport.
- `SecurityKeys.hpp`: hardcoded PMK + seed-based LMK derivation.
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
  4) `ESPNowManager::onDataReceivedWorker_` parses opcode/payload, updates Core,
     and forwards to Transport.
- Pairing uses `StartRxOnly()` (no ping task).

## Pipeline layer (NowPipeline)
- `Begin(secure, channel)`:
  - If channel is 0, read `MASTER_CHANNEL_KEY` from NVS.
  - `esp_wifi_set_channel(channel)`.
  - `esp_now_init()`; registers send/recv callbacks.
  - If secure, sets PMK from the hardcoded define (master + slave match).
- `AddPeersFromCore()`:
  - Converts `macAddress` string to bytes.
  - Ensures LMK is present (derived from the stored seed), stores in NVS, converts hex to bytes.
  - Adds peer with `encrypt=true` when secure.
- `SendToMac()`:
  - If target is a known slave, records TX stats in Core.
  - Calls `esp_now_send`.
- `onRecv_()` always routes through `NowCore::HandleRx`.

## Transport layer (NowTransport)
- Owns request queue, event queue, and a worker task.
- `Enqueue()` pushes requests for the worker to serialize into command frames.
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
1) ESP-NOW is already running; PMK is set from the hardcoded define.
2) Add the target MAC as a temporary **unencrypted** peer (encrypt=false).
3) Start RX-only worker to listen for `ACK_PAIR_INIT`.
4) Send the unencrypted init payload (`PairInit`: caps + seed_be).
5) Wait for `ACK_PAIR_INIT` from the same MAC (timeout
   `NOW_REMOVE_ACK_TIMEOUT_MS`, default 15000 ms).
6) Remove the temporary unencrypted peer.
7) Derive LMK from master MAC + seed + `"LMK-V1"` and add the peer encrypted.
8) Start traffic (if not already running).
9) Wait **5 seconds** before sending pings/heartbeats/other background traffic
   to allow the slave to finish secure rejoin.

### Slot configuration (after init ACK)
- Store MAC + LMK in NVS.
- Save role/capability flags from pairing config and init packet.
- Add secure peer with derived LMK.
- Send lock driver mode (if lock role) and other config commands. No capability
  ACK is required; `ACK_PAIR_INIT` is the only required init confirmation.
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
- Shock config commands should ACK: `ACK_SHOCK_SENSOR_TYPE_SET`,
  `ACK_SHOCK_SENS_THRESHOLD_SET`, `ACK_SHOCK_L2D_CFG_SET`, and
  `ACK_SHOCK_INT_MISSING` when internal probe fails.

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
- Shock config ACKs: 0xD6..0xD9.

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
`SecurityKeys.hpp` and `readme/security.md` define:
- PMK is hardcoded as a `#define` on both master and slave.
- The slave always applies the PMK at ESPNOW init (even for unencrypted pairing),
  so it never relies on the default PMK.
- LMK = SHA256(AP_MAC + SEED + "LMK-V1") first 16 bytes (hex-encoded).
- Keys are stored as 32 uppercase hex chars.
- No LMK is sent over the air; the seed is sent in the pairing init message.

## DeviceManager integration (master)
- DeviceManager is the only owner that starts/stops ESP-NOW and runs the
  NowTransport event loop.
- Normal Mode: ESPNOW starts only when at least one slave is paired.
- Config Mode: ESPNOW runs even with zero peers.
- Events are filtered by policy before being forwarded to UI.

## Slave-side obligations for compatibility
The slave ESP-NOW layer must implement the following to match the master:

### Pairing and channel
- Listen for the unencrypted init payload:
  `caps (u8) + seed (u32, big-endian)`.
- On receipt (unicast from master):
  1) Add the master MAC as a **temporary unencrypted peer** (no PMK/LMK).
  2) Send `ACK_PAIR_INIT (0xA2)` immediately to that MAC.
  3) After the ACK is **delivered OK**, wait **300 ms**, then remove the temporary
     unencrypted peer.
  4) Derive the LMK from master MAC + seed + `"LMK-V1"`, then add the master peer
     in encrypted mode (no ESP-NOW restart).
  5) Apply the caps from `PairInit` only after `ACK_PAIR_INIT` is confirmed OK.
- Store master MAC and current channel in NVS before secure rejoin.

### Channel change (CMD_SET_CHANNEL)
- Master broadcasts `CMD_SET_CHANNEL` to all paired slaves.
- Slave writes the new channel to NVS and sends `ACK_SET_CHANNEL`.
- After ACK delivery, slave reboots and ESP-NOW starts on the stored channel.
- Master waits for all `ACK_SET_CHANNEL` responses before saving its own channel
  to NVS and rebooting into Config Mode on that channel.

### Secure keys (must match master)
- PMK is hardcoded as a `#define` on both master and slave.
- Derive LMK from master MAC + seed + `"LMK-V1"` as in `SecurityKeys.hpp`.
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
- `EVT_REED (0xB9)` payload: `EvtReedPayload { open }`.
- `EVT_MTRTTRG (0xB4)` no payload.
- `EVT_BATTERY_PREFIX (0xB1)` payload: `EvtBatteryPayload { pct }`.
- `EVT_LWBT (0xB2)` no payload.
- `EVT_HGBT (0xB3)` no payload.
- `EVT_CRITICAL (0x98)` no payload.
- `EVT_GENERIC (0x9E)` payload: `EvtGenericPayload { len, text[] }`.
- `EVT_BREACH (0x97)` no payload.
- `EVT_FP_MATCH (0xC0)` payload: `EvtFpMatchPayload { id_le, conf }`.
- `EVT_FP_FAIL (0xC1)` no payload.

### Capability report format
`ACK_CAPS (0xAE)` payload is `AckCapsPayload { caps }` where:
- bit0=Open, bit1=Shock, bit2=Reed, bit3=Fingerprint.

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
- Total frame size must fit ESP-NOW payload limits (<= 250 bytes).
- `payloadLen` must match the actual payload size.

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

---

## Source: readme/transport.md

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
- 0x10 SetSensorType (Req/Cmd). Payload: type(u8: 0=external,1=internal). Resp: status (optional reason byte; 0x01=internal sensor missing).
- 0x11 SetThreshold (Req/Cmd). Payload: ths(u8, 0..127). Resp: status.
- 0x12 SetL2dCfg (Req/Cmd). Payload: odr,scale,res,evt,dur,axis,hpf_mode,hpf_cut,hpf_en,latch,int_lvl (axis bits: 0=XL,1=XH,2=YL,3=YH,4=ZL,5=ZH). Resp: status.

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
- Master speaks `CommandMessage` frames (`frameType=NOW_FRAME_CMD`) with hex opcodes
  from `src/api/CommandAPI.hpp`.
- RX path:
  - Parsed CommandMessage -> transport Requests: config mode, arm/disarm, reboot/reset,
    caps set/query, set role, cancel timers, pairing init/status, motor lock/unlock/diag,
    shock enable/disable, shock sensor type/threshold/LIS2DHTR config (internal missing -> `ACK_SHOCK_INT_MISSING`), all FP commands (verify on/off, enroll/delete/clear, query DB,
    next ID, adopt/release).
  - Edge-handled (immediate ResponseMessage on ESP-NOW, no transport mutation):
    `CMD_STATE_QUERY` -> `ACK_STATE` (payload `AckStatePayload`),
    `CMD_HEARTBEAT_REQ` -> `ACK_HEARTBEAT`,
    `CMD_CONFIG_STATUS` -> `ACK_CONFIGURED`/`ACK_NOT_CONFIGURED`,
    `CMD_BATTERY_LEVEL` -> `EVT_BATTERY_PREFIX` (payload pct).
- TX path:
  - Any transport message with `destId=1` is translated to a `ResponseMessage`
    with opcode set to the matching `ACK_*` or `EVT_*` value, and the payload encoded
    per `CommandAPI.hpp` (e.g., `AckStatePayload`, `AckCapsPayload`, `EvtReedPayload`).
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

---

## Source: readme/UserGuide.docx (extracted text)

Smart Lock / Alarm Slave - Quick User Guide Roles: Lock (motorized) or Alarm (reed + shock only). Master pairs the device; unpaired units will not talk over radio. Power: Good = normal. Low/Critical = motor disabled; device stays awake ~60s then sleeps when safe. If sleep is pending while busy, the master sees sleep pending; it clears when safe or power recovers. Pairing: Master sends INIT; device stores master MAC, shows paired LED. If unpaired, no radio events are sent. Config Mode: Command from master; security is off but reporting works. Exit by reboot. Using the lock (Lock role): - Paired: Master commands lock/unlock; door edges and motor completion are reported. - Unpaired bench: Good battery only - the OPEN button unlocks locally; no radio. Security (paired): Armed = shock and breach alarms; Disarmed = no alarms. Config Mode forces no alarms. Alarms and sensors: - Shock: triggers a shock event; sends alarm only when Armed. - Reed: Armed + door forced open = breach; clears on close. Buttons: - OPEN (Lock role): one action per press; respects battery/paired rules above. - BOOT: long-press = factory reset; triple-tap preserved. LEDs: show pairing state, config mode, battery alerts, shock/breach, and motor activity. Reset: Master command or BOOT long-press triggers a safe reboot/factory reset.

<!-- END EMBEDDED README FOLDER DOCS -->

