/*
  ================================================================================
  PowerSense v2.0 - Smart Energy Consumption Monitoring System
  ================================================================================
  Academic Project : 1st Year B.Tech (2025-26)
  Courses          : Innovation Design and Thinking (IDT) &
                     Engineering Exploration Course (EEC)
  Institution      : Dayananda Sagar Academy of Technology and Management (DSATM),
                     Bengaluru

  Project Team:
  - Harshit Agarwalla      (1DT25CS110) - Team Lead, Firmware & Cloud Telemetry
  - Dewanshu Dutt Thakur   (1DT25CS084) - Hardware Prototyping & Calibration
  - Gouravi Nayak          (1DT25CS104) - Data Analysis & Documentation
  - Ibbani S               (1DT25CS116) - Experimental Testing & Verification
  - Karnika Jain           (1DT25CS127) - Component Sourcing & Circuit Testing

  Faculty Mentors:
  - Project Guide          : Dr. Anjana Devi S C, Associate Professor, DSATM
  - Course Coordinator     : Dr. Roopa R Kulkarni, DSATM

  Hardware & Cloud Platform:
  - Microcontroller        : ESP32 DevKit V1 (Dual-Core, 2.4GHz Wi-Fi)
  - Voltage Sensor         : ZMPT101B Active AC Voltage Transformer (Mains-fixed)
  - Current Sensor         : SCT-013-030 Non-Invasive Split-Core Current Transformer
  - IoT Cloud Interface    : Blynk 2.0 (Live Mobile & Web Dashboard)
  - Cloud Database Logging : Google Sheets via Google Apps Script (Serverless HTTPS)
  - Benchmark Reference    : DT830D Digital Multimeter (Calibration benchmark)

  METROLOGICAL DESIGN PRINCIPLE
  --------------------------------------------------------------------------------
  The voltage sensor is permanently installed across the mains supply, while the
  current transformer clamp is physically moved between different appliances under
  test. Because voltage and current are not sampled from a singular branch with a
  guaranteed simultaneous phase angle in this multi-load experimental setup, this
  firmware rigorously computes and reports:

        Apparent Power (VA) = Vrms x Irms

  It does NOT compute, estimate, or fabricate active/real power, reactive power,
  or artificial power factors. Any power metrics referenced in this project
  strictly denote Apparent Power (VA).

  KEY ARCHITECTURAL FEATURES
  --------------------------------------------------------------------------------
  - Total Monitoring Time Denominator: Predicted monthly electricity bill uses
    total elapsed monitoring time (uptime since boot/reset) rather than active
    load runtime alone, providing an empirically realistic "observed usage" rate.
  - Dual-Stream EMA Filter: Exponential Moving Average (alpha = 0.2) is applied
    strictly to display/logging telemetry for UI gauge stability. Raw (unfiltered)
    samples are preserved for cumulative energy (kWh) integration and peak capture.
  - Load Utilization Metric: Ratio of active load runtime to total observation
    duration: Load Utilization (%) = (Runtime / Monitoring Time) x 100.
  - Fail-Safe Flash Persistence: Periodic state commits to ESP32 Preferences NVS
    every 60 seconds to safeguard cumulative kWh against power failure.
  ================================================================================
*/

// ============================================================================
// SECTION 1: LIBRARIES & BLYNK CREDENTIALS
// ============================================================================

// --- Blynk template identifiers (must precede Blynk headers) ---
#define BLYNK_TEMPLATE_ID "YOUR_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "PowerSense"
#define BLYNK_AUTH_TOKEN "YOUR_AUTH_TOKEN"
#define BLYNK_PRINT Serial                          // Enables Blynk debug output on Serial

#include <WiFi.h>
#include <HTTPClient.h>
#include <BlynkSimpleEsp32.h>
#include <Preferences.h>
#include <EmonLib.h>

// ============================================================================
// SECTION 2: NETWORK & CLOUD ENDPOINTS
// ============================================================================

char WIFI_SSID[] = "YOUR_WIFI_SSID";      
char WIFI_PASS[] = "YOUR_WIFI_PASSWORD";  

String experimentID = "UNASSIGNED";

// Google Apps Script Web App Deployment URL
// Deploy google-apps-script/Code.gs as a Web App (access: Anyone) and paste your URL below:
const char* GOOGLE_SCRIPT_URL =
  "https://script.google.com/macros/s/YOUR_GOOGLE_APPS_SCRIPT_WEB_APP_ID/exec";

const unsigned long GOOGLE_SHEETS_INTERVAL_MS = 15000; // Log to Google Sheets every 15s

unsigned long lastGoogleSheetsSend = 0;

// ============================================================================
// SECTION 3: HARDWARE PIN DEFINITIONS
// ============================================================================

const uint8_t PIN_VOLTAGE_SENSOR = 34;   // ZMPT101B analog output -> ADC1_CH6 (input-only, safe on ESP32)
const uint8_t PIN_CURRENT_SENSOR = 35;   // SCT-013-030 analog output -> ADC1_CH7 (input-only, safe on ESP32)

