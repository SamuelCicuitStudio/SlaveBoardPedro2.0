# Slave Board Firmware (Smart Lock / Alarm) - User Guide

This firmware runs on a **Slave board** that pairs to a **Master controller** over ESP-NOW.
Once paired, the slave reports status (battery, liveness, sensors) and accepts commands
from the master (lock/unlock, arm/disarm, capabilities, etc.).

There are two hardware/firmware variants:
- **Lock variant**: motorized lock + optional open button + optional fingerprint + sensors.
- **Alarm variant**: sensors only (reed + shock), no motor/open/fingerprint.

---

## What You Need

- A powered **Slave board** running this firmware.
- A powered **Master controller** running the matching master firmware.
- The master configuration UI/app (used to pair and configure the slave).

---

## Pairing (Step-by-step)

1. Power the slave board and keep it near the master.
2. Put the **master** into pairing mode from the master UI/app.
3. In the master UI/app, select the slave slot and set the capabilities (Open/Shock/Reed/Fingerprint), then press **Pair** (or **Save + Pair**).
4. When pairing is complete, the slave will show the paired/online LED pattern and the master should show "paired/configured".

If pairing fails, keep the devices closer, verify power/battery, and retry from the master UI.
Note: On the Alarm variant, motor/open/fingerprint capabilities are ignored even if enabled in the UI.

---

## Practical Use (Unpaired vs Paired)

### Unpaired (bench mode)

- **Transport**: pairing traffic only; no normal commands/events are sent.
- **Lock variant**:
  - Open button **wakes** the device (always armed for wake).
  - If battery is **Good**, open button toggles lock/unlock **locally** (no transport).
  - Fingerprint verify stays off; if the sensor is already adopted, it is **released to default** at boot so the master can decide adoption later.
- **Alarm variant**:
  - External shock pin **wakes** the device (always armed for wake).
  - Reed/shock activity is local only; no breach is reported while unpaired.
- **Sleep**: normal battery policy applies; Low/Critical disables local motor and may force sleep after grace.
 - **Quick wake tip**: unpaired Lock can be woken by the open button; unpaired Alarm can be woken by the motion/shock sensor.

### Paired (normal mode)

- **Transport**: all events and commands flow between slave and master.
- **Lock variant**:
  - Open button sends Open/Unlock requests; **never** unlocks locally when paired.
  - Master controls motor actions; fingerprint verify/enroll/adopt/release are master-driven.
- **Alarm variant**:
  - Door open while Armed triggers breach (lock state ignored).
  - No motor/open/fingerprint behavior.
- **Wake sources**: follow master-provided caps (Alarm role still forces reed+shock).

---

## Buttons (On-device)

### USER button (`USER_BUTTON_PIN`)

- **Single tap**: prints the slave MAC address in the serial logs (for identification).
- **Triple tap**: toggles **RGB feedback OFF/ON** (battery saving mode).
  - When OFF, the device continues working normally; only LEDs are disabled.
  - Serial logs confirm the toggle.

### BOOT button (`BOOT_BUTTON_PIN`)

- **Long press**: triggers **factory reset** (clears pairing and settings) and restarts.
- **Triple tap**: reserved for the existing "mode" shortcut on your build (unchanged).

When the system is armed, open-button presses are still reported to the master but never unlock locally.

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
| Boot / Init | steady | dim gray <img alt="#0A0A0A" src="https://placehold.co/16x16/0A0A0A/0A0A0A.png" /> `#0A0A0A` |
| Waiting for pairing | rainbow animation | (cycling) |
| Paired + master online | double heartbeat | green <img alt="#00B43C" src="https://placehold.co/16x16/00B43C/00B43C.png" /> `#00B43C` |
| Paired + master offline | blink | indigo <img alt="#5D00FF" src="https://placehold.co/16x16/5D00FF/5D00FF.png" /> `#5D00FF` |
| Sleep | mostly off + rare heartbeat | deep blue <img alt="#1A2E80" src="https://placehold.co/16x16/1A2E80/1A2E80.png" /> `#1A2E80` |

