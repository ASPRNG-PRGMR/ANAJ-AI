# Crop Sentinel AI — Phase 2 Deployment: Debugging Log & Known-Good Config

**Status: ✅ WORKING** — model deploys, runs inference, and produces correct
classifications on-device.

This document is the full record of what broke, why, and how it was fixed,
so future-you doesn't have to re-derive any of this from scratch.

---

## 1. Objective

Deploy the trained Edge Impulse plant-disease classifier to the ESP32-S3-CAM
board (OV3660 sensor, 16 MB flash, 8 MB PSRAM), using the board's real camera
pin mapping.

**Model classes:**
- `Pepper__bell___Bacterial_spot`
- `Pepper__bell___healthy`
- `Potato___Early_blight`
- `Potato___Late_blight`

## 2. Hardware / Toolchain (final known-good)

| Item | Value |
|---|---|
| MCU | ESP32-S3 |
| Module | ESP32-S3-WROOM-1, N16R8 class |
| Flash | 16 MB |
| PSRAM | 8 MB, **OPI** bus |
| Camera sensor | OV3660 |
| Camera framebuffer location | PSRAM (`CAMERA_FB_IN_PSRAM`) |
| Arduino ESP32 core | 3.3.11 |
| ESP-IDF (underlying) | v5.5.5 |
| Board (Tools menu) | ESP32S3 Dev Module |
| CPU Frequency | 240 MHz |
| **Flash Mode** | **QIO 80 MHz** ← critical, see §4 |
| Flash Size | 16 MB |
| **PSRAM (Tools menu)** | **OPI PSRAM** |
| Partition Scheme | Huge APP (3MB No OTA / 1MB SPIFFS) |
| USB Mode | Hardware CDC and JTAG |
| Upload Speed | 115200 |

Full FQBN from a successful build:
```
esp32:esp32:esp32s3:UploadSpeed=115200,USBMode=hwcdc,CPUFreq=240,
FlashMode=qio,FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi,
LoopCore=1,EventsCore=1,EraseFlash=none
```

## 3. Debugging Timeline

Chronological, deduplicated — each issue is recorded once, at the point it
was actually solved.

```
Edge Impulse model exported as Arduino library
        │
        ▼
Integrated into ESP32 camera inference sketch
        │
        ▼
Fixed camera pin mapping for this board (§4.1)
        │
        ▼
Boot loop: RTCWDT_RTC_RST repeating
        │
        ▼
Found & fixed: wrong Flash Mode (§4.2)  →  boots correctly
        │
        ▼
"Camera initialized" — camera path confirmed working
        │
        ▼
Toolchain break: esptool ModuleNotFoundError: No module named 'serial' (§4.3)
        │
        ▼
Fixed: installed pyserial for the correct python3 interpreter
        │
        ▼
Confirmed PSRAM actually active: psramFound()=1, size≈8.19 MB
        │
        ▼
Inference crash: "Failed to allocate persistent buffer of size 128,
does not fit in tensor arena and reached EI_MAX_OVERFLOW_BUFFER_COUNT"
followed by Guru Meditation Error (StoreProhibited)
        │
        ├── Ruled out: PSRAM absence (confirmed present & active)
        ├── Ruled out: general RAM exhaustion (~301 KB internal RAM free,
        │              ~8.17 MB PSRAM free at time of failure)
        ├── Ruled out: camera init failure (camera was already initializing fine)
        └── Ruled out: core version mismatch as root cause (error persisted
                       identically on both old and new core versions —
                       the core downgrade attempt was a dead end for THIS
                       specific error, even though it was a reasonable
                       thing to rule out)
        │
        ▼
Identified real cause: ESP32-S3-specific ESP-NN optimized kernels
(assembly-accelerated Conv/DepthConv, etc.) need extra persistent
buffers that the exported model's tensor arena wasn't sized for (§4.4)
        │
        ▼
Increased kTensorArenaSize: 256403 bytes (~250 KiB) → 512×1024 (512 KiB)
edited directly in tflite_learn_*_compiled.cpp
        │
        ▼
Still failed — error changed to a 1344-byte allocation failure
(confirms it was the ESP-NN buffers, not general undersizing)
        │
        ▼
Disabled ESP-NN optimized kernels:
EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN  1 → 0
in edge-impulse-sdk/classifier/ei_classifier_config.h
        │
        ▼
✅ SUCCESS — model runs, produces correct classifications
        │
        ▼
Tradeoff identified: inference now ~6.17s (vs. what would've been much
faster with ESP-NN enabled) — noted as a future optimization target,
not a blocker
```

## 4. Root Causes & Fixes (detail)

### 4.1 Camera pin mapping
The stock Edge Impulse Arduino example ships pin sets for `CAMERA_MODEL_ESP_EYE`
and `CAMERA_MODEL_AI_THINKER` — neither matches this board. Correct mapping
used (this is the same pin set as the board's other sketches in this
project, e.g. Phase 1/3):