// ============================================================================
// SECTION 4: BLYNK VIRTUAL PIN MAP
// ============================================================================
// V0  - Voltage (V, EMA-smoothed for display)        [read-only, pushed by device]
// V1  - Current (A, EMA-smoothed for display)        [read-only, pushed by device]
// V2  - Apparent Power (VA, EMA-smoothed for display)[read-only, pushed by device]
// V3  - Energy (kWh, cumulative, RAW)                [read-only, pushed by device]
// V4  - Runtime (HH:MM:SS string, load-active time)  [read-only, pushed by device]
// V5  - Current Cost (INR)                           [read-only, pushed by device]
// V6  - Predicted Monthly Bill (INR, monitoring-time-based) [read-only, pushed by device]
// V7  - Alert Status (string, combined)              [read-only, pushed by device]
// V8  - Device Status (string)                       [read-only, pushed by device]
// V9  - Peak Power (VA, RAW)                         [read-only, pushed by device]
// V10 - Maximum Voltage (V, RAW)                     [read-only, pushed by device]
// V11 - Minimum Voltage (V, RAW)                      [read-only, pushed by device]
// V12 - Average Power (VA, RAW)                      [read-only, pushed by device]
// V13 - Sample Count                                 [read-only, pushed by device]
// V14 - Tariff Slider (INR/kWh)                       [read/write, BLYNK_WRITE]
// V15 - Reset Button                                  [write-only trigger, BLYNK_WRITE]
// V16 - Power Threshold Slider (VA)                   [read/write, BLYNK_WRITE]
// V17 - Monitoring Time (HH:MM:SS string, since boot/reset) [read-only, pushed by device]
// V18 - Load Utilization (%)                          [read-only, pushed by device]
// ============================================================================

// ============================================================================
// SECTION 5: CALIBRATION CONSTANTS
// ============================================================================
// These MUST be re-derived for your specific sensor units using the DT830D
// multimeter procedure described in the accompanying documentation.

// ZMPT101B: raw ADC RMS-of-centered-samples multiplied by this factor -> Volts
float voltageCalibrationFactor = 0.112f;   // <-- placeholder, calibrate before use

// SCT-013-030: EmonLib current calibration = (CT rated current) / (burden voltage at rated current)
// For a 30A/1V module with a suitable burden resistor, a commonly used starting
// value is ~30.0, but MUST be verified against the multimeter during calibration.
float currentCalibrationFactor = 1.24f;     // <-- placeholder, calibrate before use

// ESP32 ADC characteristics (12-bit, 0-4095). The ZMPT101B output is biased to
// roughly the midpoint of the ADC range by its onboard potentiometer/bias network.
const int   ADC_RESOLUTION_MAX = 4095;
const float ADC_MIDPOINT        = 2048.0f; // Nominal bias point, fine-tune if needed

// ============================================================================
// SECTION 6: TIMING & SAMPLING CONSTANTS
// ============================================================================

const unsigned long SAMPLE_INTERVAL_MS   = 5000;   // Main measurement cycle: every 5 seconds
const unsigned int  VOLTAGE_ADC_SAMPLES  = 2000;   // Samples averaged per voltage RMS calculation
const unsigned int  CURRENT_EMON_SAMPLES = 1480;   // Half-wave samples for EmonLib calcIrms()
const unsigned long PREFERENCES_SAVE_INTERVAL_MS = 60000; // Persist to flash at most once/minute

// EMA smoothing factor for DISPLAYED voltage/current/power only.
// Higher alpha = more responsive / less smoothing. Lower alpha = smoother / more lag.
// 0.2 is a reasonable starting point; tune after observing real data.
const float EMA_ALPHA = 0.2f;

// ============================================================================
// SECTION 7: THRESHOLDS & LIMITS
// ============================================================================

const float NO_LOAD_CURRENT_THRESHOLD_A = 0.03f;   // Below this, treated as "no load"
const float HIGH_VOLTAGE_THRESHOLD_V    = 250.0f;
const float LOW_VOLTAGE_THRESHOLD_V     = 210.0f;
float       powerThresholdHighVA        = 100.0f;  // Default; user-adjustable via V16

const float ENERGY_MILESTONE_STEP_KWH   = 0.1f;    // Notify every 0.1 kWh of cumulative energy
const float NOISE_FLOOR_CURRENT_A       = 0.005f;  // Currents below this are clamped to 0
const float NOISE_FLOOR_VOLTAGE_V       = 1.0f;    // Voltages below this are clamped to 0

// ============================================================================
// SECTION 8: DEFAULT CONFIGURABLE PARAMETERS
// ============================================================================

const float DEFAULT_TARIFF_INR_PER_KWH = 6.5f;

// ============================================================================
// SECTION 9: GLOBAL OBJECTS
// ============================================================================

EnergyMonitor emon1;          // EmonLib instance, used here strictly for current RMS
Preferences   preferences;    // Non-volatile storage handle
const char*   PREF_NAMESPACE = "powersense";

// ============================================================================
// SECTION 10: STATE STRUCTURES
// ============================================================================

// --- Live, per-cycle RAW measurement values (used for energy, stats, alerts) ---
struct LiveReadings {
  float voltageV        = 0.0f;
  float currentA        = 0.0f;
  float apparentPowerVA = 0.0f;
};
LiveReadings live;

