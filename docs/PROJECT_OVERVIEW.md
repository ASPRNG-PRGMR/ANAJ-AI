# Project Anaj AI — Early Disease Prediction & Outbreak Mapping

*(Formerly "Crop Sentinel AI" — renamed.)*

**Track:** Arduino Physical AI Challenge
**Sensing:** camera only, one physical sensor type in the whole system
**AI location:** on-device — a two-tier split across genuine Arduino silicon (Uno R3) and an ESP32-S3-CAM running the Arduino core, not offloaded to a laptop/cloud

---

## 0. Current Build Status (read this first — supersedes phase-by-phase framing below)

**Phases 1–6 are built, flashed, and have run real end-to-end scans on hardware.** The architecture actually built diverges from an earlier draft of this doc in one important way — see the correction note immediately below — everything else in §0–§6 describes what's *actually running*, not a plan.

| Phase | Status | Notes |
|---|---|---|
| 1 — ESP32-S3-CAM camera capture | ✅ Done | |
| 2 — Primary CNN (Edge Impulse) | ✅ Done | See `PHASE2_DEPLOYMENT_DEBUGGING.md` for the full deployment war story (tensor arena / ESP-NN fix) |
| 3 — ESP32 ↔ Uno UART link | ✅ Done | Real 64-feature JSON payload, not the early stub |
| 4 — Uno tiny model | ✅ Done | Fixed-point logistic regression, trained via `train_tiny_model.py` on PlantVillage |
| 5 — Touch UI bring-up | ✅ Done (hardware currently down) | See §0.1 |
| 6 — Zone tagging + history | ✅ Done | In-memory ring buffer, `MAX_ZONES=6`, `SCANS_PER_ZONE=4` |
| 7 — Trend/progression | ⏳ Not started | |
| 8 — Telegram alerts | ⏳ Not started | |
| 9 — Outbreak heatmap | ⏳ Not started | Web dashboard (§0.2) now covers an early version of this |
| 10 — Power + full run-through | ⏳ Not started | |
| 11 — ESP-CAM second zone (stretch) | ⏳ Not started | |

### 0.1 Correction: the display lives on the Uno R3, not the ESP32-S3-CAM

An earlier draft of this document planned to move the TFT + touch UI onto the ESP32-S3-CAM. **That's not what got built.** The ILI9341 + XPT2046 touch panel is wired to and driven by the **Arduino Uno R3**. The ESP32-S3-CAM has no display at all — it's camera + CNN + feature extraction + UART only. §3–§6 below describe this actual, as-built split. Treat any earlier reference to "display on ESP32" as stale.

### 0.2 Display plan: Primary + Backup

The Uno's physical TFT is currently non-functional (hardware fault) and a replacement has been ordered. Expectation is that the new panel will work fine once it arrives — this isn't a permanent redesign, it's a sequencing reality:

- **Primary display: the physical TFT + touch panel on the Uno R3**, as designed and already coded (Phases 5–6). This is the intended long-term interface — on-station, no PC required, matches the original farmer-facing UX vision.
- **Backup display: a PC web dashboard**, fed over the Uno's existing USB serial link (see `WEB_DASHBOARD.md` for the full design). Built specifically because the touchscreen is currently the *only* way to trigger a scan or see a result, which means the system is fully untriggerable right now without it.