```cpp
#define CAMERA_MODEL_ESP32S3_EYE

#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5

#define Y2_GPIO_NUM 11
#define Y3_GPIO_NUM 9
#define Y4_GPIO_NUM 8
#define Y5_GPIO_NUM 10
#define Y6_GPIO_NUM 12
#define Y7_GPIO_NUM 18
#define Y8_GPIO_NUM 17
#define Y9_GPIO_NUM 16

#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13
```

This alone got the camera driver compiling and (eventually, after §4.2)
initializing correctly. **This was never actually the blocker** — it's
included here because it's a real config decision worth keeping on record,
not because it caused any of the crashes below.

### 4.2 Boot loop — wrong Flash Mode (the actual RTCWDT_RTC_RST cause)
Symptom:
```
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
rst:0x10 (RTCWDT_RTC_RST)
```
repeating, even though the bootloader itself was loading fine (ROM → flash
read → bootloader all succeeded; only app startup failed).

Cause: **Flash Mode was set to OPI**, but this board's flash chip is
**QIO**, not OPI — only the *PSRAM* on this module is OPI. Mixing them up
(setting Flash Mode to match the PSRAM bus mode) causes the RTC watchdog to
reset the chip during app startup, before any of your code runs.

Fix:
```
Flash Mode: OPI  →  QIO   (80 MHz)
PSRAM:      OPI PSRAM     (unchanged — this one WAS correct)
```

After this change the board booted cleanly and diagnostics confirmed:
```
SPIRAM:  Total 8388608 B, Bus Mode: OPI
Flash:   Chip Size 16777216 B, Bus Speed 80MHz, Bus Mode: QIO
```

### 4.3 Toolchain: esptool `ModuleNotFoundError: No module named 'serial'`
Happened after switching Arduino ESP32 core versions. Not related to the
board at all — `esptool.py`'s bundled Python was resolving to the system
`python3`, which didn't have `pyserial` installed.

Fix (Fedora, but the same idea applies anywhere):
```bash
which python3
python3 --version
python3 -m pip install pyserial
```
If PEP 668 blocks it: `python3 -m pip install --break-system-packages pyserial`,
or use the distro package: `sudo apt install python3-serial` (Debian/Ubuntu).

### 4.4 The real inference crash — tensor arena vs. ESP-NN kernels
Symptom (this is the one that took the longest to pin down):
```
ERR: Failed to allocate persistent buffer of size 128,
does not fit in tensor arena and reached EI_MAX_OVERFLOW_BUFFER_COUNT
Guru Meditation Error: Core 1 panic'ed (StoreProhibited)
```

**What it looked like it might be, and why each was ruled out:**
- *PSRAM not working* — ruled out. Explicit diagnostic (`psramFound()`,
  `ESP.getPsramSize()`) confirmed ~8.19 MB active and correctly detected.
- *General memory exhaustion* — ruled out. At the moment of failure,
  ~301 KB internal RAM and ~8.17 MB PSRAM were still free. This was never
  a "the chip is out of memory" problem.
- *Camera not initializing* — ruled out. "Camera initialized" printed
  successfully every time, well before the crash.
- *Arduino core version* — investigated (downgraded from 3.3.11 toward
  2.0.x territory to match what the EI example was originally tested
  against) but the crash was identical either way. Reasonable thing to
  rule out given how the error first appeared right after a core-related
  toolchain problem, but it wasn't the cause.

**Actual cause:** the ESP32-S3 has assembly-optimized neural-network
kernels (ESP-NN — see `edge-impulse-sdk/porting/espressif/ESP-NN/`,
e.g. `esp_nn_conv_esp32s3.c`, `esp_nn_depthwise_conv_s8_esp32s3.c`) used
automatically for Conv/DepthConv/etc. These optimized kernels require
**additional persistent buffer allocations on top of the normal tensor
arena**, and Edge Impulse's exported arena size didn't account for that
extra need on this chip.

**Fix, in the order actually applied:**

1. Increased the tensor arena. In the exported library, inside
   `tflite-model/tflite_learn_*_compiled.cpp`:
   ```cpp
   // was:
   constexpr int kTensorArenaSize = 256403;      // ~250 KiB
   // changed to:
   constexpr int kTensorArenaSize = 512 * 1024;  // 512 KiB
   ```
   **Important:** this constant must be edited in that specific generated
   file. Adding `#define EI_CLASSIFIER_TENSOR_ARENA_SIZE ...` in your own
   sketch/`main.cpp` does **not** work — it's not read from there.

   This alone wasn't enough — the error just changed to a 1344-byte
   allocation failure, which was the confirming clue that this was about
   ESP-NN's extra buffers specifically, not just "arena too small overall."

2. Disabled ESP-NN. In
   `edge-impulse-sdk/classifier/ei_classifier_config.h`:
   ```cpp
   // was:
   #define EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN 1
   // changed to:
   #define EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN 0
   ```
   This forces standard (non-assembly-optimized) TFLite Micro kernels,
   which don't need the extra buffers. **This was the decisive fix.**