// --- EMA-smoothed values, used ONLY for dashboard/Serial/CSV display ---
struct EmaReadings {
  float voltageV        = 0.0f;
  float currentA        = 0.0f;
  float apparentPowerVA = 0.0f;
  bool  initialized      = false;
};
EmaReadings ema;

// --- Cumulative / persisted values ---
struct PersistedState {
  float energyKWh                 = 0.0f;
  unsigned long runtimeSeconds     = 0;   // Time load current exceeded no-load threshold
  unsigned long monitoringTimeSeconds = 0; // Total elapsed time since boot / last full reset
  float tariffInrPerKWh            = DEFAULT_TARIFF_INR_PER_KWH;
  float powerThresholdVA           = 100.0f;
};
PersistedState persisted;

// --- Session statistics (resettable, computed from RAW readings) ---
struct SessionStats {
  float peakPowerVA     = 0.0f;
  float maxVoltageV     = 0.0f;
  float minVoltageV     = 99999.0f; // seeded high so first real reading replaces it
  double powerSumVA     = 0.0;      // running sum, used to compute average
  unsigned long sampleCount = 0;
};
SessionStats stats;

// --- Alert / notification state, used to avoid notification spam ---
struct AlertState {
  bool highVoltageActive = false;
  bool lowVoltageActive  = false;
  bool highPowerActive   = false;
  bool noLoadActive       = false;
  float lastMilestoneNotifiedKWh = 0.0f;
  String currentAlertText = "Normal";
};
AlertState alerts;

// --- Timing bookkeeping ---
unsigned long lastSampleMillis      = 0;
unsigned long lastPreferencesSaveMillis = 0;

// --- Serial output mode ---
// false = human-readable block format, true = single-line CSV format
bool csvLoggingEnabled = false;

// ============================================================================
// SECTION 11: FUNCTION PROTOTYPES
// ============================================================================

void connectWiFiAndBlynk();
void loadPreferences();
void savePreferences();
void resetStatistics(bool alsoResetEnergyRuntimeAndMonitoring);

float readVoltage();
float readCurrent();
void  updateStatistics(float voltage, float current, float apparentPower);
void  updateEma(float rawVoltage, float rawCurrent, float rawApparentPower);
void  calculateCost(float &currentCostOut, float &predictedMonthlyBillOut);
float calculateLoadUtilizationPercent();

void  checkAlerts();
void  sendNotifications();

void  sendData();
void  printHumanReadable(float currentCost, float predictedBill, float utilizationPercent);
void  printCsvRow(float currentCost, float predictedBill, float utilizationPercent);
String formatRuntime(unsigned long totalSeconds);

void handleSerialCommands();

// ============================================================================
// SECTION 12: BLYNK WRITE CALLBACKS
// ============================================================================

// V14 - Tariff slider: user adjusts electricity rate (INR per kWh)
BLYNK_WRITE(V14) {
  float newTariff = param.asFloat();
  if (newTariff > 0.0f && newTariff < 1000.0f) {
    persisted.tariffInrPerKWh = newTariff;
    savePreferences();
    Serial.print(F("[CONFIG] Tariff updated to Rs "));
    Serial.print(persisted.tariffInrPerKWh);
    Serial.println(F(" / kWh"));
  }
}

// V15 - Reset button: clears session statistics, energy, runtime, and monitoring time
BLYNK_WRITE(V15) {
  int buttonState = param.asInt();
  if (buttonState == 1) {
    resetStatistics(true);
    Serial.println(F("[ACTION] Full reset triggered from Blynk dashboard."));
  }
}

// V16 - Power threshold slider: user adjusts the "high power" alert level (VA)
BLYNK_WRITE(V16) {
  float newThreshold = param.asFloat();
  if (newThreshold > 0.0f && newThreshold < 10000.0f) {
    persisted.powerThresholdVA = newThreshold;
    powerThresholdHighVA = newThreshold;
    savePreferences();
    Serial.print(F("[CONFIG] Power threshold updated to "));
    Serial.print(powerThresholdHighVA);
    Serial.println(F(" VA"));
  }
}

BLYNK_WRITE(V18) {
  experimentID = param.asString();

  Serial.print("[CONFIG] Experiment ID: ");
  Serial.println(experimentID);
}

// Fires once when the device (re)connects to the Blynk server.
// Used to push current slider/config values back to the dashboard so the UI
// reflects the device's persisted state after a reboot.
BLYNK_CONNECTED() {
  Blynk.virtualWrite(V14, persisted.tariffInrPerKWh);
  Blynk.virtualWrite(V16, persisted.powerThresholdVA);
  Blynk.virtualWrite(V8, "Online");
}
// ============================================================================
// GOOGLE SHEETS DATA TRANSMISSION
// ============================================================================

String urlEncode(const String &text) {
  String encoded = "";

  const char *hex = "0123456789ABCDEF";

  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);

    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {

      encoded += c;

    } else {

      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }

  return encoded;
}