### Overlay events (short indications)

| Event | Pattern | Color |
|---|---|---|
| Door opened | quick blink | tangerine <img alt="#FF7A00" src="https://placehold.co/16x16/FF7A00/FF7A00.png" /> `#FF7A00` |
| Door closed | short flash | teal <img alt="#00A7A7" src="https://placehold.co/16x16/00A7A7/00A7A7.png" /> `#00A7A7` |
| Shock detected | quick blink | hot pink <img alt="#FF007F" src="https://placehold.co/16x16/FF007F/FF007F.png" /> `#FF007F` |
| Breach / intrusion | fast blink | orange-red <img alt="#FF3B00" src="https://placehold.co/16x16/FF3B00/FF3B00.png" /> `#FF3B00` |
| Low battery | slow blink | orange <img alt="#FF9500" src="https://placehold.co/16x16/FF9500/FF9500.png" /> `#FF9500` |
| Critical battery | heartbeat | red <img alt="#FF0000" src="https://placehold.co/16x16/FF0000/FF0000.png" /> `#FF0000` |

Breach means the system is armed and the reed reports the door open. In Lock role it also requires `LOCK_STATE=true` (expected locked); in Alarm role lock state is ignored. Breach persists across reboot and is cleared only by the master via `CMD_CLEAR_ALARM`.

### Fingerprint enrollment overlays (Lock variant only)

| Enrollment step | Pattern | Color |
|---|---|---|
| Start enroll | blink | cobalt <img alt="#004DFF" src="https://placehold.co/16x16/004DFF/004DFF.png" /> `#004DFF` |
| Lift finger | flash | yellow <img alt="#FFEA00" src="https://placehold.co/16x16/FFEA00/FFEA00.png" /> `#FFEA00` |
| Capture 1 | flash | arctic <img alt="#00E5FF" src="https://placehold.co/16x16/00E5FF/00E5FF.png" /> `#00E5FF` |
| Capture 2 | flash | seafoam <img alt="#00FFC8" src="https://placehold.co/16x16/00FFC8/00FFC8.png" /> `#00FFC8` |
| Storing model | blink | periwinkle <img alt="#6A6AFF" src="https://placehold.co/16x16/6A6AFF/6A6AFF.png" /> `#6A6AFF` |
| Enroll OK | flash | turquoise <img alt="#26FFDA" src="https://placehold.co/16x16/26FFDA/26FFDA.png" /> `#26FFDA` |
| Enroll fail | blink | vivid red <img alt="#FF1744" src="https://placehold.co/16x16/FF1744/FF1744.png" /> `#FF1744` |
| Enroll timeout | blink | orange-red <img alt="#FF5A00" src="https://placehold.co/16x16/FF5A00/FF5A00.png" /> `#FF5A00` |

Tip: If you used triple-tap to disable RGB feedback, the device will keep working but will not show these patterns until you enable RGB again.

---

## Fingerprint (Lock variant only)

If your slave has a fingerprint sensor and it's enabled in the master configuration:
- The master controls the main flows: enable/disable verify loop, enroll, delete, query DB, adopt/release.
- During enrollment, the slave streams stage-by-stage progress so the UI can guide the user.
- When unpaired, an adopted sensor is released to default on boot and verify stays off until the master adopts.
While armed, fingerprint matches are still reported but never unlock locally.

---

## Power Saving / Sleep

- The device sleeps automatically after inactivity (default: a few minutes).
- Low/Critical battery disables the motor and may force sleep after a grace window.
See `readme/behavior.md` for the exact grace timing and sleep-pending rules.

---

## Technical Documentation

The detailed behavior and protocol specs live in the `readme/` folder:

- `readme/behavior.md` (authoritative behavior rules and precedence)
- `readme/device.md` (device implementation overview)
- `readme/transport.md` (transport protocol and module/opcode tables)
- `readme/fingerprint.md` (fingerprint wiring, stages, and CommandAPI mapping)
- `readme/master_now_layer.md` (master compatibility checklist; master firmware not in this repo)
- `readme/UserGuide.docx` (original user guide source document)
