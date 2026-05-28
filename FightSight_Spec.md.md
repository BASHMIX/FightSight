# FGC Telemetry & Automation Engine: Master Technical Blueprint

## 1. Project Overview & Objective
You are tasked with building a high-performance, real-time Computer Vision Telemetry tool for Fighting Game Broadcasts (e.g., Street Fighter 6). The application reads a raw video feed via Spout2, analyzes specific user-defined Regions of Interest (ROIs) using OpenCV, and triggers real-time WebSocket events to "Streamer.bot" for broadcasting automation. 
**Crucial Constraint:** The engine must run at 60 FPS with near-zero latency. CPU/GPU efficiency is the highest priority.

---

## 2. Tech Stack & Dependencies
* **Language:** C++ (Standard C++17 or higher)
* **UI Framework:** Dear ImGui (with DirectX 11 backend)
* **Computer Vision:** OpenCV (cv::Mat, cv::matchTemplate, cv::cvtColor, HSV operations)
* **Video Capture:** Spout2 SDK (for receiving zero-latency texture sharing from OBS/Games)
* **WebSockets:** `websocketpp` or `uwebsockets` (for Streamer.bot integration)
* **Data Serialization:** `nlohmann/json` (for saving/loading `config.json` and ROI states)

---

## 3. Directory Structure
Ensure the project handles file I/O properly, specifically for captured templates.
```text
/Project_Root
 ├── /src                 # C++ Source Code
 ├── /include             # Headers
 ├── /lib                 # Dependencies (ImGui, OpenCV, Spout2)
 ├── /config
 │    └── config.json     # Saved ROIs, Triggers, and App Settings
 └── /templates           # CRITICAL: All captured ROI PNGs MUST be saved here.