void sendDataToGoogleSheets(float currentCost,
                            float predictedBill,
                            float utilizationPercent) {

  // Do not attempt transmission without Wi-Fi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[SHEETS] WiFi not connected - skipping upload."));
    return;
  }

  HTTPClient http;

  String url = String(GOOGLE_SCRIPT_URL);

  // --------------------------------------------------------------------------
  // EXPERIMENT ID
  // --------------------------------------------------------------------------

  url += "?experimentID=";
  url += urlEncode(experimentID);

  // --------------------------------------------------------------------------
  // MEASUREMENT DATA
  // --------------------------------------------------------------------------

  url += "&timestamp=";
  url += String(persisted.monitoringTimeSeconds);

  url += "&voltage=";
  url += String(ema.voltageV, 1);

  url += "&current=";
  url += String(ema.currentA, 3);

  url += "&power=";
  url += String(ema.apparentPowerVA, 1);

  url += "&energy=";
  url += String(persisted.energyKWh, 5);

  url += "&runtime=";
  url += urlEncode(formatRuntime(persisted.runtimeSeconds));

  url += "&cost=";
  url += String(currentCost, 2);

  url += "&predictedBill=";
  url += String(predictedBill, 1);

  url += "&peakPower=";
  url += String(stats.peakPowerVA, 1);

  url += "&averagePower=";
  url += String(getAveragePowerVA(), 1);

  url += "&maxVoltage=";
  url += String(stats.maxVoltageV, 1);

  float minVoltageDisplay =
      (stats.minVoltageV > 90000.0f)
      ? 0.0f
      : stats.minVoltageV;

  url += "&minVoltage=";
  url += String(minVoltageDisplay, 1);

  url += "&sampleCount=";
  url += String(stats.sampleCount);

  url += "&monitoringTime=";
  url += urlEncode(
      formatRuntime(persisted.monitoringTimeSeconds)
  );

  url += "&loadUtilization=";
  url += String(utilizationPercent, 1);

  url += "&alert=";
  url += urlEncode(alerts.currentAlertText);

  // --------------------------------------------------------------------------
  // HTTPS REQUEST
  // --------------------------------------------------------------------------

  // Google Apps Script normally redirects the /exec URL.
  // Follow that redirect automatically.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  http.begin(url);

  int httpResponseCode = http.GET();

  if (httpResponseCode > 0) {

    Serial.print(F("[SHEETS] Response code: "));
    Serial.println(httpResponseCode);

    String response = http.getString();

    Serial.print(F("[SHEETS] Response: "));
    Serial.println(response);

  } else {

    Serial.print(F("[SHEETS] Upload failed. Error: "));
    Serial.println(http.errorToString(httpResponseCode));
  }

  http.end();
}

// ============================================================================
// SECTION 13: SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== PowerSense v2.0 booting ==="));

  // --- Configure ADC ---
  analogReadResolution(12);          // 0-4095
  analogSetAttenuation(ADC_11db);    // Allows reading up to ~3.3V range on ESP32 ADC pins

  // --- Load persisted state before anything else uses it ---
  loadPreferences();
  powerThresholdHighVA = persisted.powerThresholdVA;

  // --- Configure EmonLib for current sensing only ---
  // emon1.current(pin, calibration) configures the CT input; voltage is handled
  // manually (see readVoltage()) since the ZMPT101B is on a separate, fixed branch.
  emon1.current(PIN_CURRENT_SENSOR, currentCalibrationFactor);

  // --- Connect to WiFi and Blynk ---
  connectWiFiAndBlynk();

  // --- Initialize timing baselines ---
  lastSampleMillis  = millis();
  lastPreferencesSaveMillis = millis();

  Serial.println(F("=== Setup complete. Entering main loop. ==="));
  Serial.println(F("Type 'c' + Enter in Serial Monitor to toggle CSV logging mode."));
}

// ============================================================================
// SECTION 14: MAIN LOOP
// ============================================================================