3. `EI_MAX_OVERFLOW_BUFFER_COUNT` was left at its default (10) in the
   final working config — it did not need to be changed once ESP-NN was
   disabled. (Raising it was tried as an intermediate experiment and is
   worth knowing about as an option, but it wasn't part of the final fix.)

**The tradeoff:** inference dropped from what would have been a fast
ESP-NN-accelerated run to **~6.17 seconds per classification**
(DSP ~5 ms, Anomaly 0 ms). Functionality over speed, for now — see §6.

**A cleaner alternative that was identified but not the path actually
taken:** Edge Impulse Studio → Deployment → Build → disable "EON
Compiler" → re-export. Maintainers have pointed to this as a first thing
to try for ESP32-S3 tensor-arena issues in general. It wasn't tested here
since disabling ESP-NN + raising the arena already worked, but it's worth
trying in a future pass at getting ESP-NN back (§6).

## 5. Final Known-Good Source Changes (summary)

```cpp
// Camera: ESP32-S3 / OV3660 pin mapping (§4.1)
#define CAMERA_MODEL_ESP32S3_EYE
// ... pins as listed in §4.1 ...

.fb_location = CAMERA_FB_IN_PSRAM,   // framebuffer in PSRAM

// tflite-model/tflite_learn_*_compiled.cpp
constexpr int kTensorArenaSize = 512 * 1024;   // was 256403

// edge-impulse-sdk/classifier/ei_classifier_config.h
#define EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN 0   // was 1

// precautionary, not the root-cause fix, but kept:
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
```

Arduino IDE settings: see the table in §2 (Flash Mode **QIO**, PSRAM **OPI**
— don't mix these up again).

## 6. Current Status & Confirmed Working Output

```
Predictions:
  Pepper__bell___Bacterial_spot: 0.01562
  Pepper__bell___healthy:        0.98438
  Potato___Early_blight:         0.00000
  Potato___Late_blight:          0.00000

Predictions (DSP: 5 ms., Classification: 6174 ms., Anomaly: 0 ms.)
```

| Stage | Status |
|---|---|
| ESP32-S3 boot | ✅ |
| 16 MB QIO Flash | ✅ |
| 8 MB OPI PSRAM | ✅ |
| OV3660 camera init | ✅ |
| JPEG capture → RGB888 conversion | ✅ |
| Edge Impulse DSP | ✅ |
| TFLite Micro inference | ✅ (slow — ESP-NN disabled) |
| Serial predictions | ✅ |

**Remaining issue is performance, not functionality.**

## 7. Next Steps (future optimization, not urgent)

- **Don't touch the working config casually.** Known-good baseline to
  preserve/revert to:
  ```cpp
  constexpr int kTensorArenaSize = 512 * 1024;
  #define EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN 0
  ```
  with `Flash Mode = QIO 80MHz`, `PSRAM = OPI PSRAM`.
- **Main open question:** can ESP-NN be re-enabled while keeping tensor
  arena allocation successful? Likely needs a larger arena than 512 KiB
  specifically to cover ESP-NN's extra buffers (PSRAM has plenty of
  headroom at 8 MB total, so this is a config/experiment problem, not a
  hardware limit) — or trying the EON-Compiler-disabled export path
  (§4.4, last paragraph) as an alternative route to the same goal.
  Treat as a separate, isolated experiment against this known-good
  checkpoint so a regression is easy to back out of.
- ~6.17s per inference is workable for the current phase (manual "Scan
  Crop" trigger, not continuous scanning), but worth revisiting once the
  full pipeline (Phases 3–8) is integrated and inference latency starts
  to matter for UX.

## 8. Quick Reference — If This Happens Again

| Symptom | Likely cause | Fix |
|---|---|---|
| `rst:0x10 (RTCWDT_RTC_RST)` boot loop, bootloader loads fine | Flash Mode / PSRAM bus mode mismatch | Flash Mode must be **QIO** on this board; only PSRAM is OPI |
| `esptool ... ModuleNotFoundError: No module named 'serial'` | `pyserial` missing for whichever `python3` esptool resolves to | `python3 -m pip install pyserial` (add `--break-system-packages` if PEP 668 blocks it) |
| `Failed to allocate persistent buffer of size N ... EI_MAX_OVERFLOW_BUFFER_COUNT` + Guru Meditation `StoreProhibited` | ESP32-S3 ESP-NN kernels need buffers the exported tensor arena doesn't account for | Increase `kTensorArenaSize` in `tflite_learn_*_compiled.cpp`, and/or set `EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN 0` in `ei_classifier_config.h` |
| PSRAM seems suspect | — | Confirm with `Serial.printf("PSRAM found: %d, size: %u\n", psramFound(), ESP.getPsramSize());` before ruling anything else in/out |
