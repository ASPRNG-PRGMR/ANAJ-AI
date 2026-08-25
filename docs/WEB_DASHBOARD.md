# Web Dashboard — Backup Display + Judge-Facing Telemetry

**Status: protocol + Uno-side support already built (see §2). PC-side app not yet built — this doc is the spec for it.**

## 1. Why this exists

The Uno R3's physical touchscreen is currently non-functional (hardware fault, replacement ordered). Until it arrives, **there is no way to trigger a scan or see a result at all** — the touchscreen was the only input and only output. That's the immediate problem this solves.

It's not purely a stopgap, though. Once the new screen is in, this dashboard stays useful as a **second, judge-facing view** — a laptop screen is easier for a group of judges to see at once than a 2.4" panel, and it's a good place to prototype the outbreak heatmap (README §7 Phase 9) before committing it to the small physical display. So: backup now, permanent second view going forward. See README §0.2 for the naming/framing of this alongside the physical display.

**What this explicitly is not:** it does not run, host, or influence any AI model. Both the primary CNN (ESP32-S3-CAM) and the secondary tiny model (Uno R3) keep running entirely on-device, exactly as before. This dashboard only displays what the Uno already computed and already decided to send — a passive telemetry view, not a third compute node. Worth stating plainly if a judge asks, per README §11.

## 2. Architecture

```
┌──────────────┐   USB Serial, 115200 baud   ┌──────────────┐   HTTP/SSE    ┌─────────────┐
│  Arduino Uno  │ ──────────────────────────▶ │  PC backend   │ ────────────▶ │   Browser    │
│      R3       │   DASH:{...} result lines    │  (Python)     │               │  (dashboard) │
│               │ ◀────────────────────────── │               │ ◀──────────── │              │
└──────────────┘   DASH_SCAN:<zone> commands  └──────────────┘   POST /scan  └─────────────┘
```

Only one physical link exists: the Uno's normal USB connection (the same one used for flashing and the Serial Monitor). No new wiring. The PC backend opens that same serial port, reads it, and serves a local webpage.

## 3. Protocol (already implemented on the Uno side)

This is live in `uno_phase4_5_6.ino` as of this doc — the PC side just needs to speak it.