void loop() {
  Blynk.run();
  handleSerialCommands();

  unsigned long now = millis();

  // --- Main measurement cycle, runs every SAMPLE_INTERVAL_MS ---
  if (now - lastSampleMillis >= SAMPLE_INTERVAL_MS) {
    unsigned long deltaMillis = now - lastSampleMillis;
    lastSampleMillis = now;

    // 1. Acquire fresh RAW readings
    live.voltageV = readVoltage();
    live.currentA = readCurrent();
    live.apparentPowerVA = live.voltageV * live.currentA;

    // 2. Update EMA-smoothed values (display/logging only - does not touch
    //    energy accumulation, stats, or alert detection below)
    updateEma(live.voltageV, live.currentA, live.apparentPowerVA);

    // 3. Advance total monitoring time (elapsed clock time, regardless of load)
    unsigned long deltaSeconds = deltaMillis / 1000UL;
    persisted.monitoringTimeSeconds += deltaSeconds;

    // 4. Advance load-active runtime only while a real load is present
    if (live.currentA > NO_LOAD_CURRENT_THRESHOLD_A) {
      persisted.runtimeSeconds += deltaSeconds;
    }

    // 5. Update energy accumulation (kWh) using this cycle's RAW average power
    //    Energy(kWh) += Power(VA) * time(h) / 1000
    float hoursElapsed = (float)deltaMillis / 3600000.0f;
    persisted.energyKWh += (live.apparentPowerVA * hoursElapsed) / 1000.0f;

    // 6. Update running statistics (RAW-based, so true peaks/extremes are preserved)
    updateStatistics(live.voltageV, live.currentA, live.apparentPowerVA);

    // 7. Compute cost figures (monitoring-time-based prediction)
    float currentCost = 0.0f, predictedBill = 0.0f;
    calculateCost(currentCost, predictedBill);

    // 8. Compute load utilization
    float utilizationPercent = calculateLoadUtilizationPercent();

    // 9. Evaluate alert conditions (RAW-based, for accurate/fast detection)
    checkAlerts();

    // 10. Fire any pending notifications (state-change based, cooldown-protected)
    sendNotifications();

    // 11. Push everything to Blynk
    sendData();

// Send research data to Google Sheets every 15 seconds
if (now - lastGoogleSheetsSend >= GOOGLE_SHEETS_INTERVAL_MS) {

  sendDataToGoogleSheets(
    currentCost,
    predictedBill,
    utilizationPercent
  );

  lastGoogleSheetsSend = now;
}

    // 12. Log to Serial in the active mode
    if (csvLoggingEnabled) {
      printCsvRow(currentCost, predictedBill, utilizationPercent);
    } else {
      printHumanReadable(currentCost, predictedBill, utilizationPercent);
    }
  }

  // --- Periodic flash save, independent of the 5s measurement cycle ---
  // (Unchanged: once per minute is adequate for research-length experiments.)
  if (now - lastPreferencesSaveMillis >= PREFERENCES_SAVE_INTERVAL_MS) {
    lastPreferencesSaveMillis = now;
    savePreferences();
  }
}

// ============================================================================
// SECTION 15: SENSOR ACQUISITION
// ============================================================================

// Computes true RMS voltage from the ZMPT101B by sampling the AC waveform,
// removing the DC bias, and taking the root-mean-square of the centered samples.
// This is done manually (rather than via EmonLib's combined calcVI) because the
// voltage branch is physically independent from the current branch in this setup.
float readVoltage() {
  double sumOfSquares = 0.0;

  for (unsigned int i = 0; i < VOLTAGE_ADC_SAMPLES; i++) {
    int raw = analogRead(PIN_VOLTAGE_SENSOR);
    float centered = (float)raw - ADC_MIDPOINT;
    sumOfSquares += (double)(centered * centered);
    delayMicroseconds(150); // Spreads samples across multiple mains cycles
  }

  float meanSquare = (float)(sumOfSquares / VOLTAGE_ADC_SAMPLES);
  float rawRms = sqrtf(meanSquare);
  float voltage = rawRms * voltageCalibrationFactor;

  if (voltage < NOISE_FLOOR_VOLTAGE_V) {
    voltage = 0.0f;
  }
  return voltage;
}

// Computes true RMS current using EmonLib's calcIrms(), which internally samples
// the SCT-013-030 output, removes the DC bias, and applies the configured
// current calibration factor.
float readCurrent() {
  double irms = emon1.calcIrms(CURRENT_EMON_SAMPLES);

  if (irms < NOISE_FLOOR_CURRENT_A) {
    irms = 0.0;
  }
  return (float)irms;
}
// ============================================================================
// SECTION 16: EMA FILTERING (DISPLAY/LOGGING ONLY)
// ============================================================================

// Applies exponential moving average smoothing to voltage, current, and
// apparent power for dashboard/Serial/CSV display purposes only.
// Formula: ema = alpha * raw + (1 - alpha) * ema_previous
// On the very first call, the EMA is seeded directly with the raw reading so
// there is no artificial ramp-up from zero.
void updateEma(float rawVoltage, float rawCurrent, float rawApparentPower) {
  if (!ema.initialized) {
    ema.voltageV        = rawVoltage;
    ema.currentA         = rawCurrent;
    ema.apparentPowerVA  = rawApparentPower;
    ema.initialized       = true;
    return;
  }

  ema.voltageV        = (EMA_ALPHA * rawVoltage)       + ((1.0f - EMA_ALPHA) * ema.voltageV);
  ema.currentA         = (EMA_ALPHA * rawCurrent)        + ((1.0f - EMA_ALPHA) * ema.currentA);
  ema.apparentPowerVA  = (EMA_ALPHA * rawApparentPower)  + ((1.0f - EMA_ALPHA) * ema.apparentPowerVA);
}

// ============================================================================
// SECTION 17: STATISTICS
// ============================================================================

void updateStatistics(float voltage, float current, float apparentPower) {
  // Peak power (RAW - so a genuine transient spike is never smoothed away)
  if (apparentPower > stats.peakPowerVA) {
    stats.peakPowerVA = apparentPower;
  }

  // Average power (running mean via sum/count, RAW)
  stats.powerSumVA += apparentPower;
  stats.sampleCount++;

  // Voltage extremes - only tracked when a plausible mains reading is present,
  // to avoid a "0V" reset from a temporarily disconnected sensor corrupting the
  // minimum voltage statistic.
  if (voltage > NOISE_FLOOR_VOLTAGE_V) {
    if (voltage > stats.maxVoltageV) stats.maxVoltageV = voltage;
    if (voltage < stats.minVoltageV) stats.minVoltageV = voltage;
  }
}

