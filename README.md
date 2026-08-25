# Project Anaj AI — Early Crop Disease Detection & Field Scanning Station

*(formerly "Crop Sentinel AI")*

A two-board field-scanning station: an **ESP32-S3-CAM** runs an on-device CNN to classify leaf disease from a live camera frame, and an **Arduino Uno R3** runs a second, independent on-device model on a compact feature vector as a cross-check — plus owns the on-station touch UI, zone tagging, and per-zone scan history.

## Status

Built for a hackathon (Arduino Physical AI Challenge track) but **not submitted** — the track's required board was the **Arduino UNO Q**, which wasn't available. This build substitutes an **Arduino Uno R3** instead, which changes what "AI on Arduino" can honestly claim (see `docs/PROJECT_OVERVIEW.md` §1 for the full reasoning). Left running and documented here afterward rather than shelved.

**What's actually working, on real hardware:** camera capture → primary CNN classification → real UART link carrying a 64-value feature vector → Uno-side secondary model → zone tagging and history. The on-station touchscreen is currently non-functional (hardware fault, replacement pending) — a serial-based remote-trigger protocol (`DASH_SCAN:`) exists as a working interim substitute. Full phase-by-phase status is in `docs/PROJECT_OVERVIEW.md` §0.

## Gallery

*(Add photos/video here — hardware setup, the touchscreen UI when it was working, sample scans, the two boards wired together, etc. Drop image files into `media/` and reference them below, e.g.:)*

```md
![Station overview](media/station-overview.jpg)
![ESP32-S3-CAM + Uno wiring](media/wiring-closeup.jpg)
![Touch UI - result screen](media/ui-result-screen.jpg)
```

<!-- Pictures go here -->

## How it works (short version)

```
Leaf → ESP32-S3-CAM camera → on-device CNN (primary verdict)
                            → 64-value feature vector ─┐
                                                        ▼
                                          Arduino Uno R3: second,
                                          independent on-device model
                                          (fixed-point logistic regression)
                                                        │
                                    combined result → touch UI (or dashboard)
```

Full architecture, wiring diagrams, protocol spec, and the reasoning behind the two-model split live in `docs/PROJECT_OVERVIEW.md`.

## Repo structure

```
ANAJ-AI
├── README.md                          ← you are here
├── docs/
│   ├── PROJECT_OVERVIEW.md            ← full architecture, wiring, protocols, build-phase status
│   ├── PHASE2_DEPLOYMENT_DEBUGGING.md ← ESP32-S3-CAM + Edge Impulse deployment war story
│   └── WEB_DASHBOARD.md               ← spec for the serial-fed backup/judge dashboard
├── firmware/
│   ├── esp32-cam/
│   │   ├── esp32_phase2_3_4/          ← flash this to the ESP32-S3-CAM
│   │   │   └── esp32_phase2_3_4.ino
│   │   └── reference_phase1_camera_capture/  ← standalone camera-only reference, not part of the main build
│   │       └── phase1_camera_capture.ino
│   └── arduino-uno/
│       └── uno_phase4_5_6/            ← flash this to the Uno R3
│           ├── uno_phase4_5_6.ino
│           └── tiny_model.h           ← trained model weights (regenerate via ml/training/)
├── ml/
│   └── training/
│       ├── train_tiny_model.py        ← trains the Uno's secondary model, outputs a fresh tiny_model.h
│       └── requirements.txt
└── media/                             ← photos/video go here (see Gallery above)
```

Each `firmware/*/*/` folder is a complete Arduino sketch — folder name matches the `.ino` filename, which the Arduino IDE requires. Open the folder directly in the IDE, don't just grab the loose `.ino` file.

## Getting started

### 1. Flash the ESP32-S3-CAM
Open `firmware/esp32-cam/esp32_phase2_3_4/` in the Arduino IDE. Requires the Edge Impulse-exported inferencing library for the trained CNN (not included here — export your own from Edge Impulse Studio, or see `docs/PHASE2_DEPLOYMENT_DEBUGGING.md` for the full deployment process and board settings that actually work).

### 2. Train the Uno's model
```bash
cd ml/training
pip install --break-system-packages -r requirements.txt
python train_tiny_model.py --data-dir /path/to/PlantVillage \
  --resize-w <W> --resize-h <H> --out-dir ./out
```
`<W>`/`<H>` must match the ESP32's actual classifier input resolution — it prints this at boot (`Classifier input resolution: WxH`). Copy the resulting `out/tiny_model.h` into `firmware/arduino-uno/uno_phase4_5_6/`, replacing the existing one.

### 3. Flash the Arduino Uno R3
Open `firmware/arduino-uno/uno_phase4_5_6/` in the Arduino IDE (with the fresh `tiny_model.h` from step 2 sitting alongside the `.ino`) and flash.

### 4. Wire the two boards together
See `docs/PROJECT_OVERVIEW.md` §4.2 for exact pins — short version: Uno D2/D3 ↔ ESP32 GPIO 38/39, common GND. **Do not** use ESP32 GPIO 43/44 for this link — those conflict with the ESP32's own Serial Monitor connection.

### 5. Trigger a scan
- **Via touchscreen** (if yours is working): tap "Scan Crop" → tag a zone → done.
- **Via serial** (works even with no screen): open a terminal on the Uno's USB port at 115200 baud, type `DASH_SCAN:B4`, press enter.

## Known limitations

- Touchscreen hardware is currently faulty; see `docs/WEB_DASHBOARD.md` for the interim serial-based workaround and the planned browser dashboard.
- The Uno's model gives a binary healthy/diseased verdict, not the CNN's full 4-class breakdown — "agreement" between the two models means "both think it's diseased," not "both picked the same disease."
- Trained against a small PlantVillage subset (2 species, asymmetric healthy/diseased coverage) — see `docs/PROJECT_OVERVIEW.md` §11 for the honest caveat.
- Not field-validated against a real outbreak.
