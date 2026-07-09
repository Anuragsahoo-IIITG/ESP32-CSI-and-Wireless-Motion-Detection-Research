# WiFi-Based Activity Detection Using CSI Data

This repository contains the firmware and software stack for a device-free human activity detection system. It utilizes Channel State Information (CSI) from WiFi signals to detect motion in a room without requiring physical sensors like PIRs or cameras.

Developed as part of the EC382 Embedded System Laboratory.

## 🛠️ System Architecture

The system operates by analyzing how WiFi waves scatter and fade in an environment (multipath fading). It is divided into a hardware processing layer and a PC-based software visualization layer:

1. **Transmitter Node (ESP32):** Connects to a local WLAN and broadcasts UDP packets at 50Hz.
2. **Receiver Node (ESP32):** Operates in promiscuous mode to sniff the UDP packets. It extracts the raw CSI amplitude across 52 subcarriers and calculates a temporal variance metric over a sliding window.
3. **Backend Server (Python/Flask):** Reads the serial stream from the receiver, applies a baseline adaptation algorithm, and filters the data using a strict variance ratio threshold (≥ 1.8) and a 1.5-second debounce timer to eliminate false positives.
4. **Web Dashboard (HTML/JS):** A real-time UI built with Chart.js and Socket.IO that plots the motion metrics and logs motion events.

## 🚀 Hardware & Software Requirements

* 2x ESP32 DevKit v1 boards
* Mobile Hotspot (2.4GHz)
* Python 3.x
* `esp-idf` (for flashing the ESP32s)
* Python libraries: `pyserial`, `Flask`, `flask-socketio`

## ⚙️ Setup & Implementation

### 1. Flash the ESP32 Nodes
* **Transmitter:** Update the `WIFI_SSID` and `WIFI_PASS` in `csi_sender.c`. Flash this to the first ESP32. It will immediately begin broadcasting.
* **Receiver:** Update the credentials and the `SENDER_MAC` address in `csi_receiver_serial.c` to match your transmitter. Flash this to the second ESP32.

### 2. Calibration
Power on both devices. Ensure the environment between the two antennas is completely still. The receiver requires 60 packets in a quiet environment to calculate the baseline quiet variance. 

### 3. Run the Backend & Dashboard
1. Connect the Receiver ESP32 to your PC via USB.
2. Install the required Python dependencies:
   ```bash
   pip install pyserial flask flask-socketio
   ```
3. Run the backend server, specifying your COM port (e.g., COM4):
   ```bash
   python app.py COM4
   ```
4. Open a web browser and navigate to `http://localhost:5000`.

### 4. Testing
Walk through the Line-of-Sight between the two ESP32 nodes. The dashboard will instantly register the variance ratio spike, trigger the "MOTION" alert, and update the event log. You can also use the "Start Recording" button in the UI to log timestamps and ratios to a CSV file.

## 📄 Documentation

For a deep dive into the mathematical variance calculations, the debounce logic flowchart, and representative time-series graphs, please read the full [Project Report](Anurag_EC382_Project_CSI.pdf) .

## 👥 Authors
* Anurag Sahoo
* Anubhav Anand
