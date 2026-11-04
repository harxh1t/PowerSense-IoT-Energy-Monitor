# Google Sheets Cloud Telemetry Integration

This module logs real-time experimental data from the ESP32 microcontroller into a Google Spreadsheet using Google Apps Script.

## 🚀 Setup Instructions

1. **Create a new Google Sheet**:
   - Go to [Google Sheets](https://sheets.new) and create a blank spreadsheet.
   - Name it `PowerSense_Telemetry_Log`.

2. **Open Apps Script Editor**:
   - Click **Extensions** > **Apps Script**.
   - Delete any default code in `Code.gs` and paste the contents of [`Code.gs`](Code.gs).

3. **Deploy as a Web App**:
   - Click the blue **Deploy** button (top right) > **New deployment**.
   - Click the gear icon next to "Select type" and choose **Web app**.
   - Configure:
     - **Description**: `PowerSense IoT Logger`
     - **Execute as**: `Me (your email)`
     - **Who has access**: `Anyone` *(Note: This allows ESP32 HTTP client to submit telemetry without OAuth2 token expiration).*
   - Click **Deploy** and grant required permissions.
   - Copy the generated **Web App URL** (e.g., `https://script.google.com/macros/s/AKfy.../exec`).

4. **Update ESP32 Firmware**:
   - In `PowerSense_ESP32.ino`, set:
     ```cpp
     const char* GOOGLE_SCRIPT_URL = "YOUR_WEB_APP_URL_HERE";
     ```
   - Flash your ESP32. The microcontroller will stream experimental data every 15 seconds.

## 📊 Recorded Fields

| Column Header | Description |
| :--- | :--- |
| **Server Timestamp** | Google Server timestamp when record was received |
| **Experiment ID** | Identifier tag for tested appliance (e.g., `Bulb_13W`, `Laptop_Charger`) |
| **Elapsed Seconds** | Continuous running counter (seconds) |
| **Voltage (V)** | EMA-filtered AC RMS Voltage (V) |
| **Current (A)** | EMA-filtered AC RMS Current (A) |
| **Apparent Power (VA)** | EMA-filtered Apparent Power ($S = V_{rms} \times I_{rms}$) |
| **Cumulative Energy (kWh)** | Total energy consumed in kilowatt-hours |
| **Runtime (Active)** | Load active duration (`HH:MM:SS`) |
| **Current Cost (INR)** | Real-time accumulated electricity cost |
| **Predicted Monthly Bill (INR)** | Projected monthly bill extrapolated from total monitoring duration |
| **Peak Power (VA)** | Maximum apparent power transient observed |
| **Average Power (VA)** | Mathematical mean power |
| **Max / Min Voltage (V)** | Voltage fluctuations and grid excursions |
| **Sample Count** | Total sampling cycles completed |
| **Total Monitoring Time** | Total time elapsed since device boot/reset |
| **Load Utilization (%)** | Ratio of active load runtime to total observation period |
| **Alert Status** | Active anomalies (Overvoltage, Undervoltage, Overpower, No Load) |