float getAveragePowerVA() {
  if (stats.sampleCount == 0) return 0.0f;
  return (float)(stats.powerSumVA / (double)stats.sampleCount);
}

// ============================================================================
// SECTION 18: COST CALCULATION
// ============================================================================

void calculateCost(float &currentCostOut, float &predictedMonthlyBillOut) {
  // Current cost = cumulative measured energy x tariff
  currentCostOut = persisted.energyKWh * persisted.tariffInrPerKWh;

  // Predicted monthly bill: extrapolate the measured energy rate using TOTAL
  // MONITORING TIME (elapsed clock time since boot/reset) as the denominator,
  // NOT load-active runtime. This reflects the appliance's actual observed
  // usage pattern (including idle periods) rather than assuming continuous
  // full-load operation, and is the statistically defensible basis for the
  // IEEE paper's bill projections.
  float monitoringHours = (float)persisted.monitoringTimeSeconds / 3600.0f;
  if (monitoringHours > 0.01f) {
    float avgKWhPerHour = persisted.energyKWh / monitoringHours;
    predictedMonthlyBillOut = avgKWhPerHour * 24.0f * 30.0f * persisted.tariffInrPerKWh;
  } else {
    predictedMonthlyBillOut = 0.0f;
  }
}

// ============================================================================
// SECTION 19: LOAD UTILIZATION
// ============================================================================

// Load Utilization (%) = (Runtime / Monitoring Time) x 100
// Indicates what fraction of the observed period the appliance was actually
// drawing current above the no-load threshold. Useful for comparing usage
// patterns across different appliance types in the Results & Discussion section.
float calculateLoadUtilizationPercent() {
  if (persisted.monitoringTimeSeconds == 0) return 0.0f;
  float utilization = ((float)persisted.runtimeSeconds / (float)persisted.monitoringTimeSeconds) * 100.0f;
  if (utilization > 100.0f) utilization = 100.0f; // Safety clamp
  return utilization;
}

// ============================================================================
// SECTION 20: ALERTS
// ============================================================================

void checkAlerts() {
  alerts.highVoltageActive = (live.voltageV > HIGH_VOLTAGE_THRESHOLD_V);

  // Low voltage is only meaningful when a real reading is present (not a
  // disconnected/zero sensor reading).
  alerts.lowVoltageActive = (live.voltageV > NOISE_FLOOR_VOLTAGE_V &&
                              live.voltageV < LOW_VOLTAGE_THRESHOLD_V);

  alerts.highPowerActive = (live.apparentPowerVA > powerThresholdHighVA);

  alerts.noLoadActive = (live.currentA <= NO_LOAD_CURRENT_THRESHOLD_A);

  // Build a combined alert string so multiple simultaneous conditions are all
  // visible on the dashboard/CSV instead of only the first one detected.
  String alertText = "";

  if (alerts.highVoltageActive) alertText += "High Voltage; ";
  if (alerts.lowVoltageActive)  alertText += "Low Voltage; ";
  if (alerts.highPowerActive)   alertText += "High Power; ";
  if (alerts.noLoadActive)      alertText += "No Load; ";

  if (alertText.length() == 0) {
    alertText = "Normal";
  } else {
    // Trim the trailing "; "
    alertText.remove(alertText.length() - 2);
  }

  alerts.currentAlertText = alertText;
}

// ============================================================================
// SECTION 21: NOTIFICATIONS
// ============================================================================
// Blynk.logEvent() calls are naturally state-driven here: because checkAlerts()
// re-evaluates each boolean every cycle, and notifications only fire on a
// false->true transition (tracked via static "previous state" locals below),
// there is no repeated spamming for a condition that remains continuously true.

void sendNotifications() {
  static bool prevHighVoltage = false;
  static bool prevLowVoltage  = false;
  static bool prevHighPower   = false;

  if (alerts.highVoltageActive && !prevHighVoltage) {
    Blynk.logEvent("high_voltage", "High voltage detected: " + String(live.voltageV, 1) + " V");
  }
  if (alerts.lowVoltageActive && !prevLowVoltage) {
    Blynk.logEvent("low_voltage", "Low voltage detected: " + String(live.voltageV, 1) + " V");
  }
  if (alerts.highPowerActive && !prevHighPower) {
    Blynk.logEvent("high_power", "High power draw detected: " + String(live.apparentPowerVA, 1) + " VA");
  }

  prevHighVoltage = alerts.highVoltageActive;
  prevLowVoltage  = alerts.lowVoltageActive;
  prevHighPower   = alerts.highPowerActive;

  // Energy milestone notification - fires every ENERGY_MILESTONE_STEP_KWH of
  // cumulative energy, regardless of load state.
  if (persisted.energyKWh - alerts.lastMilestoneNotifiedKWh >= ENERGY_MILESTONE_STEP_KWH) {
    alerts.lastMilestoneNotifiedKWh = persisted.energyKWh;
    Blynk.logEvent("energy_milestone",
                   "Energy milestone reached: " + String(persisted.energyKWh, 3) + " kWh");
  }
}