The backup isn't going away once the new screen arrives, though — it's genuinely useful as a second, judge-facing telemetry view during the demo (a laptop screen is easier for multiple people to see at once than a 2.4" panel), and it's a good place to prototype the outbreak heatmap (Phase 9) before committing it to the small physical screen. So: **primary for the real on-station experience, backup both as a stopgap now and as a permanent judge/dev view going forward.**

*(Naming these "Primary"/"Backup" for now — rename if a better pair of terms comes to mind, e.g. "Station Display" / "Ops Dashboard".)*

### 0.3 Design principle going forward: new model work goes on the Arduino

This is the Arduino Physical AI Challenge, and right now the ESP32-S3-CAM is doing more of the visible "AI work" (the primary CNN) than the Uno R3 is. That split is real and justified (§1) — no board in the kit is both camera-capable and Arduino-brand silicon — but it means the Uno's model story is the one worth *growing*, not the ESP32's. **Going forward, if a task could reasonably run on either board, put it on the Uno R3.** Concretely:
- Phase 7's trend/progression calculation was already scoped for the Uno — keep it there.
- Any future model refinement (better features, a slightly richer secondary model, confidence calibration) should target `train_tiny_model.py` → `tiny_model.h`, not the ESP32's CNN.
- The web dashboard (§0.2) is explicitly a **passive telemetry view fed by the Uno's own serial output** — all decision-making and both AI models still run entirely on-device, on the two boards. The dashboard doesn't run or influence any inference; it just displays what the Uno already computed and sends out.

---

## 1. Where the AI Actually Runs, and Why It's Split Across Two Boards

The challenge requires AI running "on Arduino." In practice, hackathons phrase this two different ways, and it matters which one this event means:

1. **Loose reading:** any board flashed and coded through the Arduino IDE/core counts as "Arduino" — this covers ESP32 boards.
2. **Strict reading:** must be literal Arduino-brand silicon (AVR Uno/Nano/Mega, or Arduino's own SAMD/STM32/mbed boards like Nicla/Portenta) — ESP32 boards don't count.

Since this team's kit has no camera-capable *and* Arduino-brand board, satisfying the strict reading with vision AI alone is not possible with this hardware. Rather than gamble on which reading applies, the system runs **two real, independent on-device models**, so the pitch is correct under either interpretation:

| Board | Role | AI it runs | Why this board |
|---|---|---|---|
| **ESP32-S3-CAM** | Captures the leaf image, runs the primary disease classifier, extracts a compact feature vector from the same frame. | Quantized int8 CNN (Edge Impulse-trained), 4-class leaf disease classification. | Only board in the kit with a camera and enough RAM/PSRAM to run a real vision CNN via TFLite Micro. |
| **Arduino Uno R3** | Owns the on-station UI, zone tagging, history, and runs a second, independent tiny model on the feature vector (not the image) sent from the ESP32-S3-CAM. | A fixed-point logistic regression, trained offline in Python (`train_tiny_model.py`) on the same PlantVillage-derived features, exported to plain `int16`/`int32` C arrays. | Genuine on-device inference on actual Arduino (AVR) silicon — not a UI passthrough. Never sees the raw image, only 64 numbers per scan. |
| **ESP-CAM** *(optional, stretch — §7 Phase 11)* | Second physical scanning station for a second field zone. | Same primary classifier, scaled down if needed. | Not required for core acceptance tests. |

**One-sentence answer for judges:** *"The vision model — a quantized CNN classifying leaf disease from the camera — runs on-device on the ESP32-S3-CAM, no cloud or laptop inference. A second, independent tiny classifier also runs on-device directly on the Arduino Uno R3's AVR core, using a 64-value numeric feature vector rather than the image, so we have real on-device AI on literal Arduino silicon as well."*

**What each board explicitly does NOT do:** the ESP32-S3-CAM never makes the final farmer-facing verdict alone; the Uno R3 never receives or processes the raw image, only class/confidence plus the 64-value feature vector.

---

## 2. Problem & Solution

**Problem:** Farmers typically only detect crop disease once visible damage has appeared — by then infection has often spread to neighboring plants, driving unnecessary pesticide use and yield loss.

**Solution:** A two-board scanning station. The ESP32-S3-CAM does the primary vision AI. The Arduino Uno R3 runs a second, lightweight on-device model as a verification/second-opinion layer, owns the on-station touch UI, zone tagging, and per-zone history — using only a camera as the physical sensor, plus manual zone tagging in place of GPS.

---

## 3. Full System Architecture (as built)

```
┌───────────────────────────┐   UART, 9600 baud    ┌──────────────────────────┐   USB Serial   ┌──────────────┐
│      ESP32-S3-CAM          │ ───────────────────▶ │      ARDUINO UNO R3       │ ─────────────▶ │  PC / laptop  │
│                             │  JSON: class,         │                            │  DASH: lines   │              │
│  - OV3660 camera            │  confidence,          │  - ILI9341 TFT + XPT2046  │  (outbound)    │  Web dashboard│
│  - Edge Impulse CNN         │  64-value feat[]      │    touch (on-station UI)  │                │  (backup      │
│    (int8 quantized)         │ ◀─────────────────── │  - Tiny on-device model    │ ◀───────────── │   display)    │
│  - extractFeatures()        │  "SCAN\n" trigger     │    (2nd, independent AI)  │  DASH_SCAN:     │              │
│  - Runs PRIMARY vision AI   │                        │  - Zone tagging + history │  (remote        │              │
│    ON-DEVICE                │                        │  - Combine rule            │   trigger,      │              │
│  - No display                │                        │                            │   inbound)      │              │
└───────────────────────────┘                        └──────────────────────────┘                └──────────────┘
        ▲ PRIMARY VISION AI                              ▲ SECOND, INDEPENDENT AI                    ▲ passive
        ▲ (GPIO 38/39)                                    ▲ + ON-STATION UI (Uno pins,                ▲ telemetry
                                                             see §4.3) + HISTORY                        view only
```

Only the Uno R3 is ever connected to the PC. The ESP32-S3-CAM talks exclusively to the Uno over its own dedicated UART link; the Uno is the single point of contact for both the field hardware and the dashboard.

### 3.1 Data flow, end to end

```
1. Trigger arrives at the Uno R3 -- either:
   (a) farmer taps "Scan Crop" then a zone cell on the touchscreen (primary), or
   (b) the web dashboard sends DASH_SCAN:<zone> over USB serial (backup, §0.2 / WEB_DASHBOARD.md)
2. Uno R3 sends SCAN\n to the ESP32-S3-CAM over UART
3. ESP32-S3-CAM captures a frame from its onboard camera
4. ESP32-S3-CAM runs the on-device CNN (primary classifier) on that frame
5. ESP32-S3-CAM runs extractFeatures() on the SAME frame -> 64-value feat[]
6. ESP32-S3-CAM sends one JSON line back over UART:
   {"class":"Pepper__bell___healthy","confidence":0.91,"feat":[64 ints]}
7. Uno R3 runs its own tiny model on feat[] -> independent local verdict
8. Uno R3 combines both verdicts (§5.3), attaches the zone tag + timestamp, stores to history
9. Uno R3 renders the result on its own screen AND prints a DASH: JSON line over USB serial
10. (Not yet built) Uno R3 computes trend vs. this zone's previous readings, risk score, fires FIRE_ALERT if warranted
```

**Critical property preserved:** the ESP32-S3-CAM never sends the raw image over UART — only the classification result and the 64-value feature vector. Division of labor: ESP32-S3-CAM = camera + primary vision AI, Uno R3 = second-opinion AI + UI + zone/history logic + (eventually) alert decisions.

---

## 4. Hardware

### 4.1 Bill of materials

| Component | Qty | Notes |
|---|---|---|
| Arduino Uno R3 | 1 | Runs the second-tier AI model, UI, zone logic, history. Only board connected to the PC. |
| ESP32-S3-CAM | 1 | ESP32-S3-WROOM-1 N16R8 (16MB flash, 8MB OPI PSRAM), OV3660 camera. Runs the primary vision AI. Needs external 5V/3.3V supply. |
| ESP-CAM (AI-Thinker-style) | 1 | Optional/stretch — second scanning zone (§7 Phase 11). Not required for core acceptance tests. |
| ILI9341 TFT, 240x320, SPI, with XPT2046 touch (breakout module) | 1 | On-station display + zone-tagging UI, driven by the **Uno R3**. Currently faulty; replacement ordered — see §0.2. |
| LiPo battery + TP4056 charge module (or separate 5V supply for the ESP32-S3-CAM, 9V/battery for the Uno) | 1–2 | ESP32-S3-CAM (camera) is the bigger continuous draw. |

**Explicitly not in the BOM:** soil moisture sensor, DHT22/SHT31, GPS module, SG90 servo. Camera is the only physical sensor in the system.

### 4.2 Wiring — ESP32-S3-CAM ↔ Uno R3 (UART)

The Uno R3's single hardware UART (pins 0/1) is shared with the USB-to-serial link used for programming, the Serial Monitor, **and now the web dashboard's USB serial connection**. **Do not wire the ESP32-S3-CAM to pins 0/1.** Use `SoftwareSerial` on two other Uno pins, matched to two free GPIOs on the ESP32-S3-CAM side.

| Uno R3 pin | ESP32-S3-CAM pin | Signal |
|---|---|---|
| D2 (SoftwareSerial RX) | GPIO 39 (UART TX) | ESP32-S3-CAM → Uno |
| D3 (SoftwareSerial TX) | GPIO 38 (UART RX) | Uno → ESP32-S3-CAM |
| GND | GND | Common ground — do not skip this |

**Why GPIO 38/39 on the ESP32 side, specifically:** GPIO 43/44 (the obvious first choice) are actually UART0 on this board, which is the *same* pins the Serial Monitor connection uses when USB CDC On Boot is disabled — using them for a second UART fights with and breaks the console. GPIO 26–37 are reserved for this module's octal flash + octal PSRAM (N16R8) and are never safe to repurpose. GPIO 19/20 are native USB D-/D+. 38/39 are free general-purpose pins outside all of that.

Keep the baud rate modest (9600) — `SoftwareSerial` on an 8-bit AVR is unreliable much above that, especially while TFT/touch SPI traffic is also active.

### 4.3 Wiring — Uno R3 ↔ ILI9341 + XPT2046 (SPI, breakout module)

| Signal | Uno R3 pin |
|---|---|
| TFT CS | D10 |
| TFT DC | D8 |
| TFT RESET | D9 |
| TFT/Touch MOSI | D11 (hardware SPI) |
| TFT/Touch SCK | D13 (hardware SPI) |
| TFT/Touch MISO | D12 (hardware SPI) |
| Touch CS | D7 |
| Touch IRQ | D6 |

Touch calibration constants (`TS_MINX/MAXX/MINY/MAXY` in the sketch) are generic starting values — recalibrate against the actual panel once the replacement arrives.

### 4.4 ESP32-S3-CAM power

Power the ESP32-S3-CAM from its own regulated 5V source — camera inrush current is enough to brown out if shared with the Uno's onboard regulator. Common ground between the two boards is still required for the UART link to work even with separate supplies.

### 4.5 Uno R3 ↔ PC (USB, for the web dashboard)

The Uno's normal programming/USB-serial connection (115200 baud) now also carries `DASH:` result lines outbound and accepts `DASH_SCAN:<zone>` commands inbound. This is the same physical connection you already use for flashing and the Serial Monitor — no extra wiring. See `WEB_DASHBOARD.md` for the full protocol and dashboard design.

---

## 5. AI / ML Pipeline

### 5.1 Primary model: on-device image classification (ESP32-S3-CAM)

- **Classes (actual, as trained):** `Pepper__bell___Bacterial_spot`, `Pepper__bell___healthy`, `Potato___Early_blight`, `Potato___Late_blight`
- **Training data:** PlantVillage dataset
- **Deployment path:** trained/exported via Edge Impulse (ESP32 target profile), flashed as a generated Arduino library. Full deployment war story — including the ESP-NN/tensor-arena crash and its fix — is in `PHASE2_DEPLOYMENT_DEBUGGING.md`.
- **Inference output per scan:** `{class, confidence, feat}` — `feat` is the 64-value feature vector (§5.2). The raw image never leaves the ESP32-S3-CAM.

### 5.2 Secondary model: on-device verification (Arduino Uno R3)

**This model runs on real Arduino (AVR) silicon and is the direct answer to a strict "AI must be on Arduino" judging read.**

- **Task:** binary "likely healthy vs. likely diseased" verdict from the 64-value feature vector — not the image.
- **Feature vector (`feat`, actual, as built — v2):** a 4×4 spatial grid over the CNN's own input frame, 4 stats per cell: **mean brightness, green ratio, horizontal edge density, brightness contrast (std-dev)** = 64 uint8 values. Deliberately mixes two color cues with two texture cues, rather than the earlier v1 design (four flavors of average color), so the Uno's model is checking a meaningfully different signal from the CNN — not just a cheaper echo of it. Computed identically on the ESP32 (`extractFeatures()`) and in Python (`train_tiny_model.py`'s `extract_features()`) — the two must be kept in lockstep, and **any feature-set change requires retraining** (a stale model trained on the old feature meaning still compiles and runs, it just produces meaningless output, silently).
- **Model:** fixed-point logistic regression. `score = bias + Σ(weight[i] × feat[i])`, all integer arithmetic; the sign of `score` alone gives the class (no sigmoid needed on-device except to compute a confidence percentage). Trained in Python via `sklearn.LogisticRegression`, quantized to Q8 fixed-point (`scale=256`), exported to `tiny_model.h` as plain `int16_t`/`int32_t` C arrays.
- **Label rule:** any PlantVillage folder name containing "healthy" → label 0, else → label 1.
- **Output:** a local class (`healthy`/`diseased`) and confidence, independent of the ESP32-S3-CAM's verdict.

### 5.3 Combining the two verdicts

The ESP32-S3-CAM's classification is the **primary, displayed result**. The Uno R3's verdict is shown as a small "on-Arduino verified ✓/✗" indicator next to it. If the two disagree, the scan is flagged "MISMATCH — rescan" rather than arbitrated.

### 5.4 Progression/trend model (not yet built — Phase 7)

**This will not be a machine learning model.** Planned as a simple regression (curve fit) over a zone's last N confidence readings, computed on the Uno R3. Per §0.3, this stays on the Uno.

### 5.5 Cold-start limitation

Day-one scans of a zone are classification-only — no history yet to fit a trend to.

---

## 6. Communication Protocols

### 6.1 ESP32-S3-CAM ↔ Uno R3 (field link, §4.2)

**Uno R3 → ESP32-S3-CAM (trigger):**
```
SCAN\n
```

**ESP32-S3-CAM → Uno R3 (result), one line per scan:**
```json
{"class":"Pepper__bell___healthy","confidence":0.91,"feat":[71,110,72,110,...64 values total]}\n
```

`class` is one of the 4 real trained classes (§5.1). `confidence` is a float 0.0–1.0. `feat` is always exactly 64 ints, 0–255 each.

### 6.2 Uno R3 ↔ PC (dashboard link, §4.5 / `WEB_DASHBOARD.md`)

**Uno R3 → PC (result), one line per completed scan, prefixed to separate it from ordinary debug output:**
```
DASH:{"zone":"B4","class":"Pepper__bell___healthy","confidence":0.91,"tinyVerdict":"healthy","agree":true,"ts":123456}
```

**PC → Uno R3 (remote trigger, works even with the touchscreen down):**
```
DASH_SCAN:B4\n
```

### 6.3 Not yet built: alert command

```
FIRE_ALERT {"zone":"B4","disease":"early_blight","confidence":0.94,"spread":["B5","C4"]}\n
```
Planned direction: Uno R3 → ESP32-S3-CAM once Phase 8 (Telegram alerts) starts. The ESP32-S3-CAM stays dumb about alert *logic* — it only fires the message it's told to fire.

---

## 7. Build Phases

See §0's status table for where things actually stand. Phases 1–6 are complete; this section is kept for the acceptance-test definitions, which remain the source of truth for what "done" means for each phase.

### Phase 1 — ESP32-S3-CAM: camera capture only ✅
**Acceptance test:** trigger a capture, confirm via serial log that a frame was grabbed. — Passed.

### Phase 2 — ESP32-S3-CAM: primary classifier ✅
**Acceptance test:** point the camera at each of the 4 classes, confirm correct classification with sensible confidence. — Passed; see `PHASE2_DEPLOYMENT_DEBUGGING.md` for the deployment fixes required to get here.

### Phase 3 — ESP32-S3-CAM ↔ Uno R3 UART link ✅
**Acceptance test:** triggering from the Uno produces a correctly-formatted JSON result on the Uno side, sourced from a real classification. — Passed, with the real 64-value `feat[]`, not the early 8-value stub.

### Phase 4 — Uno R3: secondary tiny model ✅
**Acceptance test:** a known `feat` vector piped in produces a sensible healthy/diseased verdict, verified against held-out examples. — Passed.

### Phase 5 — Touch UI bring-up ✅ (hardware currently down, §0.2)
**Acceptance test:** tapping "Scan Crop" visibly triggers a scan and the combined result renders on the display. — Passed prior to the display hardware fault.

### Phase 6 — Zone tagging + history storage ✅
**Acceptance test:** scan the same zone twice; confirm both readings are retrievable and correctly ordered by time. — Passed.

### Phase 7 — Trend/progression calculation ⏳
- Implement the curve fit (§5.4) over a zone's stored history
- **Acceptance test:** with ≥3 scans of one zone showing rising confidence, the trend screen shows a plausible upward projection, clearly labeled as a prediction.

### Phase 8 — Risk scoring + Telegram alerts ⏳
- Define the alert threshold on the Uno R3
- Wire `FIRE_ALERT` (§6.3) from Uno → ESP32-S3-CAM; ESP32-S3-CAM handles Wi-Fi + Telegram
- **Acceptance test:** forcing a high-confidence disease reading fires a real Telegram message with correct zone/confidence/spread fields.

### Phase 9 — Outbreak map (heatmap grid) ⏳
- Build the grid UI (🟩/🟨/🟥 by zone risk)
- The web dashboard (`WEB_DASHBOARD.md`) is a good place to prototype this before committing to the small physical screen
- **Acceptance test:** heatmap correctly reflects stored zone risk levels; tapping a red cell shows the right zone's detail.

### Phase 10 — Power + full run-through ⏳
- Measure current draw, execute the full demo end to end on battery power
- **Acceptance test:** demo completes end to end on battery power.

### Phase 11 — (Optional stretch) ESP-CAM as a second live zone ⏳

---

## 8. Power Notes

The ESP32-S3-CAM (camera) is the bigger continuous draw on its side — size its battery/solar budget accordingly. The Uno R3, driving only a small TFT and no Wi-Fi, is a comparatively light draw.

---

## 9. Telegram Alert Format (not yet built)

```
⚠️ Anaj AI Alert
Zone: B4 (Row 3, Plant 7)
Disease: Early Blight
Confidence: 94%
Predicted Spread: → Zone B5, C4
Inspection Recommended: Row 3, Plant 7 and neighbors
```

---

## 10. Demo Script

1. **Primary path (once the new display arrives):** station's touchscreen shows the idle screen → tap **Scan Crop** → tag zone → real scan → combined verdict on-screen.
2. **Backup path (current, and also a valid judge-facing demo on its own):** open the web dashboard on a laptop → click a zone → **Trigger Scan** → watch the same real pipeline (ESP32 CNN → Uno tiny model → combined verdict) render in the browser instead. See `WEB_DASHBOARD.md`.
3. Trigger a second scan of the *same zone* with rising confidence to demonstrate the progression trend (once Phase 7 exists).
4. Telegram alert fires once threshold is crossed (once Phase 8 exists).
5. Show the outbreak heatmap — either the physical screen or the dashboard — with a few historical zone readings.

---

## 11. Things to Address Before Judging

- **"Where does the AI actually run?"** — Both halves ready: *(1)* the vision classifier is a quantized int8 model on-device on the ESP32-S3-CAM; *(2)* a second, independent tiny model also runs on-device on the Arduino Uno R3's own AVR silicon, on a 64-value feature vector, not the image.
- **"Is ESP32 really an Arduino?"** — Be upfront: Espressif silicon programmed through the Arduino IDE/core. The Uno R3's tiny model exists specifically so the system has a true answer under a strict reading. Confirm the organizers' actual rule wording before judging.
- **"Is the web dashboard doing any of the AI?"** — No, and say so plainly: it's a passive display fed by the Uno's serial output, added because the physical screen is temporarily down. Both models still run entirely on the two boards; the dashboard neither computes nor influences any inference.
- **"Is the trend prediction also AI?"** — No: lightweight regression on the Uno R3, not a model.
- **"Prove the Uno model is really doing something."** — It's a small logistic regression; offer to walk through the actual weights in `tiny_model.h` — auditable in a way a black-box CNN isn't.
- **Manual zone tagging is a real UX cost** — no GPS means a human walks the field and tags zones.
- **Cold-start problem** — day-one scans of a zone are classification-only.
- **Dataset/training source** — PlantVillage, for both models.
- **Field validation** — not yet validated against a real outbreak; frame as the natural next step.
- **Why the display went down mid-build** — be upfront if asked; the backup dashboard exists specifically because a hackathon-scale system needs to keep working through hardware failures, and that's a legitimate engineering story, not something to hide.
