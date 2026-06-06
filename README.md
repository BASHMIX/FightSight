# FightSight

A high-performance, real-time Computer Vision telemetry engine for Fighting Game broadcasts (e.g. Street Fighter 6). Reads a live video feed via **Spout2**, analyzes user-defined Regions of Interest (ROIs) using **OpenCV**, and fires **WebSocket events to Streamer.bot** for broadcast automation — all at 60 FPS with near-zero latency.

---

## Features

- **Zero-latency capture** — receives GPU textures directly from OBS/game via Spout2 (no screen capture overhead)
- **ROI-based detection** — define rectangular regions and choose a detection mode per region: `Text`, `Gradient`, or `Pixel`
- **Template matching** — captures PNG templates from live frames and matches them against subsequent frames using `cv::matchTemplate`
- **WebSocket triggers** — fires configurable events to Streamer.bot the moment a match threshold is crossed
- **Dear ImGui overlay** — live viewport with an interactive ROI editor; draw, resize, and rename regions without restarting
- **Persistent config** — all ROIs, thresholds, and connection settings auto-saved to `config/config.json`

---

## Tech Stack

| Component | Library |
|---|---|
| Language | C++17 |
| UI | Dear ImGui (DirectX 11 backend) |
| Computer Vision | OpenCV |
| Video Capture | Spout2 SDK |
| WebSockets | websocketpp |
| Serialization | nlohmann/json |

---

## Project Structure

```
FightSight/
├── src/                  # C++ source files
├── include/App/          # Headers
├── config/
│   └── config.json       # ROI definitions, thresholds, connection settings
├── templates/            # Captured ROI PNG templates (auto-populated at runtime)
└── build/                # CMake build output
```

---

## Building

**Requirements:** CMake 3.20+, Visual Studio 2022, DirectX 11 SDK

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output binary and required DLLs land in `build/Release/`.

---

## Configuration

Edit `config/config.json` or use the in-app ROI editor. Key fields:

```jsonc
{
  "cv": { "target_hz": 30 },          // CV pipeline target framerate
  "spout": { "receiver_name": "" },   // Spout2 source name (blank = auto-detect)
  "websocket": {
    "host": "127.0.0.1",
    "port": 8080,
    "endpoint": "/"
  },
  "viewport": { "force_1080p": true }
}
```

ROI types:
- **Text** — template match against a saved PNG crop
- **Gradient** — edge/gradient magnitude comparison
- **Pixel** — raw pixel color sampling

---

## Usage

1. Start your game or OBS with a Spout2 output enabled.
2. Launch `FightSight.exe`.
3. Select the Spout2 source in the UI.
4. Draw ROIs over the frame and capture templates as needed.
5. Set thresholds and Streamer.bot WebSocket details.
6. Click **Start** — events fire automatically as conditions are met.

---

## License

MIT