// ============================================================================
// SECTION 22: BLYNK DATA TRANSMISSION
// ============================================================================

void sendData() {
  float currentCost = 0.0f, predictedBill = 0.0f;
  calculateCost(currentCost, predictedBill);
  float utilizationPercent = calculateLoadUtilizationPercent();

  // Instantaneous readings pushed as EMA-smoothed values for cleaner graphs.
  Blynk.virtualWrite(V0, ema.voltageV);
  Blynk.virtualWrite(V1, ema.currentA);
  Blynk.virtualWrite(V2, ema.apparentPowerVA);

  // Cumulative/derived values remain RAW-based (unaffected by smoothing).
  Blynk.virtualWrite(V3, persisted.energyKWh);
  Blynk.virtualWrite(V4, formatRuntime(persisted.runtimeSeconds));
  Blynk.virtualWrite(V5, currentCost);
  Blynk.virtualWrite(V6, predictedBill);
  Blynk.virtualWrite(V7, alerts.currentAlertText);
  Blynk.virtualWrite(V8, "Online");
  Blynk.virtualWrite(V9, stats.peakPowerVA);
  Blynk.virtualWrite(V10, stats.maxVoltageV);
  Blynk.virtualWrite(V11, (stats.minVoltageV > 90000.0f) ? 0.0f : stats.minVoltageV);
  Blynk.virtualWrite(V12, getAveragePowerVA());
  Blynk.virtualWrite(V13, (int)stats.sampleCount);
  Blynk.virtualWrite(V17, utilizationPercent);
}

// ============================================================================
// SECTION 23: SERIAL OUTPUT
// ============================================================================

void printHumanReadable(float currentCost, float predictedBill, float utilizationPercent) {
  Serial.println(F("----------------------------------------"));
  Serial.print(F("Voltage (EMA)      : ")); Serial.print(ema.voltageV, 1);        Serial.println(F(" V"));
  Serial.print(F("Current (EMA)      : ")); Serial.print(ema.currentA, 3);        Serial.println(F(" A"));
  Serial.print(F("Power (EMA, App.)  : ")); Serial.print(ema.apparentPowerVA, 1); Serial.println(F(" VA"));
  Serial.print(F("Energy             : ")); Serial.print(persisted.energyKWh, 5);  Serial.println(F(" kWh"));
  Serial.print(F("Runtime            : ")); Serial.println(formatRuntime(persisted.runtimeSeconds));
  Serial.print(F("Monitoring Time    : ")); Serial.println(formatRuntime(persisted.monitoringTimeSeconds));
  Serial.print(F("Load Utilization   : ")); Serial.print(utilizationPercent, 1); Serial.println(F(" %"));
  Serial.print(F("Current Cost       : Rs ")); Serial.println(currentCost, 2);
  Serial.print(F("Predicted Monthly Bill : Rs ")); Serial.println(predictedBill, 1);
  Serial.print(F("Peak Power         : ")); Serial.print(stats.peakPowerVA, 1);    Serial.println(F(" VA"));
  Serial.print(F("Average Power      : ")); Serial.print(getAveragePowerVA(), 1);  Serial.println(F(" VA"));
  Serial.print(F("Max Voltage        : ")); Serial.print(stats.maxVoltageV, 1);    Serial.println(F(" V"));
  Serial.print(F("Min Voltage        : ")); Serial.print((stats.minVoltageV > 90000.0f) ? 0.0f : stats.minVoltageV, 1); Serial.println(F(" V"));
  Serial.print(F("Alert              : ")); Serial.println(alerts.currentAlertText);
  Serial.println(F("----------------------------------------"));
}

void printCsvRow(float currentCost, float predictedBill, float utilizationPercent) {
  static bool headerPrinted = false;
  if (!headerPrinted) {
    Serial.println(F("Timestamp,Voltage,Current,ApparentPower,Energy,Runtime,CurrentCost,PredictedBill,PeakPower,AveragePower,MaxVoltage,MinVoltage,SampleCount,MonitoringTime,LoadUtilization,AlertStatus"));
    headerPrinted = true;
  }

  float minVoltageDisplay = (stats.minVoltageV > 90000.0f) ? 0.0f : stats.minVoltageV;

  Serial.print(persisted.monitoringTimeSeconds);       Serial.print(','); // Timestamp = elapsed seconds since boot/reset
  Serial.print(ema.voltageV, 1);                       Serial.print(',');
  Serial.print(ema.currentA, 3);                       Serial.print(',');
  Serial.print(ema.apparentPowerVA, 1);                Serial.print(',');
  Serial.print(persisted.energyKWh, 5);                Serial.print(',');
  Serial.print(formatRuntime(persisted.runtimeSeconds)); Serial.print(',');
  Serial.print(currentCost, 2);                        Serial.print(',');
  Serial.print(predictedBill, 1);                      Serial.print(',');
  Serial.print(stats.peakPowerVA, 1);                  Serial.print(',');
  Serial.print(getAveragePowerVA(), 1);                Serial.print(',');
  Serial.print(stats.maxVoltageV, 1);                  Serial.print(',');
  Serial.print(minVoltageDisplay, 1);                  Serial.print(',');
  Serial.print(stats.sampleCount);                     Serial.print(',');
  Serial.print(formatRuntime(persisted.monitoringTimeSeconds)); Serial.print(',');
  Serial.print(utilizationPercent, 1);                 Serial.print(',');
  Serial.println(alerts.currentAlertText);
}

