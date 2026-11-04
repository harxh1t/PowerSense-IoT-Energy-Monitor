/**
 * Google Apps Script for PowerSense IoT Energy Monitor
 * Receives HTTP GET requests from ESP32 and logs records to Google Sheets.
 *
 * Setup:
 * 1. Open Google Sheets (https://sheets.new)
 * 2. Extensions > Apps Script
 * 3. Paste this code into Code.gs
 * 4. Deploy > New Deployment > Web app:
 *    - Execute as: Me
 *    - Who has access: Anyone (even anonymous)
 * 5. Copy the Web App URL and paste into your ESP32 firmware (GOOGLE_SCRIPT_URL).
 */

function doGet(e) {
  try {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
    
    // Auto-create headers if sheet is empty
    if (sheet.getLastRow() === 0) {
      sheet.appendRow([
        "Server Timestamp",
        "Experiment ID",
        "Elapsed Seconds",
        "Voltage (V)",
        "Current (A)",
        "Apparent Power (VA)",
        "Cumulative Energy (kWh)",
        "Runtime (Active)",
        "Current Cost (INR)",
        "Predicted Monthly Bill (INR)",
        "Peak Power (VA)",
        "Average Power (VA)",
        "Max Voltage (V)",
        "Min Voltage (V)",
        "Sample Count",
        "Total Monitoring Time",
        "Load Utilization (%)",
        "Alert Status"
      ]);
      // Format header row bold with background color
      sheet.getRange(1, 1, 1, 18).setFontWeight("bold").setBackground("#E8EEF5");
      sheet.setFrozenRows(1);
    }
    
    var params = e.parameter;
    
    var row = [
      new Date(),
      params.experimentID || "UNASSIGNED",
      params.timestamp || 0,
      params.voltage || 0,
      params.current || 0,
      params.power || 0,
      params.energy || 0,
      params.runtime || "00:00:00",
      params.cost || 0,
      params.predictedBill || 0,
      params.peakPower || 0,
      params.averagePower || 0,
      params.maxVoltage || 0,
      params.minVoltage || 0,
      params.sampleCount || 0,
      params.monitoringTime || "00:00:00",
      params.loadUtilization || 0,
      params.alert || "Normal"
    ];
    
    sheet.appendRow(row);
    
    return ContentService.createTextOutput("SUCCESS")
      .setMimeType(ContentService.MimeType.TEXT);
      
  } catch (err) {
    return ContentService.createTextOutput("ERROR: " + err.message)
      .setMimeType(ContentService.MimeType.TEXT);
  }
}
