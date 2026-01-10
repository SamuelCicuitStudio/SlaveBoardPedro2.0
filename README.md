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

## First Boot (What You’ll See)

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
5. When pairing is complete, the slave will show the paired/online LED pattern and the master should show “paired/configured”.

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
- **Triple tap**: reserved for the existing “mode” shortcut on your build (unchanged).

---

## Reset / Unpair (How to remove a slave)

You can remove a slave in two ways:

### A) Remove from the Master UI (recommended)

- Use the master UI “Remove” action for that slave.
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

Tip: If you used triple-tap to disable RGB feedback, the device will keep working but won’t show these patterns until you enable RGB again.

---

## Fingerprint (Lock variant only)

If your slave has a fingerprint sensor and it’s enabled in the master configuration:
- The master UI controls the main flows: enable/disable verify loop, enroll, delete, query DB.
- During enrollment, the slave LED shows the enrollment step colors above.

---

## Power Saving / Sleep

- The slave goes to sleep automatically after inactivity (default ~4 minutes).
- It wakes on configured wake sources (door reed/open button, and some sensors depending on configuration).
- If battery is low/critical, the device may limit actions and sleep more aggressively to protect the battery.

---

## Troubleshooting

- **Pairing doesn’t complete**: move the slave closer to the master, ensure both are powered, retry pairing from the master UI.
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

### Core variables (what defines “mode”)

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