// Reads single-character commands from Serial to toggle behavior at runtime,
// without requiring a firmware re-flash.
void handleSerialCommands() {
  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c == 'c' || c == 'C') {
      csvLoggingEnabled = !csvLoggingEnabled;
      Serial.print(F("[CONFIG] CSV logging mode: "));
      Serial.println(csvLoggingEnabled ? F("ENABLED") : F("DISABLED"));
    }
  }
}

// ============================================================================
// SECTION 24: FORMATTING HELPERS
// ============================================================================

String formatRuntime(unsigned long totalSeconds) {
  unsigned long hours   = totalSeconds / 3600UL;
  unsigned long minutes = (totalSeconds % 3600UL) / 60UL;
  unsigned long seconds = totalSeconds % 60UL;

  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(buffer);
}

// ============================================================================
// SECTION 25: PREFERENCES (NON-VOLATILE STORAGE)
// ============================================================================

void loadPreferences() {
  preferences.begin(PREF_NAMESPACE, false); // read/write mode
  persisted.energyKWh             = preferences.getFloat("energy", 0.0f);
  persisted.runtimeSeconds        = preferences.getULong("runtime", 0UL);
  persisted.monitoringTimeSeconds = preferences.getULong("monTime", 0UL);
  persisted.tariffInrPerKWh       = preferences.getFloat("tariff", DEFAULT_TARIFF_INR_PER_KWH);
  persisted.powerThresholdVA      = preferences.getFloat("pThresh", 100.0f);
  preferences.end();

  Serial.println(F("[STORAGE] Preferences loaded:"));
  Serial.print(F("  Energy          : ")); Serial.print(persisted.energyKWh, 5); Serial.println(F(" kWh"));
  Serial.print(F("  Runtime         : ")); Serial.println(formatRuntime(persisted.runtimeSeconds));
  Serial.print(F("  Monitoring Time : ")); Serial.println(formatRuntime(persisted.monitoringTimeSeconds));
  Serial.print(F("  Tariff          : Rs ")); Serial.println(persisted.tariffInrPerKWh, 2);
  Serial.print(F("  Threshold       : ")); Serial.print(persisted.powerThresholdVA, 1); Serial.println(F(" VA"));
}

void savePreferences() {
  preferences.begin(PREF_NAMESPACE, false);
  preferences.putFloat("energy", persisted.energyKWh);
  preferences.putULong("runtime", persisted.runtimeSeconds);
  preferences.putULong("monTime", persisted.monitoringTimeSeconds);
  preferences.putFloat("tariff", persisted.tariffInrPerKWh);
  preferences.putFloat("pThresh", persisted.powerThresholdVA);
  preferences.end();
}

// Clears session statistics. If alsoResetEnergyRuntimeAndMonitoring is true,
// cumulative energy, runtime, AND total monitoring time (and their flash-stored
// copies) are cleared as well - this is the behavior triggered by the V15
// dashboard Reset button, effectively starting a brand-new experiment.
void resetStatistics(bool alsoResetEnergyRuntimeAndMonitoring) {
  stats.peakPowerVA  = 0.0f;
  stats.maxVoltageV  = 0.0f;
  stats.minVoltageV  = 99999.0f;
  stats.powerSumVA   = 0.0;
  stats.sampleCount  = 0;

  alerts.lastMilestoneNotifiedKWh = 0.0f;

  if (alsoResetEnergyRuntimeAndMonitoring) {
    persisted.energyKWh             = 0.0f;
    persisted.runtimeSeconds        = 0UL;
    persisted.monitoringTimeSeconds = 0UL;
    savePreferences();
  }
}

// ============================================================================
// SECTION 26: CONNECTIVITY
// ============================================================================
void connectWiFiAndBlynk() {
  Serial.print(F("[WIFI] Connecting to "));
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print(F("[WIFI] Connecting"));

  unsigned long wifiStart = millis();

  while (WiFi.status() != WL_CONNECTED &&
         (millis() - wifiStart < 15000)) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println(F("[WIFI] WiFi connected!"));
    Serial.print(F("[WIFI] IP address: "));
    Serial.println(WiFi.localIP());

    Serial.println(F("[BLYNK] Connecting..."));

    Blynk.config(BLYNK_AUTH_TOKEN);

    if (Blynk.connect(10000)) {
      Serial.println(F("[BLYNK] Connected successfully!"));
    } else {
      Serial.println(F("[BLYNK] Connection failed."));
    }

  } else {

    Serial.println(F("[WIFI] WiFi connection FAILED."));
  }
}