**Uno → PC**, printed once per successfully-parsed scan, prefixed with `DASH:` specifically so it's distinguishable from the Uno's other debug prints on the same Serial connection (`[TX -> ESP32]`, `[HISTORY]`, etc. — the backend should ignore any line that doesn't start with `DASH:`):
```
DASH:{"zone":"B4","class":"Pepper__bell___healthy","confidence":0.91,"tinyVerdict":"healthy","agree":true,"ts":123456}
```
- `ts` is `millis()` since the Uno last booted — not wall-clock (no RTC on this build). Fine for ordering within one session; resets on power cycle. The PC backend can stamp its own wall-clock time on arrival if that's useful for the dashboard, since the PC always has a real clock.

**PC → Uno**, to trigger a scan remotely (works even with the touchscreen non-functional, since it's handled independently of touch state in `loop()`):
```
DASH_SCAN:B4\n
```
Valid zones are the same grid the touchscreen uses: letter `A`–`E` + digit `1`–`5` (e.g. `B4`). No response line is sent immediately — the normal `DASH:` result line arrives ~6-8 seconds later (dominated by the ESP32's ~6.2s CNN inference time), same as a touchscreen-triggered scan.

## 4. PC backend

**Suggested stack:** Python, since `pyserial` is already a dependency of this project (used for the ESP32 toolchain) and `train_tiny_model.py` already establishes Python as the project's PC-side language.

- `pyserial` to open the Uno's port and read lines
- A small web framework for the local server — **Flask** is the simplest fit here (one process, no build step, easy to demo from a single `python app.py`)
- **Server-Sent Events (SSE)** for pushing new `DASH:` readings to the browser as they arrive, rather than polling — simple to implement in Flask (a generator endpoint), no WebSocket library needed, and one-directional push is all this needs (the browser never needs to stream data back, only issue occasional scan-trigger requests)

**Responsibilities:**
1. Open the Uno's serial port on startup (auto-detect or take `--port` as an argument — reuse the same "check `which python3` / list serial ports" troubleshooting muscle from earlier in this project if the port isn't obvious).
2. Continuously read lines; parse any line starting with `DASH:` as JSON; ignore everything else (or optionally log it to the console for debugging — useful during dashboard development itself).
3. Keep an in-memory history per zone (the PC has effectively unlimited storage compared to the Uno's `MAX_ZONES=6`/`SCANS_PER_ZONE=4` ring buffer — no need to mirror that constraint here; keep everything for the session).
4. Serve the dashboard page + an SSE endpoint that pushes each new reading to connected browsers.
5. Expose a `POST /scan` endpoint (or similar) that takes a `zone` parameter, writes `DASH_SCAN:<zone>\n` to the serial port, and returns immediately (the result arrives asynchronously via SSE a few seconds later, same as the physical flow).

## 5. Frontend

Single HTML page, vanilla JS is enough (no build step needed for a hackathon timeline) — optionally pull in a lightweight charting library via CDN (e.g. Chart.js) once trend history exists.

**What to show (phased — see §6):**
- **Connection status** — is the serial port open, last message received how long ago (useful for judges to see the link is live, not just static content)
- **Zone grid** — same 5×5 A1–E5 layout as the physical touchscreen, each cell colored by that zone's latest verdict (green/red) and clickable to trigger a scan of that zone
- **Latest result card** — class, confidence, zone, "on-Arduino verified ✓/✗" — mirrors exactly what the physical result screen would show
- **Per-zone history** — a simple table or line chart of confidence over time for a selected zone (early version of README §5.4/Phase 7, and a natural place to prototype it before it needs to fit the Uno's history storage constraints)
- **Raw log tail** — last N raw lines from the Uno, unfiltered — genuinely useful for debugging live during the hackathon, and honestly also good demo texture ("here's the real serial traffic, nothing's faked")

## 6. Phased build plan

### Phase D1 — Uno-side protocol ✅ Done
`DASH:` result lines + `DASH_SCAN:` remote trigger, both over the existing USB serial link. Already in `uno_phase4_5_6.ino`.

**Acceptance test:** with the Uno connected and a terminal (e.g. `screen`/`minicom`/Arduino Serial Monitor) open on its port, typing `DASH_SCAN:B4` triggers a real scan and a `DASH:` line appears with the result.

### Phase D2 — Minimal backend + single-reading page
Python backend that opens the port, parses `DASH:` lines, and serves a page showing just the most recent reading (no grid, no history yet) plus a single "Scan Zone B4" test button.

**Acceptance test:** clicking the button on the page triggers a real scan on the hardware and the page updates with the result, without touching the Uno's Serial Monitor directly.

### Phase D3 — Zone grid + connection status
Add the 5×5 clickable grid and the connection/last-seen indicator.

**Acceptance test:** every zone is independently triggerable and independently colored by its own last result.

### Phase D4 — Per-zone history view
Add the history table/chart per zone, backed by the backend's in-memory per-zone list.

**Acceptance test:** scanning the same zone 3+ times shows all readings in order on the dashboard, matching what `[HISTORY]` already prints on the Uno's own serial log.

### Phase D5 — (Once the new display arrives) Keep both, dual-purpose
No dashboard changes needed — it already only depends on the Uno's serial output, which will keep flowing regardless of the physical screen's state. At that point it shifts from "the only way to use the system" to "the judge-facing second view," per README §0.2.

## 7. Explicitly out of scope for this doc

- Any change to the ESP32-S3-CAM firmware — this dashboard only talks to the Uno.
- Wi-Fi/networking of any kind — this is a wired, local, single-laptop tool, deliberately, so it doesn't depend on venue Wi-Fi during the demo.
- Any model training, inference, or decision logic — see §1's "what this is not."
