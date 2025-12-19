#define BLYNK_TEMPLATE_ID "TMPL6tPkLWUtn"
#define BLYNK_TEMPLATE_NAME "ESP32"
#define BLYNK_AUTH_TOKEN "LWY_9nLZbf09Yvs0o2TWXwLvIbEEdHIP"

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include "mbedtls/md.h"
#include <BlynkSimpleEsp32.h>
#include <ArduinoOTA.h>
#include <Update.h>
// ---------- NTP Time Synchronization ----------
#include <NTPClient.h>
#include <WiFiUdp.h>

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ae.pool.ntp.org", 10800, 60000); // Update every 60 seconds

#define SERIAL_BAUD 115200
#define TX_PIN  03    // 21
#define RX_PIN  01    // 20
#define WS_PORT 81
#define PREF_NAMESPACE "aqprefs"

const int NUM_PRESETS = 8;
const int JWT_EXP_SECONDS = 3600; // 1 hour

// WiFi Reconnection variables
unsigned long lastWiFiReconnectAttempt = 0;
const unsigned long WIFI_RECONNECT_INTERVAL = 10000; // 10 seconds
int wifiReconnectCount = 0;
const int MAX_WIFI_RECONNECT_ATTEMPTS = 10;
bool wifiConnected = false;


WebServer server(80);
WebSocketsServer webSocket(WS_PORT);
Preferences prefs;
String jwtSecret;

// Default values (used if Preferences empty)
const char* DEFAULT_WIFI_SSID = "Adnaan";
const char* DEFAULT_WIFI_PASS = "plumauto764";
const char* DEFAULT_WEB_USER  = "admin";
const char* DEFAULT_WEB_PASS  = "password";

// Blynk Virtual Pins - OPTIMIZED with dual-mode switches
#define BLYNK_VPIN_BRIGHTNESS V0
#define BLYNK_VPIN_PRESET V2
#define BLYNK_VPIN_FEED_NOW V3
#define BLYNK_VPIN_WATER_CHANGE V4
#define BLYNK_VPIN_TEMPERATURE V5
#define BLYNK_VPIN_PH V6
#define BLYNK_VPIN_WATER_LEVEL V7
#define BLYNK_VPIN_STATUS V8

// Dual-mode toggle switches
#define BLYNK_VPIN_SUNRISE_SUNSET V9      // OFF=Sunrise, ON=Sunset
#define BLYNK_VPIN_MOONLIGHT_CLOUDY V10   // OFF=Moonlight, ON=Cloudy  
#define BLYNK_VPIN_THUNDER_RAINY V11      // OFF=Thunder, ON=Rainy
#define BLYNK_VPIN_FOG_RGB V12            // OFF=Fog, ON=RGB
#define BLYNK_VPIN_OFF_MODE V13           // Simple OFF button
#define BLYNK_VPIN_SHOW_IP V14            // Show IP on TFT
#define BLYNK_VPIN_FILTER_STATUS V15      // Filter ON/OFF status
#define BLYNK_VPIN_WC_STATUS V16          // Water Change status
#define BLYNK_VPIN_RGB_PRESET_UP V17      // RGB Preset Up
#define BLYNK_VPIN_RGB_PRESET_DOWN V18    // RGB Preset Down
#define BLYNK_VPIN_FILTER_TOGGLE V19      // Filter ON/OFF toggle

// --- ESP32 <-> Mega compatibility helpers ---
// Use the UART you wired to the Mega (Serial2 is common on ESP32).
// Example Serial2.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN); in setup().

// Binary command bytes (match Mega side defines)
const uint8_t CMD_GET_STATUS     = 0x01;
const uint8_t CMD_SET_BRIGHTNESS = 0x02;
const uint8_t CMD_SET_MODE       = 0x03;
const uint8_t CMD_SET_PRESET     = 0x04;
const uint8_t CMD_FEED_NOW       = 0x05;
const uint8_t CMD_SET_REFILL_START = 0x06;
const uint8_t CMD_SET_REFILL_STOP  = 0x07;
const uint8_t CMD_SET_DRAIN_LEVEL  = 0x08;
const uint8_t CMD_START_WC       = 0x09;
const uint8_t CMD_STOP_WC        = 0x0A;
const uint8_t CMD_SHOW_IP        = 0x0B;
const uint8_t CMD_RELEARN_RF     = 0x0C;
const uint8_t CMD_SET_DATETIME   = 0x0E;
const uint8_t CMD_CANCEL_TRANSIENT = 0x0F;
// ... extend as needed

// Mode mapping: ensure numbers match Mega's MODE_* constants
const char* MODE_MAP[] = {
  "SUNRISE", "SUNSET", "MOONLIGHT", "CLOUDY", "RAINY",
  "THUNDER", "FOG", "RGB", "OFF"
};

const uint8_t MODE_COUNT = sizeof(MODE_MAP) / sizeof(MODE_MAP[0]);

// Preset names array (same as webpage)
const char* presetNames[] = {
  "Red", "Green", "Blue", "White", "Purple", "Yellow", "Cyan", "Magenta", 
  "Orange", "Pink", "Rose Pink", "Forest Green", "Aquamarine", "Tomato Red", 
  "Deep Sky Blue", "Gold", "Off"
};
const int NUM_PRESETs = sizeof(presetNames) / sizeof(presetNames[0]);

// Preset colors array (same as webpage)
const char* presetColors[] = {
  "#FF0000", "#00FF00", "#0000FF", "#FFFFFF", "#800080", "#FFFF00", "#00FFFF", "#FF00FF",
  "#FFA500", "#FF70FA", "#FF70FA", "#16C940", "#69FFBE", "#E01600", "#00BFC", "#FFD700", "#000000"
};

// Rotating status display - cycles through different info
unsigned long lastStatusRotate = 0;
const unsigned long STATUS_ROTATE_INTERVAL = 5000; // Change every 5 seconds
int statusDisplayMode = 0;

// Helper - send a text command (Mega expects newline-terminated lines)
void sendTextCmd(Stream &mega, const String &cmd) {
  mega.print(cmd);
  mega.print('\n'); // Mega reads until '\n' in processTextCommand()
  // Optional small delay if you have flow problems:
  // delay(5);
}

// Public convenience functions to issue the common commands:

// Set brightness (0-100) - sends text by default
void espSetBrightness(Stream &mega, uint8_t brightness, bool useBinary=false) {
  brightness = constrain(brightness, 0, 100);
  if (useBinary) {
    sendBinaryCmd(mega, CMD_SET_BRIGHTNESS, brightness);
  } else {
    sendTextCmd(mega, String("SET_BRIGHTNESS:") + String(brightness));
  }
}

// Set lighting mode by index or name. Accepts numeric string or name.
void espSetMode(Stream &mega, const String &modeOrIndex, bool useBinary=false) {
  int idx = -2;
  // try numeric first
  if (modeOrIndex.length() > 0 && isdigit(modeOrIndex.charAt(0))) {
    idx = modeOrIndex.toInt();
  } else {
    idx = modeNameToIndex(modeOrIndex);
  }
  if (idx < 0 || idx >= MODE_COUNT) {
    Serial.printf("espSetMode: invalid mode '%s'\n", modeOrIndex.c_str());
    return;
  }
  if (useBinary) {
    sendBinaryCmd(mega, CMD_SET_MODE, (uint8_t)idx);
  } else {
    sendTextCmd(mega, String("SET_MODE:") + String(idx));
  }
}

// Set RGB preset by index
void espSetPreset(Stream &mega, uint8_t presetIndex, bool useBinary=false) {
  if (useBinary) {
    sendBinaryCmd(mega, CMD_SET_PRESET, presetIndex);
  } else {
    sendTextCmd(mega, String("SET_PRESET:") + String(presetIndex));
  }
}

// Feed now for number of seconds (1-15)
void espFeedNow(Stream &mega, uint8_t seconds, bool useBinary=false) {
  seconds = constrain(seconds, 1, 15);
  if (useBinary) {
    sendBinaryCmd(mega, CMD_FEED_NOW, seconds);
  } else {
    sendTextCmd(mega, String("FEED_NOW:") + String(seconds));
  }
}

// Show IP on Mega TFT
void espShowIP(Stream &mega) {
  sendTextCmd(mega, "SHOW_IP");
}

// Start / Stop water change
void espStartWaterChange(Stream &mega) { sendTextCmd(mega, "START_WC"); }
void espStopWaterChange(Stream &mega)  { sendTextCmd(mega, "STOP_WC"); }

// Relearn RF
void espRelearnRF(Stream &mega) { sendTextCmd(mega, "RELEARN_RF"); }

// Request status
void espRequestStatus(Stream &mega) { sendTextCmd(mega, "GET_STATUS"); }

// Example: set sunrise start/stop times (HH:MM)
void espSetSunriseStart(Stream &mega, const String &timeHHMM) { sendTextCmd(mega, "SET_SUNRISE_START:" + timeHHMM); }
void espSetSunriseStop (Stream &mega, const String &timeHHMM) { sendTextCmd(mega, "SET_SUNRISE_STOP:" + timeHHMM); }

// --- Receiving responses from Mega ---
// This simple handler reads lines sent back by Mega (TEXT responses)
// and prints them to Serial (or you can parse them as needed).
void processMegaResponses(Stream &mega) {
  while (mega.available()) {
    String line = mega.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    // Example: display on main Serial for debug
    Serial.print("[Mega] ");
    Serial.println(line);

    // If you want to parse STATUS: lines from Mega:
    if (line.startsWith("STATUS:")) {
      // STATUS payload format (example from Mega): "STATUS:BRIGHT:xx,MODE:yy,TEMP:zz,..."
      // Or your Mega's sendStatusUpdate implementation might be different.
      // Add parsing logic here if the ESP must react to updated values.
    }
  }
}

// ---------- OTA Update Handlers ----------
void handleOTAPage() {
  // Simple OTA update page - no authentication required
  String html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 OTA Update</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; background: #071021; color: #eaf3f6; padding: 20px; }
        .container { max-width: 500px; margin: 0 auto; background: #0b1220; padding: 20px; border-radius: 10px; }
        h2 { color: #10b981; }
        input[type="file"] { margin: 10px 0; }
        button { background: #10b981; color: #042018; border: none; padding: 10px 20px; border-radius: 5px; cursor: pointer; }
        .progress { width: 100%; background: #333; border-radius: 5px; margin: 10px 0; }
        .progress-bar { width: 0%; height: 20px; background: #10b981; border-radius: 5px; text-align: center; color: white; }
    </style>
</head>
<body>
    <div class="container">
        <h2>ESP32 OTA Update</h2>
        <form method="POST" action="/update" enctype="multipart/form-data">
            <input type="file" name="update" accept=".bin">
            <button type="submit">Update Firmware</button>
        </form>
        <div id="progress" style="display:none;">
            <div class="progress">
                <div id="progress-bar" class="progress-bar">0%</div>
            </div>
            <div id="status">Uploading...</div>
        </div>
    </div>
    <script>
        document.querySelector('form').addEventListener('submit', function(e) {
            var fileInput = document.querySelector('input[type="file"]');
            if (!fileInput.files.length) {
                alert('Please select a firmware file');
                e.preventDefault();
                return;
            }
            document.getElementById('progress').style.display = 'block';
            
            var xhr = new XMLHttpRequest();
            var formData = new FormData(this);
            
            xhr.upload.addEventListener('progress', function(e) {
                if (e.lengthComputable) {
                    var percent = (e.loaded / e.total) * 100;
                    document.getElementById('progress-bar').style.width = percent + '%';
                    document.getElementById('progress-bar').textContent = Math.round(percent) + '%';
                }
            });
            
            xhr.addEventListener('load', function() {
                document.getElementById('status').textContent = 'Update complete! Rebooting...';
                setTimeout(function() {
                    window.location.href = '/';
                }, 3000);
            });
            
            xhr.open('POST', '/update');
            xhr.send(formData);
            e.preventDefault();
        });
    </script>
</body>
</html>
)";
  server.send(200, "text/html", html);
}
// ---------- OTA Update Handlers ----------
void handleOTAUpdate() {
  // Allow OTA updates without authentication in both STA and AP modes
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
  ESP.restart();
}

void handleOTAUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA Update: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA Success: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

void handleOTAStatus() {
  server.send(200, "text/plain", "OTA Ready");
}

// ---------- helpers ----------
String base64urlEncode(const uint8_t *data, size_t len) {
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  String out;
  int val = 0, valb = -6;
  for (size_t i=0;i<len;i++) {
    val = (val<<8) + data[i];
    valb += 8;
    while (valb >= 0) {
      out += b64[(val>>valb)&0x3F];
      valb -= 6;
    }
  }
  if (valb > -6) {
    out += b64[((val<<8)>>(valb+8))&0x3F];
  }
  return out;
}
String base64urlEncode(const String &s) { return base64urlEncode((const uint8_t*)s.c_str(), s.length()); }

String hmacSha256Base64Url(const String &key, const String &msg) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint8_t out[32];
  mbedtls_md_hmac(info, (const unsigned char*)key.c_str(), key.length(),
                  (const unsigned char*)msg.c_str(), msg.length(), out);
  return base64urlEncode(out, 32);
}

// Create JWT header.payload.signature
String createJWT(const String &sub) {
  unsigned long now = (unsigned long)time(NULL);
  unsigned long exp = now + JWT_EXP_SECONDS;
  String header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
  String payload = String("{\"sub\":\"") + sub + String("\",\"iat\":") + String(now) + String(",\"exp\":") + String(exp) + String("}");
  String h_b64 = base64urlEncode(header);
  String p_b64 = base64urlEncode(payload);
  String signing = h_b64 + "." + p_b64;
  String sig = hmacSha256Base64Url(jwtSecret, signing);
  return signing + "." + sig;
}

// minimal base64url decode -> raw bytes -> ASCII string
String base64urlDecodeToString(const String &in) {
  String b = in;
  b.replace('-','+'); b.replace('_','/');
  while (b.length() % 4) b += '=';
  // decoding table
  static const unsigned char dtable[] = {
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
    52,53,54,55,56,57,58,59,60,61,64,64,64, 0,64,64,
    64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
    64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51
  };
  String out;
  int val=0, valb=-8;
  for (size_t i=0;i<b.length();i++) {
    unsigned char c = b[i];
    if (c > 127) return String();
    unsigned char d = dtable[c];
    if (d == 64) continue;
    val = (val<<6) + d;
    valb += 6;
    if (valb >= 0) {
      out += char((val>>valb)&0xFF);
      valb -= 8;
    }
  }
  return out;
}

// verify token; return subject (sub) on success, empty on failure
String verifyJWT(const String &token) {
  int p1 = token.indexOf('.');
  int p2 = token.indexOf('.', p1+1);
  if (p1<0 || p2<0) return "";
  String signing = token.substring(0,p2); // header.payload
  String sig = token.substring(p2+1);
  String expected = hmacSha256Base64Url(jwtSecret, signing);
  if (expected != sig) return "";
  // decode payload
  String payload_b64 = token.substring(p1+1,p2);
  String payload = base64urlDecodeToString(payload_b64);
  if (payload.length()==0) return "";
  int iexp = payload.indexOf("\"exp\":");
  if (iexp<0) return "";
  int j = iexp+6;
  while (j < (int)payload.length() && (payload[j]<'0' || payload[j]>'9')) j++;
  unsigned long exp = 0;
  while (j < (int)payload.length() && payload[j]>='0' && payload[j]<='9') { exp = exp*10 + (payload[j]-'0'); j++; }
  if (exp < (unsigned long)time(NULL)) return "";
  int isub = payload.indexOf("\"sub\":\"");
  if (isub<0) return "";
  isub += 7;
  int ie = payload.indexOf('"', isub);
  if (ie<0) return "";
  return payload.substring(isub, ie);
}

// ---------- Preferences helpers ----------
void ensurePrefsDefaults() {
  prefs.begin(PREF_NAMESPACE, false);
  if (!prefs.isKey("user")) prefs.putString("user", DEFAULT_WEB_USER);
  if (!prefs.isKey("pass")) prefs.putString("pass", DEFAULT_WEB_PASS);
  if (!prefs.isKey("wifi_ssid")) prefs.putString("wifi_ssid", DEFAULT_WIFI_SSID);
  if (!prefs.isKey("wifi_pass")) prefs.putString("wifi_pass", DEFAULT_WIFI_PASS);
  if (!prefs.isKey("jwt_secret")) {
    String s = String(millis(), HEX) + String(random(0xFFFF), HEX);
    prefs.putString("jwt_secret", s);
  }
  jwtSecret = prefs.getString("jwt_secret", "");
}

String prefGetUser(){ return prefs.getString("user", DEFAULT_WEB_USER); }
String prefGetPass(){ return prefs.getString("pass", DEFAULT_WEB_PASS); }
String prefGetWifiSsid(){ return prefs.getString("wifi_ssid", DEFAULT_WIFI_SSID); }
String prefGetWifiPass(){ return prefs.getString("wifi_pass", DEFAULT_WIFI_PASS); }

// ---------- Auth check ----------
bool checkAuth() {
  // cookie 'session'
  if (server.hasHeader("Cookie")) {
    String cookie = server.header("Cookie");
    Serial.printf("DEBUG: Cookie header: %s\n", cookie.c_str());
    int p = cookie.indexOf("session=");
    if (p >= 0) {
      String tok = cookie.substring(p + 8);
      int semi = tok.indexOf(';');
      if (semi >= 0) tok = tok.substring(0, semi);
      String sub = verifyJWT(tok);
      Serial.printf("DEBUG: verifyJWT from cookie -> sub: '%s'\n", sub.c_str());
      if (sub.length()) return true;
      else Serial.println("DEBUG: cookie token invalid/expired");
    } else {
      Serial.println("DEBUG: cookie header present but no session= entry");
    }
  } else {
    Serial.println("DEBUG: no Cookie header");
  }
  // Authorization: Bearer
  if (server.hasHeader("Authorization")) {
    String auth = server.header("Authorization");
    Serial.printf("DEBUG: Authorization header: %s\n", auth.c_str());
    if (auth.startsWith("Bearer ")) {
      String tok = auth.substring(7);
      String sub = verifyJWT(tok);
      Serial.printf("DEBUG: verifyJWT from Authorization -> sub: '%s'\n", sub.c_str());
      if (sub.length()) return true;
      else Serial.println("DEBUG: bearer token invalid/expired");
    } else {
      Serial.println("DEBUG: Authorization header present but not Bearer");
    }
  } else {
    Serial.println("DEBUG: no Authorization header");
  }
  return false;
}

void handleMegaCommunication() {
  static String lineBuf;
  static unsigned long lastDataReceived = 0;
  const unsigned long COMM_TIMEOUT = 10000; // 10 seconds
  
  // Check for Serial1 data
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    Serial.write(c); // Echo to Serial for debugging
    
    if (c == '\r') continue;
    
    if (c == '\n') {
      if (lineBuf.length() > 0) {
        lastDataReceived = millis();
        
        // Check for time sync requests from Mega2560 - FIXED VERSION
        if (lineBuf.startsWith("TIME_SYNC:")) {
          Serial.println("Mega2560 requested time synchronization: " + lineBuf);
          handleTimeSyncRequest();
          lineBuf = "";
          continue;
        }
        
        // Send to WebSocket
        wsBroadcast(lineBuf);
        
        // Parse the data
        parseMegaData(lineBuf);
        
        lineBuf = "";
      }
    } else {
      lineBuf += c;
      if (lineBuf.length() > 4096) {
        wsBroadcast("BUFFER_OVERFLOW:" + lineBuf.substring(0, 50));
        lineBuf = "";
      }
    }
  }
  
  // Check for communication timeout
  if (millis() - lastDataReceived > COMM_TIMEOUT && lastDataReceived > 0) {
    Serial.println("WARNING: No data from Mega2560 for 10 seconds");
    wsBroadcast("COMM_TIMEOUT:No data from Mega");
    lastDataReceived = millis(); // Reset to avoid spamming
  }
}

// Global variables to store parsed data
int currentBrightness = 0;
int currentMode = 0;
float currentTemperature = 0;
float currentPH = 0;
int currentWaterLevel = 0;
int currentPreset = 0;

int currentFilterStatus = 0;      // 0=OFF, 1=ON
int currentWCStatus = 0;          // 0=Stopped, 1=Running
String currentWCState = "Stopped"; // Detailed water change state

void parseMegaData(String data) {
    data.trim();
    Serial.println("Parsing: " + data);
    
    if (data.startsWith("STATUS:")) {
        String statusData = data.substring(7);
        
        // Parse basic status (existing code)
        int firstComma = statusData.indexOf(',');
        int secondComma = statusData.indexOf(',', firstComma + 1);
        int thirdComma = statusData.indexOf(',', secondComma + 1);
        int fourthComma = statusData.indexOf(',', thirdComma + 1);
        int fifthComma = statusData.indexOf(',', fourthComma + 1);
        
        if (firstComma != -1 && secondComma != -1 && thirdComma != -1 && 
            fourthComma != -1 && fifthComma != -1) {
            
            // Extract and store basic values
            currentBrightness = statusData.substring(0, firstComma).toInt();
            currentMode = statusData.substring(firstComma + 1, secondComma).toInt();
            currentTemperature = statusData.substring(secondComma + 1, thirdComma).toFloat();
            currentPH = statusData.substring(thirdComma + 1, fourthComma).toFloat();
            currentWaterLevel = statusData.substring(fourthComma + 1, fifthComma).toInt();
            currentPreset = statusData.substring(fifthComma + 1).toInt();
        }
        
        // Parse new status parameters
        int filterIndex = statusData.indexOf("FILTER:");
        if (filterIndex != -1) {
            int filterEnd = statusData.indexOf(',', filterIndex);
            if (filterEnd == -1) filterEnd = statusData.length();
            String filterStr = statusData.substring(filterIndex + 7, filterEnd);
            currentFilterStatus = (filterStr == "ON" || filterStr == "1") ? 1 : 0;
        }
        
        int wcIndex = statusData.indexOf("WC:");
        if (wcIndex != -1) {
            int wcEnd = statusData.indexOf(',', wcIndex);
            if (wcEnd == -1) wcEnd = statusData.length();
            currentWCState = statusData.substring(wcIndex + 3, wcEnd);
            currentWCStatus = (currentWCState == "RUNNING" || currentWCState == "1") ? 1 : 0;
        }
        
        // Also check for water change specific states
        if (statusData.indexOf("WATER_CHANGE:") != -1) {
            currentWCStatus = 1;
            currentWCState = "Running";
        } else if (statusData.indexOf("WC_STOPPED") != -1) {
            currentWCStatus = 0;
            currentWCState = "Stopped";
        } else if (statusData.indexOf("DRAINING") != -1) {
            currentWCStatus = 1;
            currentWCState = "Draining";
        } else if (statusData.indexOf("REFILLING") != -1) {
            currentWCStatus = 1;
            currentWCState = "Refilling";
        }
        
        Serial.printf("Parsed: Bright=%d%%, Mode=%d, Temp=%.1f, pH=%.2f, Level=%d, Preset=%d, Filter=%d, WC=%s\n",
                     currentBrightness, currentMode, currentTemperature, currentPH, 
                     currentWaterLevel, currentPreset, currentFilterStatus, currentWCState.c_str());
        
        // Update Blynk with optimized updates
        updateBlynkValues();
        
        // Update WebSocket with proper format for web page
        updateWebSocketStatus();
    }
    
    // Also parse individual status messages
    if (data.startsWith("FILTER:")) {
        String filterState = data.substring(7);
        currentFilterStatus = (filterState == "ON" || filterState == "1") ? 1 : 0;
        updateBlynkValues();
    }
    
    if (data.startsWith("WC:") || data.startsWith("WATER_CHANGE:")) {
        String wcState = data.substring(data.indexOf(':') + 1);
        currentWCState = wcState;
        currentWCStatus = (wcState == "RUNNING" || wcState == "DRAINING" || wcState == "REFILLING") ? 1 : 0;
        updateBlynkValues();
    }
}

void updateWebSocketStatus() {
    String filterState = currentFilterStatus ? "ON" : "OFF";
    String wsData = "BRIGHT:" + String(currentBrightness) + 
                   ",MODE:" + String(currentMode) +
                   ",TEMP:" + String(currentTemperature, 1) +
                   ",PH:" + String(currentPH, 2) +
                   ",LEVEL:" + String(currentWaterLevel) +
                   ",RGB_PRESET:" + String(currentPreset) +
                   ",FILTER:" + filterState +
                   ",WC:" + currentWCState;
    
    wsBroadcast(wsData);
    Serial.println("WebSocket: " + wsData);
}
// ---------- Serial bridge ----------
String sendSerialCommand(const String &cmd, unsigned long timeout = 1500) {
  Serial1.print(cmd);
  Serial1.print('\n');
  unsigned long start = millis();
  String res;
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (c == '\r') continue;
      if (c == '\n') {
        if (res.length()) return res;
        continue;
      }
      res += c;
      if (res.length() > 4096) return res;
    }
    delay(5);
  }
  return res.length() ? res : String("ERR:TIMEOUT");
}
void wsBroadcast(String msg) { webSocket.broadcastTXT(msg); }


const char LOGIN_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Login - Aquarium Controller</title>
<link rel='stylesheet' href='https://cdn.jsdelivr.net/npm/modern-css-reset/dist/reset.min.css'>
<style>
:root {
  --primary: #10b981;
  --primary-dark: #059669;
  --secondary: #06b6d4;
  --dark-bg: #071021;
  --darker-bg: #050a18;
  --card-bg: rgba(255,255,255,0.03);
  --card-border: rgba(255,255,255,0.06);
  --text-primary: #eaf3f6;
  --text-secondary: #9aa6b2;
  --text-muted: #6b7280;
  --success: #10b981;
  --warning: #f59e0b;
  --error: #ef4444;
  --radius-lg: 16px;
  --radius-md: 12px;
  --radius-sm: 8px;
}

* {
  box-sizing: border-box;
}

body {
  font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
  background: linear-gradient(135deg, var(--darker-bg) 0%, var(--dark-bg) 100%);
  color: var(--text-primary);
  line-height: 1.6;
  min-height: 100vh;
  padding: 0;
  margin: 0;
  display: flex;
  align-items: center;
  justify-content: center;
}

.container {
  max-width: 440px;
  width: 90%;
  margin: 20px auto;
}

.card {
  background: var(--card-bg);
  border: 1px solid var(--card-border);
  border-radius: var(--radius-lg);
  padding: 40px;
  backdrop-filter: blur(20px);
  box-shadow: 0 8px 40px rgba(2, 6, 23, 0.4);
  position: relative;
  overflow: hidden;
}

.card::before {
  content: '';
  position: absolute;
  top: 0;
  left: 0;
  right: 0;
  height: 3px;
  background: linear-gradient(90deg, var(--secondary), var(--primary));
}

.header {
  text-align: center;
  margin-bottom: 32px;
}

.logo {
  width: 80px;
  height: 80px;
  border-radius: var(--radius-md);
  background: linear-gradient(135deg, var(--secondary), var(--primary));
  display: flex;
  align-items: center;
  justify-content: center;
  font-weight: 800;
  font-size: 32px;
  color: white;
  box-shadow: 0 8px 25px rgba(6, 182, 212, 0.4);
  margin: 0 auto 20px;
  transition: transform 0.3s ease;
}

.logo:hover {
  transform: scale(1.05) rotate(5deg);
}

h1 {
  margin: 0 0 8px;
  font-size: 32px;
  font-weight: 700;
  background: linear-gradient(135deg, var(--text-primary), var(--secondary));
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
}

.subtitle {
  color: var(--text-secondary);
  font-size: 15px;
  font-weight: 400;
}

.form-group {
  margin-bottom: 24px;
  position: relative;
}

label {
  display: block;
  margin-bottom: 8px;
  color: var(--text-primary);
  font-weight: 500;
  font-size: 14px;
  display: flex;
  align-items: center;
  gap: 6px;
}

label::before {
  font-size: 16px;
}

.user-label::before {
  content: "👤";
}

.pass-label::before {
  content: "🔒";
}

input[type="text"], input[type="password"] {
  width: 100%;
  background: rgba(255,255,255,0.05);
  border: 1px solid var(--card-border);
  border-radius: var(--radius-md);
  padding: 16px 18px;
  color: var(--text-primary);
  font-size: 15px;
  transition: all 0.3s ease;
  font-family: inherit;
}

input:focus {
  outline: none;
  border-color: var(--primary);
  box-shadow: 0 0 0 3px rgba(16, 185, 129, 0.15);
  transform: translateY(-1px);
}

input::placeholder {
  color: var(--text-muted);
}

.btn {
  width: 100%;
  background: linear-gradient(135deg, var(--primary), var(--primary-dark));
  color: white;
  border: none;
  padding: 16px 20px;
  border-radius: var(--radius-md);
  font-weight: 600;
  cursor: pointer;
  transition: all 0.3s ease;
  font-size: 16px;
  margin-top: 8px;
  position: relative;
  overflow: hidden;
}

.btn:hover {
  transform: translateY(-2px);
  box-shadow: 0 8px 25px rgba(16, 185, 129, 0.4);
}

.btn:active {
  transform: translateY(0);
}

.btn-loading::after {
  content: '';
  position: absolute;
  top: 50%;
  left: 50%;
  width: 20px;
  height: 20px;
  margin: -10px 0 0 -10px;
  border: 2px solid transparent;
  border-top: 2px solid #ffffff;
  border-radius: 50%;
  animation: spin 1s linear infinite;
}

@keyframes spin {
  0% { transform: rotate(0deg); }
  100% { transform: rotate(360deg); }
}

.error-message {
  background: rgba(239, 68, 68, 0.1);
  border: 1px solid rgba(239, 68, 68, 0.3);
  border-radius: var(--radius-md);
  padding: 14px 16px;
  margin-top: 16px;
  font-size: 14px;
  color: var(--error);
  display: none;
  align-items: center;
  gap: 8px;
  animation: slideDown 0.3s ease;
}

.error-message::before {
  content: "⚠️";
  font-size: 16px;
}

@keyframes slideDown {
  from {
    opacity: 0;
    transform: translateY(-10px);
  }
  to {
    opacity: 1;
    transform: translateY(0);
  }
}

.links {
  display: flex;
  justify-content: center;
  gap: 20px;
  margin-top: 28px;
  padding-top: 24px;
  border-top: 1px solid var(--card-border);
  flex-wrap: wrap;
}

.link {
  color: var(--text-secondary);
  text-decoration: none;
  font-size: 13px;
  transition: all 0.2s ease;
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 12px;
  border-radius: var(--radius-sm);
}

.link:hover {
  color: var(--primary);
  background: rgba(255,255,255,0.03);
  transform: translateY(-1px);
}

.link::before {
  font-size: 14px;
}

.wifi-link::before {
  content: "📶";
}

.creds-link::before {
  content: "🔑";
}

.ota-link::before {
  content: "🔄";
}

.footer {
  text-align: center;
  margin-top: 24px;
  color: var(--text-muted);
  font-size: 12px;
}

.pulse {
  animation: pulse 2s infinite;
}

@keyframes pulse {
  0% {
    box-shadow: 0 0 0 0 rgba(6, 182, 212, 0.4);
  }
  70% {
    box-shadow: 0 0 0 10px rgba(6, 182, 212, 0);
  }
  100% {
    box-shadow: 0 0 0 0 rgba(6, 182, 212, 0);
  }
}

@media (max-width: 480px) {
  .container {
    width: 95%;
  }
  
  .card {
    padding: 30px 24px;
  }
  
  .logo {
    width: 70px;
    height: 70px;
    font-size: 28px;
  }
  
  h1 {
    font-size: 28px;
  }
  
  .links {
    gap: 12px;
  }
  
  .link {
    font-size: 12px;
    padding: 6px 10px;
  }
}

/* Input focus animations */
.input-container {
  position: relative;
}

.input-highlight {
  position: absolute;
  bottom: 0;
  left: 0;
  height: 2px;
  width: 0;
  background: linear-gradient(90deg, var(--secondary), var(--primary));
  transition: width 0.3s ease;
}

input:focus + .input-highlight {
  width: 100%;
}
</style>
</head>
<body>
<div class="container">
  <div class="card">
    <div class="header">
      <div class="logo pulse">AQ</div>
      <h1>Welcome Back</h1>
      <div class="subtitle">Sign in to access your Aquarium Controller</div>
    </div>

    <form id="loginForm" onsubmit="doLogin(event)">
      <div class="form-group">
        <label for="user" class="user-label">Username</label>
        <div class="input-container">
          <input type="text" id="user" placeholder="Enter your username" required>
          <div class="input-highlight"></div>
        </div>
      </div>

      <div class="form-group">
        <label for="pass" class="pass-label">Password</label>
        <div class="input-container">
          <input type="password" id="pass" placeholder="Enter your password" required>
          <div class="input-highlight"></div>
        </div>
      </div>

      <div id="error" class="error-message">
        Authentication failed. Please check your credentials.
      </div>

      <button type="submit" class="btn" id="loginBtn">
        Sign In to Dashboard
      </button>
    </form>

    <div class="links">
      <a href="/wifi" class="link wifi-link">Wi-Fi Setup</a>
      <a href="/change-creds" class="link creds-link">Credentials</a>
      <a href="/ota" class="link ota-link">OTA Update</a>
    </div>
  </div>

  <div class="footer">
    Aquarium Controller System • Secure Access
  </div>
</div>

<script>
async function doLogin(e) {
  e.preventDefault();
  
  const userInput = document.getElementById('user');
  const passInput = document.getElementById('pass');
  const errorDiv = document.getElementById('error');
  const loginBtn = document.getElementById('loginBtn');
  
  const username = userInput.value.trim();
  const password = passInput.value;
  
  // Hide previous errors
  errorDiv.style.display = 'none';
  
  if (!username || !password) {
    showError('Please enter both username and password');
    return;
  }
  
  // Show loading state
  const originalText = loginBtn.innerHTML;
  loginBtn.innerHTML = 'Authenticating...';
  loginBtn.classList.add('btn-loading');
  loginBtn.disabled = true;
  
  try {
    const body = new URLSearchParams();
    body.append('user', username);
    body.append('pass', password);
    
    const response = await fetch('/login', {
      method: 'POST',
      body: body,
      credentials: 'same-origin'
    });
    
    if (response.status === 200) {
      const data = await response.json();
      
      if (data && data.token) {
        // Successful login with token
        loginBtn.innerHTML = 'Success! Redirecting...';
        
        // Add slight delay for better UX
        setTimeout(() => {
          window.location.href = '/?token=' + encodeURIComponent(data.token);
        }, 800);
        
      } else {
        // Login successful but no token (shouldn't happen normally)
        window.location.href = '/';
      }
      
    } else {
      const errorText = await response.text();
      showError(errorText || 'Login failed. Please check your credentials.');
    }
    
  } catch (error) {
    showError('Network error: Unable to connect to the device');
    console.error('Login error:', error);
  } finally {
    // Reset button state if not redirecting
    if (!loginBtn.innerHTML.includes('Redirecting')) {
      loginBtn.innerHTML = originalText;
      loginBtn.classList.remove('btn-loading');
      loginBtn.disabled = false;
    }
  }
}

function showError(message) {
  const errorDiv = document.getElementById('error');
  errorDiv.innerHTML = `<span>${message}</span>`;
  errorDiv.style.display = 'flex';
  
  // Add shake animation to inputs
  const inputs = document.querySelectorAll('input');
  inputs.forEach(input => {
    input.style.animation = 'none';
    setTimeout(() => {
      input.style.animation = 'shake 0.5s ease-in-out';
    }, 10);
  });
  
  // Create shake animation
  if (!document.querySelector('#shake-animation')) {
    const style = document.createElement('style');
    style.id = 'shake-animation';
    style.textContent = `
      @keyframes shake {
        0%, 100% { transform: translateX(0); }
        25% { transform: translateX(-5px); }
        75% { transform: translateX(5px); }
      }
    `;
    document.head.appendChild(style);
  }
}

// Add enter key support and auto-focus
document.addEventListener('DOMContentLoaded', function() {
  document.getElementById('user').focus();
  
  // Enter key to submit
  document.addEventListener('keypress', function(e) {
    if (e.key === 'Enter') {
      const loginBtn = document.getElementById('loginBtn');
      if (!loginBtn.disabled) {
        document.getElementById('loginForm').requestSubmit();
      }
    }
  });
  
  // Clear error when user starts typing
  const inputs = document.querySelectorAll('input');
  inputs.forEach(input => {
    input.addEventListener('input', function() {
      const errorDiv = document.getElementById('error');
      if (errorDiv.style.display !== 'none') {
        errorDiv.style.display = 'none';
      }
    });
  });
});

// Add visual feedback for inputs
document.querySelectorAll('input').forEach(input => {
  input.addEventListener('focus', function() {
    this.parentElement.classList.add('focused');
  });
  
  input.addEventListener('blur', function() {
    this.parentElement.classList.remove('focused');
  });
});
</script>
</body>
</html>
)HTML";


// Split the HTML into multiple parts to avoid PROGMEM issues
const char MAIN_PAGE_HEAD[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Aquarium Controller - Dashboard</title>
<link rel='stylesheet' href='https://cdn.jsdelivr.net/npm/modern-css-reset/dist/reset.min.css'>
<link rel='stylesheet' href='https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css'>
<!-- Chart.js for graphs -->
<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>
<style>
:root {
  --primary: #10b981;
  --primary-dark: #059669;
  --secondary: #06b6d4;
  --dark-bg: #071021;
  --darker-bg: #050a18;
  --card-bg: rgba(255,255,255,0.03);
  --card-border: rgba(255,255,255,0.06);
  --text-primary: #eaf3f6;
  --text-secondary: #9aa6b2;
  --text-muted: #6b7280;
  --success: #10b981;
  --warning: #f59e0b;
  --error: #ef4444;
  --info: #3b82f6;
  --radius-lg: 16px;
  --radius-md: 12px;
  --radius-sm: 8px;
}

* {
  box-sizing: border-box;
}

body {
  font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
  background: linear-gradient(135deg, var(--darker-bg) 0%, var(--dark-bg) 100%);
  color: var(--text-primary);
  line-height: 1.6;
  min-height: 100vh;
  padding: 0;
  margin: 0;
}

.container {
  max-width: 1200px;
  margin: 0 auto;
  padding: 20px;
}

/* Notification System */
.notifications-container {
  position: fixed;
  top: 20px;
  right: 20px;
  z-index: 1000;
  max-width: 400px;
  pointer-events: none;
}

.notification {
  background: var(--card-bg);
  border: 1px solid var(--card-border);
  border-radius: var(--radius-md);
  padding: 16px;
  margin-bottom: 12px;
  backdrop-filter: blur(20px);
  box-shadow: 0 8px 30px rgba(2, 6, 23, 0.6);
  display: flex;
  align-items: flex-start;
  gap: 12px;
  transform: translateX(400px);
  opacity: 0;
  transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
  pointer-events: all;
  border-left: 4px solid var(--primary);
}

.notification.show {
  transform: translateX(0);
  opacity: 1;
}

.notification.hide {
  transform: translateX(400px);
  opacity: 0;
}

.notification.success {
  border-left-color: var(--success);
}

.notification.warning {
  border-left-color: var(--warning);
}

.notification.error {
  border-left-color: var(--error);
}

.notification.info {
  border-left-color: var(--info);
}

.notification-icon {
  font-size: 20px;
  flex-shrink: 0;
  margin-top: 2px;
}

.notification-content {
  flex: 1;
}

.notification-title {
  font-weight: 600;
  font-size: 14px;
  margin-bottom: 4px;
  color: var(--text-primary);
}

.notification-message {
  font-size: 13px;
  color: var(--text-secondary);
  line-height: 1.4;
}

.notification-close {
  background: none;
  border: none;
  color: var(--text-muted);
  cursor: pointer;
  padding: 4px;
  border-radius: var(--radius-sm);
  transition: all 0.2s ease;
  flex-shrink: 0;
}

.notification-close:hover {
  color: var(--text-primary);
  background: rgba(255,255,255,0.05);
}

/* Header Styles */
.header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 20px 0;
  margin-bottom: 30px;
  border-bottom: 1px solid var(--card-border);
}

.logo-container {
  display: flex;
  align-items: center;
  gap: 16px;
}

.logo {
  width: 52px;
  height: 52px;
  border-radius: var(--radius-md);
  background: linear-gradient(135deg, var(--secondary), var(--primary));
  display: flex;
  align-items: center;
  justify-content: center;
  font-weight: 800;
  font-size: 20px;
  color: white;
  box-shadow: 0 4px 20px rgba(6, 182, 212, 0.3);
}

.header-content h1 {
  margin: 0;
  font-size: 28px;
  font-weight: 700;
  background: linear-gradient(135deg, var(--text-primary), var(--secondary));
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
}

.header-content .subtitle {
  color: var(--text-secondary);
  font-size: 14px;
  margin-top: 4px;
  display: flex;
  align-items: center;
  gap: 8px;
}

.header-actions {
  display: flex;
  gap: 12px;
  align-items: center;
}

/* Card Layout */
.dashboard {
  display: grid;
  grid-template-columns: 1fr 1.2fr;
  gap: 24px;
  margin-bottom: 30px;
}

.card {
  background: var(--card-bg);
  border: 1px solid var(--card-border);
  border-radius: var(--radius-lg);
  padding: 24px;
  backdrop-filter: blur(20px);
  box-shadow: 0 8px 40px rgba(2, 6, 23, 0.4);
  transition: transform 0.2s ease, box-shadow 0.2s ease;
}

.card:hover {
  transform: translateY(-2px);
  box-shadow: 0 12px 50px rgba(2, 6, 23, 0.6);
}

.card-header {
  display: flex;
  justify-content: between;
  align-items: center;
  margin-bottom: 20px;
}

.card-title {
  font-size: 18px;
  font-weight: 600;
  margin: 0;
  color: var(--text-primary);
  display: flex;
  align-items: center;
  gap: 10px;
}

.card-title i {
  color: var(--primary);
  font-size: 20px;
}

.card-subtitle {
  color: var(--text-secondary);
  font-size: 13px;
  margin-top: 4px;
  display: flex;
  align-items: center;
  gap: 6px;
}

/* Graph Section */
.graph-section {
  margin-bottom: 24px;
}

.graph-container {
  background: rgba(0, 0, 0, 0.2);
  border-radius: var(--radius-md);
  padding: 16px;
  border: 1px solid var(--card-border);
  position: relative;
  height: 300px;
}

.graph-controls {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 16px;
  gap: 12px;
}

.graph-time-selector {
  display: flex;
  gap: 8px;
}

.time-btn {
  background: rgba(255,255,255,0.05);
  border: 1px solid var(--card-border);
  color: var(--text-secondary);
  padding: 6px 12px;
  border-radius: var(--radius-sm);
  font-size: 12px;
  cursor: pointer;
  transition: all 0.2s ease;
}

.time-btn:hover {
  background: rgba(255,255,255,0.1);
  color: var(--text-primary);
}

.time-btn.active {
  background: var(--primary);
  color: white;
  border-color: var(--primary);
}

.graph-legend {
  display: flex;
  gap: 16px;
  font-size: 12px;
}

.legend-item {
  display: flex;
  align-items: center;
  gap: 6px;
}

.legend-color {
  width: 12px;
  height: 12px;
  border-radius: 2px;
}

.legend-temperature {
  background: linear-gradient(90deg, #ef4444, #f97316);
}

.legend-ph {
  background: linear-gradient(90deg, #3b82f6, #06b6d4);
}
)HTML";

const char MAIN_PAGE_MIDDLE[] PROGMEM = R"HTML(
/* Controls Section */
.control-group {
  margin-bottom: 24px;
}

.control-label {
  display: block;
  margin-bottom: 8px;
  color: var(--text-primary);
  font-weight: 500;
  font-size: 14px;
  display: flex;
  align-items: center;
  gap: 8px;
}

.control-label i {
  color: var(--primary);
  width: 20px;
  text-align: center;
}

.control-value {
  color: var(--primary);
  font-weight: 600;
  margin-left: 8px;
}

.slider-container {
  background: rgba(255,255,255,0.02);
  border-radius: var(--radius-md);
  padding: 16px;
  border: 1px solid var(--card-border);
  position: relative;
}

.slider-container::before {
  content: '🔆';
  position: absolute;
  left: 12px;
  top: 50%;
  transform: translateY(-50%);
  font-size: 16px;
  opacity: 0.7;
}

input[type="range"] {
  width: 100%;
  height: 6px;
  border-radius: 3px;
  background: rgba(255,255,255,0.1);
  outline: none;
  -webkit-appearance: none;
  margin-left: 30px;
}

input[type="range"]::-webkit-slider-thumb {
  -webkit-appearance: none;
  width: 20px;
  height: 20px;
  border-radius: 50%;
  background: var(--primary);
  cursor: pointer;
  box-shadow: 0 2px 10px rgba(16, 185, 129, 0.4);
  transition: all 0.2s ease;
}

input[type="range"]::-webkit-slider-thumb:hover {
  transform: scale(1.1);
  box-shadow: 0 4px 15px rgba(16, 185, 129, 0.6);
}

/* Status Chips */
.status-grid {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 12px;
  margin: 20px 0;
}

.status-chip {
  background: rgba(255,255,255,0.02);
  border: 1px solid var(--card-border);
  border-radius: var(--radius-md);
  padding: 16px;
  text-align: center;
  transition: all 0.3s ease;
  position: relative;
  overflow: hidden;
}

.status-chip:hover {
  background: rgba(255,255,255,0.04);
  border-color: var(--primary);
  transform: translateY(-2px);
}

.status-chip::before {
  content: '';
  position: absolute;
  top: 0;
  left: 0;
  right: 0;
  height: 2px;
  background: linear-gradient(90deg, var(--secondary), var(--primary));
  transform: scaleX(0);
  transition: transform 0.3s ease;
}

.status-chip.changed::before {
  transform: scaleX(1);
  animation: pulseGlow 2s ease-in-out;
}

@keyframes pulseGlow {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.6; }
}

.status-value {
  font-size: 20px;
  font-weight: 700;
  color: var(--primary);
  display: block;
  transition: all 0.3s ease;
}

.status-chip.changed .status-value {
  animation: valueChange 1s ease;
}

@keyframes valueChange {
  0% { transform: scale(1); }
  50% { transform: scale(1.1); color: var(--warning); }
  100% { transform: scale(1); }
}

.status-label {
  font-size: 12px;
  color: var(--text-secondary);
  margin-top: 4px;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
}

.status-icon {
  font-size: 14px;
  opacity: 0.8;
}

/* Button Styles */
.btn {
  background: var(--primary);
  color: white;
  border: none;
  padding: 12px 20px;
  border-radius: var(--radius-md);
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s ease;
  display: inline-flex;
  align-items: center;
  gap: 8px;
  font-size: 14px;
}

.btn:hover {
  background: var(--primary-dark);
  transform: translateY(-1px);
  box-shadow: 0 4px 20px rgba(16, 185, 129, 0.3);
}

.btn-secondary {
  background: transparent;
  border: 1px solid var(--card-border);
  color: var(--text-secondary);
}

.btn-secondary:hover {
  background: rgba(255,255,255,0.05);
  border-color: var(--text-secondary);
  color: var(--text-primary);
}

.btn-warning {
  background: var(--warning);
  color: #000;
}

.btn-warning:hover {
  background: #eab308;
  box-shadow: 0 4px 20px rgba(245, 158, 11, 0.3);
}

/* Form Elements */
.select-container, .input-container {
  width: 100%;
  position: relative;
}

.select-container::before, .input-container::before {
  position: absolute;
  left: 12px;
  top: 50%;
  transform: translateY(-50%);
  font-size: 16px;
  opacity: 0.7;
  z-index: 1;
}

select, input[type="text"], input[type="password"], input[type="time"], input[type="datetime-local"], input[type="number"] {
  width: 100%;
  background: rgba(255,255,255,0.05);
  border: 1px solid var(--card-border);
  border-radius: var(--radius-md);
  padding: 12px 12px 12px 40px;
  color: var(--text-primary);
  font-size: 14px;
  transition: all 0.2s ease;
}

select:focus, input:focus {
  outline: none;
  border-color: var(--primary);
  box-shadow: 0 0 0 3px rgba(16, 185, 129, 0.1);
}

/* Preset Colors */
.preset-selector {
  display: flex;
  gap: 12px;
  align-items: center;
}

.color-indicator {
  width: 32px;
  height: 32px;
  border-radius: var(--radius-sm);
  border: 2px solid var(--card-border);
  flex-shrink: 0;
  transition: all 0.3s ease;
}

.color-indicator:hover {
  transform: scale(1.1);
  box-shadow: 0 4px 15px rgba(0,0,0,0.3);
}

/* Log Section */
.log-container {
  background: rgba(0,0,0,0.3);
  border-radius: var(--radius-md);
  padding: 16px;
  height: 300px;
  overflow-y: auto;
  font-family: 'Monaco', 'Menlo', 'Ubuntu Mono', monospace;
  font-size: 12px;
  line-height: 1.4;
}

.log-entry {
  margin-bottom: 4px;
  padding: 4px 0;
  border-bottom: 1px solid rgba(255,255,255,0.05);
  transition: background-color 0.2s ease;
}

.log-entry:hover {
  background: rgba(255,255,255,0.02);
}

.log-entry.new {
  background: rgba(16, 185, 129, 0.1);
  border-left: 3px solid var(--success);
  padding-left: 8px;
}

/* OTA Section */
.ota-section {
  background: linear-gradient(135deg, rgba(245, 158, 11, 0.1), rgba(245, 158, 11, 0.05));
  border: 1px solid rgba(245, 158, 11, 0.3);
  border-radius: var(--radius-md);
  padding: 20px;
  margin-top: 24px;
  position: relative;
  overflow: hidden;
}

.ota-section::before {
  content: '🔄';
  position: absolute;
  top: 20px;
  right: 20px;
  font-size: 24px;
  opacity: 0.3;
}

.ota-title {
  color: var(--warning);
  font-size: 16px;
  font-weight: 600;
  margin-bottom: 8px;
  display: flex;
  align-items: center;
  gap: 8px;
}

.ota-description {
  color: rgba(245, 158, 11, 0.8);
  font-size: 13px;
  margin-bottom: 16px;
}

/* Utility Classes */
.text-center { text-align: center; }
.text-muted { color: var(--text-muted); }
.mb-0 { margin-bottom: 0; }
.mt-0 { margin-top: 0; }
.mb-4 { margin-bottom: 16px; }
.mt-4 { margin-top: 16px; }
.flex { display: flex; }
.items-center { align-items: center; }
.justify-between { justify-content: space-between; }
.gap-3 { gap: 12px; }
.gap-4 { gap: 16px; }
.w-full { width: 100%; }

/* Mode-specific styling */
.mode-sunrise { color: #f59e0b; }
.mode-sunset { color: #f97316; }
.mode-moonlight { color: #60a5fa; }
.mode-cloudy { color: #9ca3af; }
.mode-rainy { color: #3b82f6; }
.mode-thunder { color: #fbbf24; }
.mode-fog { color: #d1d5db; }
.mode-rgb { color: #ec4899; }
.mode-off { color: #6b7280; }

/* Responsive */
@media (max-width: 768px) {
  .dashboard {
    grid-template-columns: 1fr;
  }
  
  .status-grid {
    grid-template-columns: repeat(2, 1fr);
  }
  
  .header {
    flex-direction: column;
    gap: 16px;
    text-align: center;
  }

  .notifications-container {
    top: 10px;
    right: 10px;
    left: 10px;
    max-width: none;
  }

  .graph-controls {
    flex-direction: column;
    align-items: stretch;
  }

  .graph-time-selector {
    justify-content: center;
  }
}
</style>
</head>
<body>
<!-- Notifications Container -->
<div id="notificationsContainer" class="notifications-container"></div>

<div class="container">
  <!-- Header -->
  <header class="header">
    <div class="logo-container">
      <div class="logo">AQ</div>
      <div class="header-content">
        <h1>Aquarium Controller</h1>
        <div class="subtitle">
          <i class="fas fa-wave-square"></i>
          Professional Aquarium Management System
        </div>
      </div>
    </div>
    <div class="header-actions">
      <button class="btn btn-secondary" onclick="showIP()" title="Show device IP on TFT">
        <i class="fas fa-network-wired"></i>
        <span>Show IP</span>
      </button>
      <button class="btn btn-secondary" onclick="relearnRF()" title="Enter RF relearn mode">
        <i class="fas fa-satellite-dish"></i>
        <span>RF Relearn</span>
      </button>
      <button class="btn" onclick="logout()">
        <i class="fas fa-sign-out-alt"></i>
        <span>Log Out</span>
      </button>
    </div>
  </header>

  <!-- Main Dashboard -->
  <div class="dashboard">
    <!-- Left Column - Controls -->
    <div class="card">
      <div class="card-header">
        <div>
          <h2 class="card-title">
            <i class="fas fa-sliders-h"></i>
            Device Controls
          </h2>
          <div class="card-subtitle">
            <i class="fas fa-bolt"></i>
            Real-time device management
          </div>
        </div>
      </div>

      <!-- Brightness Control -->
      <div class="control-group">
        <label class="control-label">
          <i class="fas fa-sun"></i>
          Brightness <span id="brightVal" class="control-value">0%</span>
        </label>
        <div class="slider-container">
          <input id="brightness" type="range" min="0" max="100" value="0" 
                 oninput="onBrightnessInput(this.value)" 
                 onchange="onBrightnessChange(this.value)">
        </div>
      </div>

      <!-- Mode Control -->
 <div class="control-group">
    <label class="control-label">
        <i class="fas fa-palette"></i>
        Lighting Mode
    </label>
    <div class="flex items-center gap-4">
        <div id="activeMode" class="status-chip" style="flex: 1; text-align: center;">-</div>
        <div class="select-container">
            <select id="modeSel" onchange="onModeChange()">
                <option value="0">🌅 Sunrise</option>
                <option value="1">🌇 Sunset</option>
                <option value="2">🌙 Moonlight</option>
                <option value="3">☁️ Cloudy</option>
                <option value="4">⚡ Thunderstorm</option>
                <option value="5">🌧️ Rainy</option>
                <option value="6">🌫️ Fog</option>
                <option value="7">🌈 RGB</option>
                <option value="8">⭕ Off</option>
            </select>
        </div>
        <button class="btn" onclick="applyMode()">
            <i class="fas fa-check"></i>
            Apply
        </button>
    </div>
</div>

      <!-- Preset Control -->
      <div class="control-group">
        <label class="control-label">
          <i class="fas fa-fill-drip"></i>
          Color Presets
        </label>
        <div class="preset-selector">
          <div class="select-container">
            <select id="presetSel"></select>
          </div>
          <button class="btn" onclick="setPreset()">
            <i class="fas fa-play"></i>
            Set Preset
          </button>
          <div id="presetColorBox" class="color-indicator" title="Current preset color"></div>
        </div>
        <div class="text-muted mt-4" style="font-size: 12px;">
          <i class="fas fa-info-circle"></i>
          Current: <span id="presetCurrent">-</span> • Select unchanged until you press Set Preset
        </div>
      </div>

      <!-- Menu Navigation -->
      <div class="control-group">
        <label class="control-label">
          <i class="fas fa-bars"></i>
          Device Menu
        </label>
        <div class="flex gap-3">
          <div class="select-container">
            <select id="menuSel">
              <option value="MAIN">📱 Main Menu</option>
              <option value="WATER">💧 Water Settings</option>
              <option value="LIGHT">💡 Light Schedule</option>
              <option value="PH">🧪 pH Calibration</option>
              <option value="FEEDER">🍽️ Feeder</option>
              <option value="CLOCK">⏰ Date & Time</option>
              <option value="WATER_CHANGE">🔄 Water Change</option>
              <option value="AUDIO">🔊 Audio</option>
            </select>
          </div>
          <button class="btn" onclick="openMenu()">
            <i class="fas fa-external-link-alt"></i>
            Open Menu
          </button>
        </div>
      </div>

      <!-- Submenu Controls -->
      <div id="submenuControls" class="submenu"></div>

      <!-- OTA Update Section -->
      <div class="ota-section">
        <div class="ota-title">
          <i class="fas fa-cloud-upload-alt"></i>
          Firmware Update
        </div>
        <div class="ota-description">
          Update device firmware wirelessly. No authentication required.
        </div>
        <button class="btn btn-warning w-full" onclick="window.open('/ota', '_blank')">
          <i class="fas fa-rocket"></i>
          Open OTA Update Portal
        </button>
      </div>
    </div>
)HTML";

const char MAIN_PAGE_TAIL[] PROGMEM = R"HTML(
    <!-- Right Column - Status & Logs -->
    <div class="card">
      <div class="card-header">
        <div>
          <h2 class="card-title">
            <i class="fas fa-heartbeat"></i>
            System Status
          </h2>
          <div class="card-subtitle">
            <i class="fas fa-chart-line"></i>
            Live monitoring & device logs
          </div>
        </div>
      </div>

      <!-- Temperature & pH Graph -->
      <div class="graph-section">
        <div class="graph-controls">
          <div class="graph-time-selector">
            <button class="time-btn active" onclick="setTimeRange('1h')">1H</button>
            <button class="time-btn" onclick="setTimeRange('6h')">6H</button>
            <button class="time-btn" onclick="setTimeRange('24h')">24H</button>
            <button class="time-btn" onclick="setTimeRange('7d')">7D</button>
          </div>
          <div class="graph-legend">
            <div class="legend-item">
              <div class="legend-color legend-temperature"></div>
              <span>Temperature (°C)</span>
            </div>
            <div class="legend-item">
              <div class="legend-color legend-ph"></div>
              <span>pH Level</span>
            </div>
          </div>
        </div>
        <div class="graph-container">
          <canvas id="tempPhChart"></canvas>
        </div>
      </div>

      <!-- Status Grid -->
      <div class="status-grid">
        <div class="status-chip" id="brightnessChip">
          <span id="chipBright" class="status-value">0%</span>
          <div class="status-label">
            <i class="fas fa-sun status-icon"></i>
            Brightness
          </div>
        </div>
        <div class="status-chip" id="modeChip">
          <span id="chipMode" class="status-value">-</span>
          <div class="status-label">
            <i class="fas fa-palette status-icon"></i>
            Mode
          </div>
        </div>
        <div class="status-chip" id="tempChip">
          <span id="chipTemp" class="status-value">-</span>
          <div class="status-label">
            <i class="fas fa-thermometer-half status-icon"></i>
            Temperature
          </div>
        </div>
        <div class="status-chip" id="phChip">
          <span id="chipPH" class="status-value">-</span>
          <div class="status-label">
            <i class="fas fa-tint status-icon"></i>
            pH Level
          </div>
        </div>
        <div class="status-chip" id="levelChip">
          <span id="chipLevel" class="status-value">-</span>
          <div class="status-label">
            <i class="fas fa-ruler-vertical status-icon"></i>
            Water Level
          </div>
        </div>
        <div class="status-chip" id="ipChip">
          <span id="chipIP" class="status-value">-</span>
          <div class="status-label">
            <i class="fas fa-wifi status-icon"></i>
            IP Address
          </div>
        </div>
      </div>
         
      <!-- Status Details -->
      <div class="control-group">
        <label class="control-label">
          <i class="fas fa-microchip"></i>
          Device Status
        </label>
        <div id="status" class="log-container" style="height: 100px; font-size: 13px;">
          <div class="log-entry new">
            <i class="fas fa-circle" style="color: var(--success); font-size: 8px; margin-right: 8px;"></i>
            Waiting for status update...
          </div>
        </div>
      </div>

      <!-- Live Logs -->
      <div class="control-group">
        <label class="control-label">
          <i class="fas fa-terminal"></i>
          Live Logs
        </label>
        <div id="log" class="log-container" style="height: 100px;">
          <div class="log-entry new">
            <i class="fas fa-circle" style="color: var(--info); font-size: 8px; margin-right: 8px;"></i>
            [WebSocket connecting...]
          </div>
        </div>
      </div>

      <!-- Raw Data -->
      <div class="control-group">
        <label class="control-label">
          <i class="fas fa-code"></i>
          Raw Data
        </label>
        <pre id="raw" class="log-container" style="height: 80px; font-size: 11px;">
Last command response will appear here...
        </pre>
      </div>
    </div>
  </div>
</div>

<script>
// Chart initialization and data management
let tempPhChart;
let chartData = {
  labels: [],
  temperature: [],
  ph: []
};
let currentTimeRange = '1h';

// Initialize the chart
function initializeChart() {
  const ctx = document.getElementById('tempPhChart').getContext('2d');
  
  tempPhChart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: chartData.labels,
      datasets: [
        {
          label: 'Temperature (°C)',
          data: chartData.temperature,
          borderColor: '#ef4444',
          backgroundColor: 'rgba(239, 68, 68, 0.1)',
          borderWidth: 2,
          fill: true,
          tension: 0.4,
          pointBackgroundColor: '#ef4444',
          pointBorderColor: '#ffffff',
          pointBorderWidth: 2,
          pointRadius: 3,
          pointHoverRadius: 6,
          yAxisID: 'y'
        },
        {
          label: 'pH Level',
          data: chartData.ph,
          borderColor: '#3b82f6',
          backgroundColor: 'rgba(59, 130, 246, 0.1)',
          borderWidth: 2,
          fill: true,
          tension: 0.4,
          pointBackgroundColor: '#3b82f6',
          pointBorderColor: '#ffffff',
          pointBorderWidth: 2,
          pointRadius: 3,
          pointHoverRadius: 6,
          yAxisID: 'y1'
        }
      ]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      interaction: {
        mode: 'index',
        intersect: false,
      },
      plugins: {
        legend: {
          display: false
        },
        tooltip: {
          backgroundColor: 'rgba(7, 16, 33, 0.9)',
          titleColor: '#eaf3f6',
          bodyColor: '#9aa6b2',
          borderColor: 'rgba(255,255,255,0.1)',
          borderWidth: 1,
          cornerRadius: 8,
          displayColors: true,
          callbacks: {
            label: function(context) {
              let label = context.dataset.label || '';
              if (label) {
                label += ': ';
              }
              if (context.parsed.y !== null) {
                label += context.parsed.y.toFixed(2);
              }
              return label;
            }
          }
        }
      },
      scales: {
        x: {
          grid: {
            color: 'rgba(255,255,255,0.05)',
            borderColor: 'rgba(255,255,255,0.1)'
          },
          ticks: {
            color: '#9aa6b2',
            font: {
              size: 10
            },
            maxRotation: 0
          }
        },
        y: {
          type: 'linear',
          display: true,
          position: 'left',
          grid: {
            color: 'rgba(255,255,255,0.05)'
          },
          ticks: {
            color: '#ef4444',
            font: {
              size: 10
            }
          },
          title: {
            display: true,
            text: 'Temperature (°C)',
            color: '#ef4444',
            font: {
              size: 11
            }
          },
          suggestedMin: 20,
          suggestedMax: 30
        },
        y1: {
          type: 'linear',
          display: true,
          position: 'right',
          grid: {
            drawOnChartArea: false,
          },
          ticks: {
            color: '#3b82f6',
            font: {
              size: 10
            }
          },
          title: {
            display: true,
            text: 'pH Level',
            color: '#3b82f6',
            font: {
              size: 11
            }
          },
          suggestedMin: 6,
          suggestedMax: 8
        }
      },
      animation: {
        duration: 1000,
        easing: 'easeOutQuart'
      }
    }
  });
}

// Set time range for chart
function setTimeRange(range) {
  currentTimeRange = range;
  
  // Update active button
  document.querySelectorAll('.time-btn').forEach(btn => {
    btn.classList.remove('active');
  });
  event.target.classList.add('active');
  
  updateChartDisplay();
}

// Update chart with new data
function updateChartData(temperature, ph) {
  const now = new Date();
  const timeLabel = now.toLocaleTimeString('en-US', { 
    hour12: false, 
    hour: '2-digit', 
    minute: '2-digit' 
  });
  
  // Add new data point
  chartData.labels.push(timeLabel);
  chartData.temperature.push(temperature);
  chartData.ph.push(ph);
  
  // Keep only the last 20 data points for performance
  const maxPoints = 20;
  if (chartData.labels.length > maxPoints) {
    chartData.labels = chartData.labels.slice(-maxPoints);
    chartData.temperature = chartData.temperature.slice(-maxPoints);
    chartData.ph = chartData.ph.slice(-maxPoints);
  }
  
  // Update chart
  if (tempPhChart) {
    tempPhChart.update();
  }
}

// Update chart display based on time range
function updateChartDisplay() {
  if (tempPhChart) {
    tempPhChart.update();
  }
}

// Generate demo data for initial display
function generateDemoData() {
  const now = new Date();
  const baseTemp = 25.0;
  const basePH = 7.2;
  
  for (let i = 10; i >= 0; i--) {
    const time = new Date(now.getTime() - i * 5 * 60000);
    const timeLabel = time.toLocaleTimeString('en-US', { 
      hour12: false, 
      hour: '2-digit', 
      minute: '2-digit' 
    });
    
    // Add some realistic variation
    const temp = baseTemp + (Math.random() - 0.5) * 0.8;
    const ph = basePH + (Math.random() - 0.5) * 0.2;
    
    chartData.labels.push(timeLabel);
    chartData.temperature.push(parseFloat(temp.toFixed(1)));
    chartData.ph.push(parseFloat(ph.toFixed(2)));
  }
}
)HTML";

const char MAIN_PAGE_SCRIPT[] PROGMEM = R"HTML(
// Notification System
class NotificationManager {
  constructor() {
    this.container = document.getElementById('notificationsContainer');
    this.notifications = new Set();
  }

  show(title, message, type = 'info', duration = 5000) {
    const notification = document.createElement('div');
    notification.className = `notification ${type}`;
    
    const icons = {
      success: '✅',
      warning: '⚠️',
      error: '❌',
      info: 'ℹ️'
    };

    notification.innerHTML = `
      <div class="notification-icon">${icons[type] || icons.info}</div>
      <div class="notification-content">
        <div class="notification-title">${title}</div>
        <div class="notification-message">${message}</div>
      </div>
      <button class="notification-close" onclick="notificationManager.close(this.parentElement)">
        <i class="fas fa-times"></i>
      </button>
    `;

    this.container.appendChild(notification);
    this.notifications.add(notification);

    setTimeout(() => notification.classList.add('show'), 10);

    if (duration > 0) {
      setTimeout(() => this.close(notification), duration);
    }

    return notification;
  }

  close(notification) {
    if (this.notifications.has(notification)) {
      notification.classList.remove('show');
      notification.classList.add('hide');
      
      setTimeout(() => {
        if (notification.parentElement) {
          notification.parentElement.removeChild(notification);
        }
        this.notifications.delete(notification);
      }, 300);
    }
  }

  clearAll() {
    this.notifications.forEach(notification => this.close(notification));
  }
}

const notificationManager = new NotificationManager();

// State tracking for change detection
let previousState = {
  BRIGHT: '0',
  MODE: '-',
  TEMP: '-',
  PH: '-',
  LEVEL: '-',
  RGB_PRESET: '-'
};

// FIXED: Use the same status parsing technique from 1st version
async function refreshStatus(){
  try {
    const headers = {}; 
    const tk = getToken(); 
    if (tk) headers['Authorization'] = 'Bearer ' + tk;
    
    const r = await fetch('/api/status', { 
      method:'GET', 
      credentials:'same-origin', 
      headers 
    });
    
    if (r.status === 200){
      const txt = await r.text(); 
      document.getElementById('raw').innerText = txt; 
      document.getElementById('status').innerText = txt;
      const parts = txt.split(','); 
      const currentState = {};
      
      parts.forEach(p => { 
        const idx = p.indexOf(':'); 
        if (idx>0) { 
          const k = p.substring(0,idx).trim(); 
          const v = p.substring(idx+1).trim(); 
          currentState[k]=v; 
        } 
      });

      // Check for changes and update UI
      checkParameterChanges(currentState);

      // Update chart with new temperature and pH data if available
      if (currentState.TEMP && currentState.PH) {
        const temp = parseFloat(currentState.TEMP);
        const ph = parseFloat(currentState.PH);
        if (!isNaN(temp) && !isNaN(ph)) {
          updateChartData(temp, ph);
        }
      }
       
      updateStatusUI(currentState);
      previousState = { ...currentState };

    } else if (r.status === 401){
      localStorage.removeItem('session_token'); 
      document.getElementById('raw').innerText = '[401] Unauthorized';
    }
  } catch(e){ 
    document.getElementById('raw').innerText = 'Status error: '+e.message; 
  }
}

// FIXED: Use the same UI update technique from 1st version
function updateStatusUI(state) {
    // Brightness
    if (state.BRIGHT) { 
        const bv = parseInt(state.BRIGHT) || 0; 
        document.getElementById('brightVal').innerText = bv + '%'; 
        document.getElementById('chipBright').innerText = bv + '%'; 
        document.getElementById('brightness').value = bv; 
    }
  
  // Mode - FIXED: Use same technique as 1st version - properly handle numeric mode values
  if (state.MODE){ 
    const modeNumber = parseInt(state.MODE);
    const modeName = getModeName(modeNumber);
    
    document.getElementById('chipMode').innerText = modeName; 
    document.getElementById('activeMode').innerText = modeName;
    
    // Update dropdown to match current mode
    const modeSelect = document.getElementById('modeSel');
    if (modeSelect && !isNaN(modeNumber) && modeNumber >= 0 && modeNumber <= 8) {
      modeSelect.value = modeNumber.toString();
    }
    
    // Apply mode styling
    const modeElement = document.getElementById('chipMode');
    const activeModeElement = document.getElementById('activeMode');
    
    modeElement.className = 'status-value';
    activeModeElement.className = '';
    
    const modeClass = `mode-${modeName.toLowerCase()}`;
    modeElement.classList.add(modeClass);
    activeModeElement.classList.add(modeClass);
  }
  
  // Other status values
  if (state.TEMP) document.getElementById('chipTemp').innerText = state.TEMP;
  if (state.PH) document.getElementById('chipPH').innerText = state.PH;
  if (state.LEVEL) document.getElementById('chipLevel').innerText = state.LEVEL;

  // IP address
  const ipAddress = state.WIFI_IP || state.IP || state.IP_ADDRESS || window.location.hostname;
  if (ipAddress && ipAddress !== 'undefined') {
    document.getElementById('chipIP').innerText = ipAddress;
  }

  // Preset
  if (state.RGB_PRESET){
    const idx = parseInt(state.RGB_PRESET);
    if(!isNaN(idx)) updatePresetDisplayFromIndex(idx);
  }
 // Water settings
  if (state.REFILL_START) window._refillStart = state.REFILL_START;
  if (state.REFILL_STOP) window._refillStop = state.REFILL_STOP;
  if (state.DRAIN_LEVEL) window._drainLevel = state.DRAIN_LEVEL;


  // Feed status
  const last = state.LAST_FEED || state.LASTFEED || '-'; 
  const next = state.NEXT_FEED || state.NEXTFEED || '-';
  if (document.getElementById('lastNextFeed')) {
    document.getElementById('lastNextFeed').innerText = 'Last feed: ' + last + '  ·  Next feed: ' + next;
  }

  if (state.SUNRISE_START) window._sunriseStart = state.SUNRISE_START;
  if (state.SUNRISE_END)   window._sunriseEnd = state.SUNRISE_END;
  if (state.SUNSET_START)  window._sunsetStart = state.SUNSET_START;
  if (state.SUNSET_END)    window._sunsetEnd = state.SUNSET_END;
  if (state.MOON_START)    window._moonStart = state.MOON_START;
  if (state.MOON_END)      window._moonEnd = state.MOON_END;
  if (typeof state.AUTO_SUN !== 'undefined') {
    window._autoSun = (state.AUTO_SUN === '1' || state.AUTO_SUN === 'ON');
  }
}

function checkParameterChanges(currentState) {
  const parameters = [
    { key: 'BRIGHT', name: 'Brightness', chip: 'brightnessChip', unit: '%' },
    { key: 'MODE', name: 'Lighting Mode', chip: 'modeChip', unit: '' },
    { key: 'TEMP', name: 'Temperature', chip: 'tempChip', unit: '°C' },
    { key: 'PH', name: 'pH Level', chip: 'phChip', unit: '' },
    { key: 'LEVEL', name: 'Water Level', chip: 'levelChip', unit: 'cm' },
    { key: 'RGB_PRESET', name: 'Color Preset', chip: null, unit: '' }
  ];

  parameters.forEach(param => {
    const currentValue = currentState[param.key];
    const previousValue = previousState[param.key];

    if (currentValue !== undefined && currentValue !== previousValue && previousValue !== undefined) {
      const displayValue = param.unit ? `${currentValue}${param.unit}` : currentValue;
      const displayName = param.name === 'RGB_PRESET' ? 'Color Preset' : param.name;
      
      notificationManager.show(
        `${displayName} Updated`,
        `Changed from ${previousValue} to ${displayValue}`,
        'info',
        4000
      );

      if (param.chip) {
        const chip = document.getElementById(param.chip);
        chip.classList.add('changed');
        setTimeout(() => chip.classList.remove('changed'), 2000);
      }
    }
  });

  updateStatusUI(currentState);
}
// FIXED: Use the same WebSocket technique from 1st version
function startWS(){
  const logEl=document.getElementById('log'); 
  const protocol=(location.protocol==='https:')?'wss':'ws';
  
  try {
    const ws = new WebSocket(protocol+'://'+location.hostname+':81');
    
    ws.onopen = () => { 
      const entry = document.createElement('div');
      entry.className = 'log-entry new';
      entry.innerHTML = '<i class="fas fa-circle" style="color: var(--success); font-size: 8px; margin-right: 8px;"></i>[WebSocket connected]';
      logEl.appendChild(entry);
      logEl.scrollTop = logEl.scrollHeight;
      notificationManager.show(
        'Connection Established',
        'WebSocket connected successfully',
        'success',
        3000
      );
    };
    
    ws.onmessage = (e) => { 
      const message = e.data.trim();
      const entry = document.createElement('div');
      entry.className = 'log-entry new';
      entry.innerHTML = `<i class="fas fa-circle" style="color: var(--info); font-size: 8px; margin-right: 8px;"></i>${message}`;
      logEl.appendChild(entry);
      logEl.scrollTop = logEl.scrollHeight;
      
      
      if (message.includes('FEED_NOW') || message.includes('FEEDING')) {
        notificationManager.show(
          'Feeder Activated',
          'Food dispenser is running',
          'info',
          3000
        );
      } else if (message.includes('WATER_CHANGE') || message.includes('DRAIN') || message.includes('REFILL')) {
        notificationManager.show(
          'Water Change',
          'Water change process updated',
          'info',
          3000
        );
      } else if (message.includes('ERROR') || message.includes('FAIL')) {
        notificationManager.show(
          'System Alert',
          message,
          'error',
          5000
        );
      }
      // Update IP if received via WebSocket
      try {
        if (message.startsWith('WIFI_IP:') || message.startsWith('IP:')){ 
          const ip = message.split(':')[1]; 
          if(ip) document.getElementById('chipIP').innerText = ip.trim(); 
        }
      } catch(ex){}
    };
    
    ws.onclose = () => {
      const entry = document.createElement('div');
      entry.className = 'log-entry new';
      entry.innerHTML = '<i class="fas fa-circle" style="color: var(--warning); font-size: 8px; margin-right: 8px;"></i>[WebSocket disconnected - reconnecting...]';
      logEl.appendChild(entry);
      logEl.scrollTop = logEl.scrollHeight;

      notificationManager.show(
        'Connection Lost',
        'WebSocket disconnected. Reconnecting...',
        'warning',
        3000
      );
      
      setTimeout(startWS, 3000);
    };

    ws.onerror = (error) => {
      const entry = document.createElement('div');
      entry.className = 'log-entry new';
      entry.innerHTML = '<i class="fas fa-circle" style="color: var(--error); font-size: 8px; margin-right: 8px;"></i>[WebSocket error]';
      logEl.appendChild(entry);
      logEl.scrollTop = logEl.scrollHeight;
    };
    
  } catch(e) {
    const entry = document.createElement('div');
    entry.className = 'log-entry new';
    entry.innerHTML = `<i class="fas fa-circle" style="color: var(--error); font-size: 8px; margin-right: 8px;"></i>[WebSocket connection failed: ${e.message}]`;
    logEl.appendChild(entry);
    logEl.scrollTop = logEl.scrollHeight;
    
    setTimeout(startWS, 5000);
  }
}

// FIXED: Use the same mode functions from 1st version that work with processTextCommand
function getModeName(modeNumber) {
  const modes = ["SUNRISE", "SUNSET", "MOONLIGHT", "CLOUDY", "THUNDER", "RAINY", "FOG", "RGB", "OFF"];
  return modes[modeNumber] || "UNKNOWN";
}

// FIXED: Apply mode - sends numeric value that Mega expects (same as 1st version)
async function applyMode() {
  const modeSelect = document.getElementById('modeSel');
  const modeNumber = modeSelect.value;
  const modeName = modeSelect.options[modeSelect.selectedIndex].text.replace(/^[^a-zA-Z]*/, '');
  
  const result = await sendCmd('SET_MODE:' + modeNumber);
  
  if (result.ok) {
    document.getElementById('chipMode').innerText = modeName;
    document.getElementById('activeMode').innerText = modeName;
    
    notificationManager.show(
      'Lighting Mode Changed',
      `Changed to ${modeName}`,
      'info',
      3000
    );
  }
}

// FIXED: Working command function (same as 1st version)
async function sendCmd(cmd){
  try {
    const body = new URLSearchParams(); 
    body.append('cmd', cmd);
    
    const headers = {}; 
    const tk = getToken(); 
    if (tk) headers['Authorization'] = 'Bearer ' + tk;
    
    const r = await fetch('/api/cmd', { 
      method:'POST', 
      body, 
      credentials:'same-origin', 
      headers 
    });
    
    const txt = await r.text();
    
    if (r.status === 200) {
      document.getElementById('raw').innerText = txt;
      if (cmd.includes('FEED_NOW')) {
        notificationManager.show(
          'Feeding Started',
          'Food dispenser activated',
          'success',
          2000
        );
      } else if (cmd.includes('START_WC')) {
        notificationManager.show(
          'Water Change Started',
          'Water change process initiated',
          'info',
          3000
        );
      }
      return { ok: true, text: txt, status: r.status };
    } else if (r.status === 401) { 
      document.getElementById('raw').innerText = '[401] ' + txt; 
      localStorage.removeItem('session_token'); 
      notificationManager.show(
        'Authentication Error',
        'Please log in again',
        'error',
        5000
      );
      return { ok: false, text: txt, status: r.status };
    } else { 
      document.getElementById('raw').innerText = '['+r.status+'] '+txt; 
      return { ok: false, text: txt, status: r.status };
    }
  } catch(e){
    document.getElementById('raw').innerText = 'Network error: ' + e.message;
    notificationManager.show(
      'Network Error',
      'Failed to send command',
      'error',
      5000
    );
    return { ok: false, text: String(e), status: 0 };
  }
}

/* ------------------ Basic Functions ------------------ */
function getToken(){ 
  const t = localStorage.getItem('session_token'); 
  if (t && t.length) return t; 
  const cookies = document.cookie.split(';').map(s=>s.trim()); 
  for (let c of cookies) if (c.startsWith('session=')) return c.substring(8); 
  return null; 
}

// Remove token capture from URL if present
(function captureTokenFromUrl(){ 
  try { 
    const params=new URLSearchParams(window.location.search);
    if(params.has('token')){ 
      const tk=params.get('token'); 
      if(tk && tk.length>10){ 
        localStorage.setItem('session_token', tk); 
        params.delete('token'); 
        history.replaceState(null,'', window.location.pathname + (params.toString()?('?'+params.toString()):'')); 
      } 
    } 
  } catch(e){} 
})();

/* ------------------ Presets ------------------ */
const presetNames=["Red","Green","Blue","White","Purple","Yellow","Cyan","Magenta","Orange","Pink","Rose Pink","Forest Green","Aquamarine","Tomato Red","Deep Sky Blue","Gold","Off"];
const presetColors=["#FF0000","#00FF00","#0000FF","#FFFFFF","#800080","#FFFF00","#00FFFF","#FF00FF","#FFA500","#FF70FA","#FF70FA","#16C940","#69FFBE","#E01600","#00BFC","#FFD700","#000000"];

function populatePresets(){ 
  const s=document.getElementById('presetSel'); 
  s.innerHTML=''; 
  for(let i=0;i<presetNames.length;i++){ 
    const o=document.createElement('option'); 
    o.value=i; 
    o.text = '🎨 ' + i + ' - ' + presetNames[i]; 
    s.appendChild(o);
  } 
}

function setPresetColorIndicator(idx){ 
  const box=document.getElementById('presetColorBox'); 
  box.style.background = (idx>=0 && idx<presetColors.length) ? presetColors[idx] : 'transparent'; 
}

function updatePresetDisplayFromIndex(idx){ 
  document.getElementById('presetCurrent').innerText = (idx>=0 && idx<presetNames.length) ? presetNames[idx] : 'Unknown'; 
  setPresetColorIndicator(idx); 
}

function setPreset(){ 
  const idx = Number(document.getElementById('presetSel').value||0); 
  const presetName = presetNames[idx] || 'Unknown';
  sendCmd('SET_PRESET:' + idx).then(()=>{ 
    updatePresetDisplayFromIndex(idx); 
    notificationManager.show(
      'Color Preset Applied',
      `Set to ${presetName}`,
      'info',
      3000
    );
  }); 
}

/* ------------------ UI Actions ------------------ */
function onBrightnessInput(v) {
    document.getElementById('brightVal').innerText = v + '%';
    document.getElementById('chipBright').innerText = v + '%';
}

function onBrightnessChange(v) {
    document.getElementById('brightVal').innerText = v + '%';
    document.getElementById('chipBright').innerText = v + '%';
    sendCmd('SET_BRIGHTNESS:' + v).then(response => {
        if (response.ok) {
            notificationManager.show(
                'Brightness Updated',
                `Set to ${v}%`,
                'info',
                2000
            );
        }
    });
}

function showIP(){
  sendCmd('SHOW_IP').then(res=>{
    if (res.ok) {
      document.getElementById('raw').innerText = 'Request sent: SHOW_IP';
      notificationManager.show(
        'IP Display',
        'IP address shown on device display',
        'info',
        3000
      );
    } else {
      document.getElementById('raw').innerText = 'SHOW_IP failed: ' + res.text;
    }
  });
}

function relearnRF(){
  if (!confirm('Enter RF relearn mode on Mega?')) return;
  sendCmd('RELEARN_RF').then(res=>{
    if (res.ok) {
      document.getElementById('raw').innerText = 'RELEARN_RF sent';
      notificationManager.show(
        'RF Relearn Mode',
        'Entering RF relearn mode',
        'warning',
        5000
      );
    } else {
      document.getElementById('raw').innerText = 'RELEARN_RF failed: ' + res.text;
    }
  });
}

function logout(){ 
  localStorage.removeItem('session_token'); 
  document.cookie='session=;path=/;max-age=0'; 
  fetch('/logout',{ method:'POST', credentials:'same-origin' }).finally(()=>location='/'); 
}

// Keep all the enhanced submenu functionality from 2nd version
/* ------------------ submenu UI (full) ------------------ */
function openMenu(){ const m=document.getElementById('menuSel').value; showSubmenuUI(m); sendCmd('OPEN_MENU:'+m); }

function showSubmenuUI(menu){
  const div = document.getElementById('submenuControls'); div.innerHTML = '';
  const addBtn = (label,fn, icon = 'fas fa-cog') => { 
    const b = document.createElement('button'); 
    b.className='btn'; 
    b.style.margin='6px 6px 6px 0'; 
    b.innerHTML = `<i class="${icon}"></i> ${label}`;
    b.onclick=fn; 
    div.appendChild(b); 
  };
   const addText = (s) => { const t = document.createElement('div'); t.className='text-muted'; t.style.marginTop='6px'; t.innerHTML = s; div.appendChild(t); };

  // ===== MAIN =====
  if (menu === 'MAIN') {
    addText('Device main menu displayed on device. Use device to navigate or open submenus below.');
    addBtn('Show Device Main Menu', ()=>sendCmd('OPEN_MENU:MAIN'), 'fas fa-home');
    return;
  }
  
  // ===== WATER SETTINGS =====
  if (menu === 'WATER'){
    const wrap = document.createElement('div');
    wrap.innerHTML = '<div class="control-label"><i class="fas fa-tint"></i> Water Settings — Edit refill/drain levels (cm)</div>';
    
    // start refill
    wrap.appendChild(document.createTextNode('Start Refill (cm)'));
    const sStart = document.createElement('input'); sStart.type='range'; sStart.min=0; sStart.max=50; sStart.value=Number(window._refillStart||10); sStart.style.display='block'; sStart.className='w-full';
    const sStartVal = document.createElement('div'); sStartVal.className='text-muted'; sStartVal.innerText = sStart.value + ' cm';
    sStart.oninput = ()=> sStartVal.innerText = sStart.value + ' cm';
    wrap.appendChild(sStart); wrap.appendChild(sStartVal);

    // stop refill
    wrap.appendChild(document.createTextNode('Stop Refill (cm)'));
    const sStop = document.createElement('input'); sStop.type='range'; sStop.min=0; sStop.max=50; sStop.value=Number(window._refillStop||30); sStop.style.display='block'; sStop.className='w-full';
    const sStopVal = document.createElement('div'); sStopVal.className='text-muted'; sStopVal.innerText = sStop.value + ' cm';
    sStop.oninput = ()=> sStopVal.innerText = sStop.value + ' cm';
    wrap.appendChild(sStop); wrap.appendChild(sStopVal);

    // drain level
    wrap.appendChild(document.createTextNode('Drain Level (cm)'));
    const sDrain = document.createElement('input'); sDrain.type='range'; sDrain.min=0; sDrain.max=50; sDrain.value=Number(window._drainLevel||5); sDrain.style.display='block'; sDrain.className='w-full';
    const sDrainVal = document.createElement('div'); sDrainVal.className='text-muted'; sDrainVal.innerText = sDrain.value + ' cm';
    sDrain.oninput = ()=> sDrainVal.innerText = sDrain.value + ' cm';
    wrap.appendChild(sDrain); wrap.appendChild(sDrainVal);

    const save = document.createElement('button'); save.className='btn'; save.style.marginTop='12px'; save.innerHTML = '<i class="fas fa-save"></i> Save Water Settings';
    save.onclick = ()=>{
      const a = sStart.value; const b = sStop.value; const c = sDrain.value;
      sendCmd('SET_REFILL_START:' + a);
      sendCmd('SET_REFILL_STOP:' + b);
      sendCmd('SET_DRAIN_LEVEL:' + c);
      window._refillStart = a; window._refillStop = b; window._drainLevel = c;
      addText('Water settings saved successfully.');
      notificationManager.show(
        'Water Settings Saved',
        'Refill and drain levels updated',
        'success',
        3000
      );
    };
    wrap.appendChild(save);
    div.appendChild(wrap);
    return;
  }

    // ===== LIGHT =====
if (menu === 'LIGHT'){
  const wrap = document.createElement('div');
  wrap.innerHTML = '<div class="control-label"><i class="fas fa-lightbulb"></i> Light Schedule — edit sunrise, sunset and moonlight times</div>';

  // Sunrise start/stop
  const srLabel = document.createElement('div'); 
  srLabel.className='text-muted'; 
  srLabel.style.marginTop='12px'; 
  srLabel.innerHTML = '<i class="fas fa-sunrise" style="margin-right: 6px;"></i>Sunrise Start / Stop';
  wrap.appendChild(srLabel);
  const srRow = document.createElement('div'); 
  srRow.className='flex gap-3';
  const srStart = document.createElement('input'); 
  srStart.type='time'; 
  srStart.id='sunrise_start'; 
  srStart.className='w-full';
  const srEnd = document.createElement('input'); 
  srEnd.type='time'; 
  srEnd.id='sunrise_end'; 
  srEnd.className='w-full';
  srRow.appendChild(srStart); 
  srRow.appendChild(srEnd);
  wrap.appendChild(srRow);

  // Sunset start/stop
  const ssLabel = document.createElement('div'); 
  ssLabel.className='text-muted'; 
  ssLabel.style.marginTop='12px'; 
  ssLabel.innerHTML = '<i class="fas fa-sunset" style="margin-right: 6px;"></i>Sunset Start / Stop';
  wrap.appendChild(ssLabel);
  const ssRow = document.createElement('div'); 
  ssRow.className='flex gap-3';
  const ssStart = document.createElement('input'); 
  ssStart.type='time'; 
  ssStart.id='sunset_start'; 
  ssStart.className='w-full';
  const ssEnd = document.createElement('input'); 
  ssEnd.type='time'; 
  ssEnd.id='sunset_end'; 
  ssEnd.className='w-full';
  ssRow.appendChild(ssStart); 
  ssRow.appendChild(ssEnd);
  wrap.appendChild(ssRow);

  // Moonlight start/stop
  const moLabel = document.createElement('div'); 
  moLabel.className='text-muted'; 
  moLabel.style.marginTop='12px'; 
  moLabel.innerHTML = '<i class="fas fa-moon" style="margin-right: 6px;"></i>Moonlight Start / Stop';
  wrap.appendChild(moLabel);
  const moRow = document.createElement('div'); 
  moRow.className='flex gap-3';
  const moStart = document.createElement('input'); 
  moStart.type='time'; 
  moStart.id='moon_start'; 
  moStart.className='w-full';
  const moEnd = document.createElement('input'); 
  moEnd.type='time'; 
  moEnd.id='moon_end'; 
  moEnd.className='w-full';
  moRow.appendChild(moStart); 
  moRow.appendChild(moEnd);
  wrap.appendChild(moRow);

  // Auto Sun toggle
  const autoRow = document.createElement('div'); 
  autoRow.style.marginTop='16px'; 
  autoRow.className='flex items-center';
  const autoToggle = document.createElement('input'); 
  autoToggle.type='checkbox'; 
  autoToggle.id='auto_sun'; 
  autoToggle.style.marginRight='8px';
  const autoLabel = document.createElement('label'); 
  autoLabel.className='text-muted'; 
  autoLabel.innerHTML = '<i class="fas fa-robot" style="margin-right: 6px;"></i>Auto Sun (on/off)';
  autoRow.appendChild(autoToggle); 
  autoRow.appendChild(autoLabel);
  wrap.appendChild(autoRow);

  // Load previously fetched values
  setTimeout(()=> {
    if (window._sunriseStart) srStart.value = window._sunriseStart;
    if (window._sunriseEnd)   srEnd.value = window._sunriseEnd;
    if (window._sunsetStart)  ssStart.value = window._sunsetStart;
    if (window._sunsetEnd)    ssEnd.value = window._sunsetEnd;
    if (window._moonStart)    moStart.value = window._moonStart;
    if (window._moonEnd)      moEnd.value = window._moonEnd;
    if (typeof window._autoSun !== 'undefined') autoToggle.checked = !!window._autoSun;
  }, 50);

  const save = document.createElement('button'); 
  save.className='btn'; 
  save.style.marginTop='16px'; 
  save.innerHTML = '<i class="fas fa-save"></i> Save Light Schedule';
  save.onclick = ()=>{
    const s1 = document.getElementById('sunrise_start').value || '';
    const s2 = document.getElementById('sunrise_end').value || '';
    const t1 = document.getElementById('sunset_start').value || '';
    const t2 = document.getElementById('sunset_end').value || '';
    const m1 = document.getElementById('moon_start').value || '';
    const m2 = document.getElementById('moon_end').value || '';
    const autos = document.getElementById('auto_sun').checked ? 'ON' : 'OFF';

    if (s1) { sendCmd('SET_SUNRISE_START:' + s1); window._sunriseStart = s1; }
    if (s2) { sendCmd('SET_SUNRISE_STOP:' + s2); window._sunriseEnd = s2; }
    if (t1) { sendCmd('SET_SUNSET_START:' + t1); window._sunsetStart = t1; }
    if (t2) { sendCmd('SET_SUNSET_STOP:' + t2); window._sunsetEnd = t2; }
    if (m1) { sendCmd('SET_MOON_START:' + m1); window._moonStart = m1; }
    if (m2) { sendCmd('SET_MOON_STOP:' + m2); window._moonEnd = m2; }
    sendCmd('SET_AUTO_SUN:' + autos); window._autoSun = (autos === 'ON');

    addText('Light schedule & Auto Sun setting saved.');
    notificationManager.show(
      'Light Schedule Updated',
      'Lighting times and auto-sun setting saved',
      'success',
      3000
    );
  };
  wrap.appendChild(save);
  div.appendChild(wrap);
  return;
}

// ===== PH =====
if (menu === 'PH'){
  addText('<i class="fas fa-vial"></i> pH Calibration & Read');
  
  addBtn('Read pH', ()=> {
    sendCmd('GET_PH').then(response => {
      if (response.ok) {
        notificationManager.show(
          'pH Reading',
          'Current pH value retrieved',
          'info',
          3000
        );
      }
    });
  }, 'fas fa-eye');
  
  addBtn('pH Status', ()=> {
    sendCmd('GET_PH_STATUS').then(response => {
      if (response.ok) {
        notificationManager.show(
          'pH Status',
          'pH sensor status retrieved',
          'info',
          3000
        );
      }
    });
  }, 'fas fa-info-circle');
  
  addBtn('Calibrate 7.0', ()=> {
    if (confirm('Start pH calibration at 7.0?')) {
      sendCmd('PH_CAL_START:0').then(response => {
        if (response.ok) {
          notificationManager.show(
            'pH Calibration Started',
            'Calibrating at pH 7.0 - follow device instructions',
            'warning',
            5000
          );
        }
      });
    }
  }, 'fas fa-sync-alt');
  
  addBtn('Calibrate 4.0', ()=> {
    if (confirm('Start pH calibration at 4.0?')) {
      sendCmd('PH_CAL_START:1').then(response => {
        if (response.ok) {
          notificationManager.show(
            'pH Calibration Started',
            'Calibrating at pH 4.0 - follow device instructions',
            'warning',
            5000
          );
        }
      });
    }
  }, 'fas fa-sync-alt');
  
  addBtn('Calibrate 10.0', ()=> {
    if (confirm('Start pH calibration at 10.0?')) {
      sendCmd('PH_CAL_START:2').then(response => {
        if (response.ok) {
          notificationManager.show(
            'pH Calibration Started',
            'Calibrating at pH 10.0 - follow device instructions',
            'warning',
            5000
          );
        }
      });
    }
  }, 'fas fa-sync-alt');
  return;
}

// ===== WATER_CHANGE =====
if (menu === 'WATER_CHANGE'){
  addText('<i class="fas fa-exchange-alt"></i> Water Change control');
  addBtn('Start Water Change Now', ()=> {
    if (confirm('Start water change process?')) {
      sendCmd('START_WC').then(response => {
        if (response.ok) {
          notificationManager.show(
            'Water Change Started',
            'Water change process initiated',
            'info',
            4000
          );
        }
      });
    }
  }, 'fas fa-play');
  
  addBtn('Stop Water Change', ()=> {
    if (confirm('Stop water change process?')) {
      sendCmd('STOP_WC').then(response => {
        if (response.ok) {
          notificationManager.show(
            'Water Change Stopped',
            'Water change process terminated',
            'warning',
            3000
          );
        }
      });
    }
  }, 'fas fa-stop');
  
  addText('<i class="fas fa-clock"></i> Scheduler: pick date/time and repeat (days)');
  const dt = document.createElement('input'); 
  dt.type='datetime-local'; 
  dt.style.display='block'; 
  dt.style.marginTop='8px'; 
  dt.id='wc_dt'; 
  dt.className='w-full';
  div.appendChild(dt);
  
  const repRow = document.createElement('div');
  repRow.style.marginTop='8px';
  repRow.style.display='flex';
  repRow.style.alignItems='center';
  repRow.style.gap='8px';
  
  repRow.appendChild(document.createTextNode('Repeat (days):'));
  const rep = document.createElement('input'); 
  rep.type='number'; 
  rep.min=1; 
  rep.max=365; 
  rep.value=1; 
  rep.style.width='80px'; 
  rep.id='wc_repeat'; 
  rep.className='w-full';
  repRow.appendChild(rep);
  div.appendChild(repRow);
  
  addBtn('Save WC Schedule', ()=>{
    const v = document.getElementById('wc_dt').value; 
    const r = document.getElementById('wc_repeat').value;
    if (!v){ 
      notificationManager.show(
        'Schedule Error',
        'Please select date and time',
        'error',
        3000
      );
      return; 
    }
    sendCmd('SET_WC_DATETIME:' + v);
    const time = v.split('T')[1] || '';
    if (time) sendCmd('SET_WC_SCHEDULE:' + time.substring(0,5) + ',' + r);
    addText('Water change schedule saved.');
    notificationManager.show(
      'Water Change Scheduled',
      `Scheduled for ${v} repeating every ${r} days`,
      'success',
      4000
    );
  }, 'fas fa-calendar-check');
  return;
}

// ===== FEEDER =====
if (menu === 'FEEDER'){
  const wrap = document.createElement('div');
  wrap.innerHTML = '<div class="control-label"><i class="fas fa-utensils"></i> Feeder — Manual & Schedule</div>';

  // manual feed qty
  const qtyRow = document.createElement('div'); 
  qtyRow.style.marginTop='12px';
  qtyRow.style.display='flex';
  qtyRow.style.alignItems='center';
  qtyRow.style.gap='8px';
  
  qtyRow.appendChild(document.createTextNode('Feed Quantity (secs, 0-15):'));
  const qty = document.createElement('input'); 
  qty.type='number'; 
  qty.min=0; 
  qty.max=15; 
  qty.value=(window._feedQty||3); 
  qty.style.width='80px'; 
  qty.id='feed_qty';
  qtyRow.appendChild(qty);
  
  const feedNow = document.createElement('button'); 
  feedNow.className='btn'; 
  feedNow.innerHTML = '<i class="fas fa-play"></i> Feed Now';
  feedNow.onclick = ()=>{ 
    const v = document.getElementById('feed_qty').value; 
    sendCmd('FEED_NOW:'+v); 
    window._feedQty = v; 
    addText('Feed command sent.'); 
    notificationManager.show(
      'Feeding Started',
      `Dispensing food for ${v} seconds`,
      'info',
      2000
    );
  };
  qtyRow.appendChild(feedNow);
  wrap.appendChild(qtyRow);

  // last/next feed display
  const lastNext = document.createElement('div'); 
  lastNext.className='text-muted'; 
  lastNext.id='lastNextFeed'; 
  lastNext.style.marginTop='8px';
  lastNext.innerHTML = '<i class="fas fa-history" style="margin-right: 6px;"></i>Last feed: -  ·  Next feed: -';
  wrap.appendChild(lastNext);

  // schedule editor: up to 6 times
  wrap.appendChild(document.createElement('br'));
  wrap.appendChild(document.createTextNode('Feed schedule (up to 6 times):'));
  const ft = document.createElement('div'); 
  ft.style.display='grid'; 
  ft.style.gridTemplateColumns='repeat(2, 1fr)'; 
  ft.style.gap='8px'; 
  ft.style.marginTop='8px';
  
  for (let i=0;i<6;i++){
    const f = document.createElement('div'); 
    f.style.display='flex'; 
    f.style.gap='6px'; 
    f.style.alignItems='center';
    const inp = document.createElement('input'); 
    inp.type='time'; 
    inp.id='feed_time_'+i; 
    inp.className='w-full';
    const clr = document.createElement('button'); 
    clr.className='btn-secondary'; 
    clr.style.padding='6px 8px'; 
    clr.innerHTML = '<i class="fas fa-times"></i>';
    clr.title = 'Clear time slot';
    clr.onclick = ((idx, el)=>{ return (ev)=>{ el.value=''; }; })(i, inp);
    f.appendChild(inp); 
    f.appendChild(clr); 
    ft.appendChild(f);
  }
  wrap.appendChild(ft);
  
  const saveSched = document.createElement('button'); 
  saveSched.className='btn'; 
  saveSched.style.marginTop='12px'; 
  saveSched.innerHTML = '<i class="fas fa-save"></i> Save Feed Schedule';
  saveSched.onclick = ()=>{
    let savedCount = 0;
    for (let i=0;i<6;i++){
      const v = document.getElementById('feed_time_'+i).value;
      if (v) {
        sendCmd('SET_FEED_SCHEDULE:' + i + ',' + v);
        savedCount++;
      } else {
        sendCmd('CLEAR_FEED_SCHEDULE:' + i);
      }
    }
    addText('Feed schedule saved.');
    notificationManager.show(
      'Feed Schedule Updated',
      `${savedCount} feeding time(s) scheduled`,
      'success',
      3000
    );
  };
  wrap.appendChild(saveSched);
  div.appendChild(wrap);
  return;
}

// ===== AUDIO =====
if (menu === 'AUDIO') {
  addText('<i class="fas fa-volume-up"></i> Audio Volume');
  
  const volContainer = document.createElement('div');
  volContainer.style.marginTop='12px';
  
  const vol = document.createElement('input'); 
  vol.type='range'; 
  vol.min=0; 
  vol.max=30; 
  vol.value=(window._audioVol !== undefined) ? window._audioVol : 15; 
  vol.style.display='block'; 
  vol.id='audio_vol'; 
  vol.className='w-full';
  volContainer.appendChild(vol);
  
  const sp = document.createElement('div'); 
  sp.className='text-muted'; 
  sp.style.marginTop='6px'; 
  sp.innerHTML = '<i class="fas fa-volume-up" style="margin-right: 6px;"></i>Volume: ' + vol.value; 
  volContainer.appendChild(sp);
  
  vol.oninput = () => { 
    sp.innerHTML = '<i class="fas fa-volume-up" style="margin-right: 6px;"></i>Volume: ' + vol.value; 
  };
  
  div.appendChild(volContainer);
  
  addBtn('Set Volume', () => { 
    const v = Number(document.getElementById('audio_vol').value || 0); 
    sendCmd('SET_AUDIO_VOL:' + v); 
    window._audioVol = v; 
    addText('Audio volume sent: ' + v); 
    notificationManager.show(
      'Volume Updated',
      `Audio volume set to ${v}`,
      'info',
      2000
    );
  }, 'fas fa-check');
  return;
}

// ===== CLOCK =====
if (menu === 'CLOCK'){
  addText('<i class="fas fa-clock"></i> Set Date & Time (RTC)');
  
  const dtc = document.createElement('input'); 
  dtc.type='datetime-local'; 
  dtc.id='clock_dt'; 
  dtc.style.display='block'; 
  dtc.style.marginTop='8px'; 
  dtc.className='w-full';
  div.appendChild(dtc);
  
  addBtn('Set RTC', ()=>{ 
    const v=document.getElementById('clock_dt').value; 
    if(!v){ 
      notificationManager.show(
        'Date/Time Error',
        'Please select date and time',
        'error',
        3000
      );
      return; 
    } 
    sendCmd('SET_DATETIME:'+v); 
    addText('Date and time set.');
    notificationManager.show(
      'RTC Updated',
      `Date/time set to ${v}`,
      'success',
      3000
    );
  }, 'fas fa-sync-alt');
  return;
}

// fallback generic
addText('Menu opened: ' + menu);
}

// Initialize everything
window.onload = function(){ 
  populatePresets(); 

  // Initialize chart first
  generateDemoData();
  initializeChart();
  
  refreshStatus(); 
  startWS(); 
  setInterval(refreshStatus, 5000); 
  
  // Show welcome notification
  setTimeout(() => {
    notificationManager.show(
      'Dashboard Ready',
      'System monitoring is active',
      'success',
      4000
    );
  }, 1000);
};
</script>

</body>
</html>
)HTML";

const char WIFI_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Wi-Fi Configuration - Aquarium Controller</title>
<link rel='stylesheet' href='https://cdn.jsdelivr.net/npm/modern-css-reset/dist/reset.min.css'>
<style>
:root {
  --primary: #10b981;
  --primary-dark: #059669;
  --secondary: #06b6d4;
  --dark-bg: #071021;
  --darker-bg: #050a18;
  --card-bg: rgba(255,255,255,0.03);
  --card-border: rgba(255,255,255,0.06);
  --text-primary: #eaf3f6;
  --text-secondary: #9aa6b2;
  --text-muted: #6b7280;
  --success: #10b981;
  --warning: #f59e0b;
  --error: #ef4444;
  --radius-lg: 16px;
  --radius-md: 12px;
  --radius-sm: 8px;
}

* {
  box-sizing: border-box;
}

body {
  font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
  background: linear-gradient(135deg, var(--darker-bg) 0%, var(--dark-bg) 100%);
  color: var(--text-primary);
  line-height: 1.6;
  min-height: 100vh;
  padding: 0;
  margin: 0;
  display: flex;
  align-items: center;
  justify-content: center;
}

.container {
  max-width: 500px;
  width: 90%;
  margin: 20px auto;
}

.card {
  background: var(--card-bg);
  border: 1px solid var(--card-border);
  border-radius: var(--radius-lg);
  padding: 32px;
  backdrop-filter: blur(20px);
  box-shadow: 0 8px 40px rgba(2, 6, 23, 0.4);
  margin-bottom: 24px;
}

.header {
  text-align: center;
  margin-bottom: 32px;
}

.logo {
  width: 64px;
  height: 64px;
  border-radius: var(--radius-md);
  background: linear-gradient(135deg, var(--secondary), var(--primary));
  display: flex;
  align-items: center;
  justify-content: center;
  font-weight: 800;
  font-size: 24px;
  color: white;
  box-shadow: 0 4px 20px rgba(6, 182, 212, 0.3);
  margin: 0 auto 16px;
}

h1 {
  margin: 0 0 8px;
  font-size: 28px;
  font-weight: 700;
  background: linear-gradient(135deg, var(--text-primary), var(--secondary));
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
}

.subtitle {
  color: var(--text-secondary);
  font-size: 14px;
}

.form-group {
  margin-bottom: 24px;
}

label {
  display: block;
  margin-bottom: 8px;
  color: var(--text-primary);
  font-weight: 500;
  font-size: 14px;
}

input[type="text"], input[type="password"] {
  width: 100%;
  background: rgba(255,255,255,0.05);
  border: 1px solid var(--card-border);
  border-radius: var(--radius-md);
  padding: 14px 16px;
  color: var(--text-primary);
  font-size: 15px;
  transition: all 0.2s ease;
}

input:focus {
  outline: none;
  border-color: var(--primary);
  box-shadow: 0 0 0 3px rgba(16, 185, 129, 0.1);
}

input::placeholder {
  color: var(--text-muted);
}

.btn {
  width: 100%;
  background: var(--primary);
  color: white;
  border: none;
  padding: 14px 20px;
  border-radius: var(--radius-md);
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s ease;
  font-size: 15px;
  margin-top: 8px;
}

.btn:hover {
  background: var(--primary-dark);
  transform: translateY(-1px);
  box-shadow: 0 4px 20px rgba(16, 185, 129, 0.3);
}

.btn-secondary {
  background: transparent;
  border: 1px solid var(--card-border);
  color: var(--text-secondary);
  margin-top: 12px;
}

.btn-secondary:hover {
  background: rgba(255,255,255,0.05);
  border-color: var(--text-secondary);
  color: var(--text-primary);
}

.networks-section {
  margin-top: 32px;
}

.section-title {
  font-size: 18px;
  font-weight: 600;
  margin-bottom: 16px;
  color: var(--text-primary);
  display: flex;
  align-items: center;
  gap: 8px;
}

.section-title::before {
  content: "📶";
  font-size: 20px;
}

.networks-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
  gap: 12px;
  margin-bottom: 20px;
}

.network-btn {
  background: rgba(255,255,255,0.02);
  border: 1px solid var(--card-border);
  border-radius: var(--radius-md);
  padding: 12px 8px;
  color: var(--text-secondary);
  cursor: pointer;
  transition: all 0.2s ease;
  font-size: 13px;
  text-align: center;
  word-break: break-word;
}

.network-btn:hover {
  background: rgba(255,255,255,0.05);
  border-color: var(--primary);
  color: var(--text-primary);
  transform: translateY(-1px);
}

.loading {
  text-align: center;
  color: var(--text-muted);
  font-size: 14px;
  padding: 20px;
}

.loading::after {
  content: " ...";
  animation: dots 1.5s steps(4, end) infinite;
}

@keyframes dots {
  0%, 20% { color: var(--text-muted); text-shadow: none; }
  50% { color: var(--primary); text-shadow: 0 0 5px var(--primary); }
  100% { color: var(--text-muted); text-shadow: none; }
}

.status-message {
  padding: 12px 16px;
  border-radius: var(--radius-md);
  margin-top: 16px;
  font-size: 14px;
  text-align: center;
  display: none;
}

.status-success {
  background: rgba(16, 185, 129, 0.1);
  border: 1px solid rgba(16, 185, 129, 0.3);
  color: var(--success);
}

.status-error {
  background: rgba(239, 68, 68, 0.1);
  border: 1px solid rgba(239, 68, 68, 0.3);
  color: var(--error);
}

.back-link {
  text-align: center;
  margin-top: 24px;
}

.back-link a {
  color: var(--text-secondary);
  text-decoration: none;
  font-size: 14px;
  transition: color 0.2s ease;
}

.back-link a:hover {
  color: var(--primary);
}

@media (max-width: 480px) {
  .container {
    width: 95%;
  }
  
  .card {
    padding: 24px 20px;
  }
  
  .networks-grid {
    grid-template-columns: repeat(auto-fill, minmax(120px, 1fr));
  }
}
</style>
</head>
<body>
<div class="container">
  <div class="card">
    <div class="header">
      <div class="logo">AQ</div>
      <h1>Wi-Fi Setup</h1>
      <div class="subtitle">Configure your device's network connection</div>
    </div>

    <form id="wifiForm" onsubmit="doSave(event)">
      <div class="form-group">
        <label for="ssid">Network Name (SSID)</label>
        <input type="text" id="ssid" placeholder="Enter your Wi-Fi network name" required>
      </div>
      
      <div class="form-group">
        <label for="pass">Password</label>
        <input type="password" id="pass" placeholder="Enter your Wi-Fi password">
      </div>

      <button type="submit" class="btn">
        Save & Connect
      </button>
      
      <div id="statusMessage" class="status-message"></div>
    </form>

    <div class="networks-section">
      <div class="section-title">Available Networks</div>
      <div id="networksList" class="loading">
        Scanning for Wi-Fi networks...
      </div>
      <button type="button" class="btn btn-secondary" onclick="scanNetworks()">
        Rescan Networks
      </button>
    </div>
  </div>

  <div class="back-link">
    <a href="/">← Back to Dashboard</a>
  </div>
</div>

<script>
async function scanNetworks() {
  const list = document.getElementById('networksList');
  list.innerHTML = '<div class="loading">Scanning for Wi-Fi networks...</div>';
  
  try {
    const response = await fetch('/wifi/scan', { credentials: 'same-origin' });
    const networks = await response.json();
    
    if (networks.length === 0) {
      list.innerHTML = '<div style="text-align: center; color: var(--text-muted); padding: 20px;">No networks found</div>';
      return;
    }
    
    list.innerHTML = '<div class="networks-grid"></div>';
    const grid = list.querySelector('.networks-grid');
    
    networks.forEach(ssid => {
      const button = document.createElement('button');
      button.className = 'network-btn';
      button.type = 'button';
      button.innerText = ssid;
      button.onclick = () => {
        document.getElementById('ssid').value = ssid;
        document.getElementById('pass').focus();
        
        // Add visual feedback
        document.querySelectorAll('.network-btn').forEach(btn => {
          btn.style.borderColor = 'var(--card-border)';
          btn.style.background = 'rgba(255,255,255,0.02)';
        });
        button.style.borderColor = 'var(--primary)';
        button.style.background = 'rgba(16, 185, 129, 0.1)';
      };
      grid.appendChild(button);
    });
    
  } catch (error) {
    list.innerHTML = '<div style="text-align: center; color: var(--error); padding: 20px;">Failed to scan networks</div>';
  }
}

async function doSave(e) {
  e.preventDefault();
  
  const ssid = document.getElementById('ssid').value.trim();
  const password = document.getElementById('pass').value;
  const statusEl = document.getElementById('statusMessage');
  
  if (!ssid) {
    showStatus('Please enter a network name', 'error');
    return;
  }
  
  const button = e.target.querySelector('button[type="submit"]');
  const originalText = button.innerHTML;
  button.innerHTML = 'Connecting...';
  button.disabled = true;
  
  try {
    const body = new URLSearchParams();
    body.append('ssid', ssid);
    body.append('pass', password);
    
    const response = await fetch('/wifi/save', {
      method: 'POST',
      body: body,
      credentials: 'same-origin'
    });
    
    const result = await response.text();
    
    if (response.ok) {
      showStatus('Wi-Fi settings saved successfully! Device is connecting...', 'success');
      setTimeout(() => {
        window.location.href = '/';
      }, 2000);
    } else {
      showStatus('Failed to save settings: ' + result, 'error');
    }
  } catch (error) {
    showStatus('Network error: ' + error.message, 'error');
  } finally {
    button.innerHTML = originalText;
    button.disabled = false;
  }
}

function showStatus(message, type) {
  const statusEl = document.getElementById('statusMessage');
  statusEl.textContent = message;
  statusEl.className = `status-message status-${type}`;
  statusEl.style.display = 'block';
  
  // Auto-hide success messages after 5 seconds
  if (type === 'success') {
    setTimeout(() => {
      statusEl.style.display = 'none';
    }, 5000);
  }
}

// Initial scan when page loads
scanNetworks();
</script>
</body>
</html>
)HTML";

const char CREDS_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Change Credentials - Aquarium Controller</title>
<link rel='stylesheet' href='https://cdn.jsdelivr.net/npm/modern-css-reset/dist/reset.min.css'>
<style>
:root {
  --primary: #10b981;
  --primary-dark: #059669;
  --secondary: #06b6d4;
  --dark-bg: #071021;
  --darker-bg: #050a18;
  --card-bg: rgba(255,255,255,0.03);
  --card-border: rgba(255,255,255,0.06);
  --text-primary: #eaf3f6;
  --text-secondary: #9aa6b2;
  --text-muted: #6b7280;
  --success: #10b981;
  --warning: #f59e0b;
  --error: #ef4444;
  --radius-lg: 16px;
  --radius-md: 12px;
  --radius-sm: 8px;
}

* {
  box-sizing: border-box;
}

body {
  font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
  background: linear-gradient(135deg, var(--darker-bg) 0%, var(--dark-bg) 100%);
  color: var(--text-primary);
  line-height: 1.6;
  min-height: 100vh;
  padding: 0;
  margin: 0;
  display: flex;
  align-items: center;
  justify-content: center;
}

.container {
  max-width: 500px;
  width: 90%;
  margin: 20px auto;
}

.card {
  background: var(--card-bg);
  border: 1px solid var(--card-border);
  border-radius: var(--radius-lg);
  padding: 32px;
  backdrop-filter: blur(20px);
  box-shadow: 0 8px 40px rgba(2, 6, 23, 0.4);
}

.header {
  text-align: center;
  margin-bottom: 24px;
}

.logo {
  width: 64px;
  height: 64px;
  border-radius: var(--radius-md);
  background: linear-gradient(135deg, var(--secondary), var(--primary));
  display: flex;
  align-items: center;
  justify-content: center;
  font-weight: 800;
  font-size: 24px;
  color: white;
  box-shadow: 0 4px 20px rgba(6, 182, 212, 0.3);
  margin: 0 auto 16px;
}

h1 {
  margin: 0 0 8px;
  font-size: 28px;
  font-weight: 700;
  background: linear-gradient(135deg, var(--text-primary), var(--secondary));
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
}

.subtitle {
  color: var(--text-secondary);
  font-size: 14px;
}

.notice {
  background: linear-gradient(135deg, rgba(245, 158, 11, 0.1), rgba(245, 158, 11, 0.05));
  border: 1px solid rgba(245, 158, 11, 0.3);
  border-radius: var(--radius-md);
  padding: 16px;
  margin-bottom: 24px;
  font-size: 14px;
  color: #ffdca8;
}

.notice strong {
  color: #fbbf24;
}

.form-group {
  margin-bottom: 20px;
}

label {
  display: block;
  margin-bottom: 8px;
  color: var(--text-primary);
  font-weight: 500;
  font-size: 14px;
}

.input-hint {
  font-size: 12px;
  color: var(--text-muted);
  margin-top: 4px;
  margin-bottom: 8px;
}

input[type="text"], input[type="password"] {
  width: 100%;
  background: rgba(255,255,255,0.05);
  border: 1px solid var(--card-border);
  border-radius: var(--radius-md);
  padding: 14px 16px;
  color: var(--text-primary);
  font-size: 15px;
  transition: all 0.2s ease;
}

input:focus {
  outline: none;
  border-color: var(--primary);
  box-shadow: 0 0 0 3px rgba(16, 185, 129, 0.1);
}

input::placeholder {
  color: var(--text-muted);
}

.btn {
  background: var(--primary);
  color: white;
  border: none;
  padding: 14px 24px;
  border-radius: var(--radius-md);
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s ease;
  font-size: 15px;
  flex: 1;
}

.btn:hover {
  background: var(--primary-dark);
  transform: translateY(-1px);
  box-shadow: 0 4px 20px rgba(16, 185, 129, 0.3);
}

.btn-secondary {
  background: transparent;
  border: 1px solid var(--card-border);
  color: var(--text-secondary);
}

.btn-secondary:hover {
  background: rgba(255,255,255,0.05);
  border-color: var(--text-secondary);
  color: var(--text-primary);
}

.button-group {
  display: flex;
  gap: 12px;
  margin-top: 24px;
}

.status-message {
  padding: 12px 16px;
  border-radius: var(--radius-md);
  margin-top: 16px;
  font-size: 14px;
  text-align: center;
  display: none;
}

.status-success {
  background: rgba(16, 185, 129, 0.1);
  border: 1px solid rgba(16, 185, 129, 0.3);
  color: var(--success);
}

.status-error {
  background: rgba(239, 68, 68, 0.1);
  border: 1px solid rgba(239, 68, 68, 0.3);
  color: var(--error);
}

.back-link {
  text-align: center;
  margin-top: 24px;
}

.back-link a {
  color: var(--text-secondary);
  text-decoration: none;
  font-size: 14px;
  transition: color 0.2s ease;
}

.back-link a:hover {
  color: var(--primary);
}

.password-strength {
  height: 4px;
  background: var(--card-border);
  border-radius: 2px;
  margin-top: 8px;
  overflow: hidden;
}

.strength-weak { background: linear-gradient(90deg, #ef4444 0%, #ef4444 33%); }
.strength-medium { background: linear-gradient(90deg, #f59e0b 0%, #f59e0b 66%); }
.strength-strong { background: linear-gradient(90deg, #10b981 0%, #10b981 100%); }

@media (max-width: 480px) {
  .container {
    width: 95%;
  }
  
  .card {
    padding: 24px 20px;
  }
  
  .button-group {
    flex-direction: column;
  }
}
</style>
</head>
<body>
<div class="container">
  <div class="card">
    <div class="header">
      <div class="logo">AQ</div>
      <h1>Change Credentials</h1>
      <div class="subtitle">Update your dashboard login information</div>
    </div>

    <div class="notice">
      <strong>Authentication Required:</strong> If you're not logged in, you must enter your current username and password. If already logged in, leave current credentials blank.
    </div>

    <form id="credsForm" onsubmit="saveCredentials(event)">
      <div class="form-group">
        <label for="currentUser">Current Username</label>
        <div class="input-hint">Required only if you're not currently logged in</div>
        <input type="text" id="currentUser" placeholder="Enter current username">
      </div>

      <div class="form-group">
        <label for="currentPass">Current Password</label>
        <div class="input-hint">Required only if you're not currently logged in</div>
        <input type="password" id="currentPass" placeholder="Enter current password">
      </div>

      <div class="form-group">
        <label for="newUser">New Username</label>
        <input type="text" id="newUser" placeholder="Enter new username" required>
      </div>

      <div class="form-group">
        <label for="newPass">New Password</label>
        <input type="password" id="newPass" placeholder="Enter new password" required oninput="updatePasswordStrength(this.value)">
        <div class="password-strength" id="passwordStrength"></div>
        <div class="input-hint">Use a strong password with letters, numbers, and symbols</div>
      </div>

      <div id="statusMessage" class="status-message"></div>

      <div class="button-group">
        <button type="submit" class="btn">
          Update Credentials
        </button>
        <button type="button" class="btn btn-secondary" onclick="cancelChanges()">
          Cancel
        </button>
      </div>
    </form>
  </div>

  <div class="back-link">
    <a href="/">← Back to Dashboard</a>
  </div>
</div>

<script>
function updatePasswordStrength(password) {
  const strengthBar = document.getElementById('passwordStrength');
  let strength = 0;
  
  if (password.length >= 8) strength++;
  if (password.match(/[a-z]/) && password.match(/[A-Z]/)) strength++;
  if (password.match(/\d/)) strength++;
  if (password.match(/[^a-zA-Z\d]/)) strength++;
  
  strengthBar.className = 'password-strength';
  if (password.length > 0) {
    if (strength <= 2) {
      strengthBar.classList.add('strength-weak');
    } else if (strength === 3) {
      strengthBar.classList.add('strength-medium');
    } else {
      strengthBar.classList.add('strength-strong');
    }
  }
}

async function saveCredentials(e) {
  e.preventDefault();
  
  const currentUser = document.getElementById('currentUser').value.trim();
  const currentPass = document.getElementById('currentPass').value;
  const newUser = document.getElementById('newUser').value.trim();
  const newPass = document.getElementById('newPass').value;
  const statusEl = document.getElementById('statusMessage');
  
  if (!newUser || !newPass) {
    showStatus('Please enter both new username and password', 'error');
    return;
  }
  
  if (newPass.length < 4) {
    showStatus('Password should be at least 4 characters long', 'error');
    return;
  }
  
  const button = e.target.querySelector('button[type="submit"]');
  const originalText = button.innerHTML;
  button.innerHTML = 'Updating...';
  button.disabled = true;
  
  try {
    const body = new URLSearchParams();
    if (currentUser) body.append('cur_user', currentUser);
    if (currentPass) body.append('cur_pass', currentPass);
    body.append('user', newUser);
    body.append('pass', newPass);
    
    const response = await fetch('/change-creds', {
      method: 'POST',
      body: body,
      credentials: 'same-origin'
    });
    
    const result = await response.text();
    
    if (response.status === 200) {
      showStatus('Credentials updated successfully! Redirecting to login...', 'success');
      
      // Clear stored session and redirect
      localStorage.removeItem('session_token');
      document.cookie = 'session=;path=/;max-age=0';
      
      setTimeout(() => {
        window.location.href = '/';
      }, 2000);
    } else if (response.status === 401) {
      showStatus('Authentication failed. Please provide current credentials or log in first.', 'error');
    } else {
      showStatus('Error: ' + result, 'error');
    }
  } catch (error) {
    showStatus('Network error: ' + error.message, 'error');
  } finally {
    button.innerHTML = originalText;
    button.disabled = false;
  }
}

function cancelChanges() {
  if (confirm('Are you sure you want to cancel? Any unsaved changes will be lost.')) {
    window.location.href = '/';
  }
}

function showStatus(message, type) {
  const statusEl = document.getElementById('statusMessage');
  statusEl.textContent = message;
  statusEl.className = `status-message status-${type}`;
  statusEl.style.display = 'block';
  
  // Auto-hide after 5 seconds
  setTimeout(() => {
    statusEl.style.display = 'none';
  }, 5000);
}

// Focus on first input
document.addEventListener('DOMContentLoaded', function() {
  const currentUser = document.getElementById('currentUser');
  if (currentUser.value === '') {
    currentUser.focus();
  }
});
</script>
</body>
</html>
)HTML";

// ---------- HTTP handlers ----------
void handleRoot() {
  // If a token is passed via query param (e.g. /?token=...), validate it.
  if (server.hasArg("token")) {
    String tok = server.arg("token");
    // sanity
    if (tok.length() > 10) {
      String sub = verifyJWT(tok);
      if (sub.length()) {
        Serial.printf("handleRoot: valid token for '%s'\n", sub.c_str());
        // set cookie (optional; many clients will accept it)
        String cookie = "session=" + tok + "; Path=/; Max-Age=" + String(JWT_EXP_SECONDS);
        server.sendHeader("Set-Cookie", cookie);
        server.sendHeader("Cache-Control","no-store");
        
        // Combine the split HTML parts
        String html = FPSTR(MAIN_PAGE_HEAD);
        html += FPSTR(MAIN_PAGE_MIDDLE);
        html += FPSTR(MAIN_PAGE_TAIL);
        html += FPSTR(MAIN_PAGE_SCRIPT);
        server.send(200,"text/html", html);
        return;
      } else {
        Serial.println("handleRoot: token invalid or expired");
        // fallthrough to show login
      }
    }
  }

  // normal behavior: require cookie/bearer
  if (!checkAuth()) {
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"text/html", LOGIN_PAGE);
    return;
  }
  
  server.sendHeader("Cache-Control","no-store");
  
  // Combine the split HTML parts for authenticated users
  String html = FPSTR(MAIN_PAGE_HEAD);
  html += FPSTR(MAIN_PAGE_MIDDLE);
  html += FPSTR(MAIN_PAGE_TAIL);
  html += FPSTR(MAIN_PAGE_SCRIPT);
  server.send(200,"text/html", html);
}



void handleLogin() {
  if (!server.hasArg("user") || !server.hasArg("pass")) { server.send(400,"text/plain","Missing"); return; }
  String u = server.arg("user"), p = server.arg("pass");
  if (u == prefGetUser() && p == prefGetPass()) {
    String token = createJWT(u);
    // set cookie too (optional)
    String cookie = "session=" + token + "; Path=/; Max-Age=" + String(JWT_EXP_SECONDS);
    server.sendHeader("Set-Cookie", cookie);
    // return token in JSON body
    String body = String("{\"token\":\"") + token + String("\"}");
    server.send(200, "application/json", body);
  } else server.send(401,"text/plain","Invalid");
}




void handleLogout(){
  server.sendHeader("Set-Cookie","session=; Path=/; Max-Age=0");
  server.send(200,"text/plain","OK");
}

void handleApiCmd() {
  if (!checkAuth()) { server.send(401,"text/plain","Unauthorized"); return; }
  String cmd = "";
  if (server.hasArg("cmd")) cmd = server.arg("cmd");
  else cmd = server.arg("plain");
  cmd.trim();
  if (cmd.length()==0) { server.send(400,"text/plain","No command"); return; }

  // Special-case: SHOW_IP -> write IP to Serial1 for Mega/TFT
  if (cmd.equalsIgnoreCase("SHOW_IP")) {
    String ip = WiFi.localIP().toString();
    // Write the WIFI_IP message to Serial1 so the Mega's processEspCommand(...) sees it
    Serial1.print("WIFI_IP:"); Serial1.println(ip);
    // Also send a small HTTP response so UI knows it succeeded
    server.send(200,"text/plain","OK");
    return;
  }

  // Normal behavior: forward to serial and wait for a reply
  String resp = sendSerialCommand(cmd, 2000);
  server.send(200,"text/plain",resp);
}

void handleStatus() {
    // Return the current status in the format your web page expects
    String status = "BRIGHT:" + String(currentBrightness) + 
                   ",MODE:" + String(currentMode) +
                   ",TEMP:" + String(currentTemperature, 1) +
                   ",PH:" + String(currentPH, 2) +
                   ",LEVEL:" + String(currentWaterLevel) +
                   ",RGB_PRESET:" + String(currentPreset);
    
    server.send(200, "text/plain", status);
    Serial.println("API Status: " + status);
}

// Wi-Fi captive portal handlers
void handleWifiPage() {
  server.send(200,"text/html", WIFI_PAGE);
}

void handleWifiScan() {
  int n = WiFi.scanComplete();
  if (n == -2) {
    WiFi.scanNetworks(true);
    server.send(200,"application/json","[]");
    return;
  }
  if (n == -1) n = WiFi.scanComplete();
  String out = "[";
  for (int i=0;i<n;i++){
    if (i) out += ",";
    out += "\"" + WiFi.SSID(i) + "\"";
  }
  out += "]";
  server.send(200,"application/json", out);
  WiFi.scanDelete();
}

void handleWifiSave() {
  if (!server.hasArg("ssid") || !server.hasArg("pass")) { server.send(400,"text/plain","Missing"); return; }
  String ss = server.arg("ssid"), pp = server.arg("pass");
  prefs.putString("wifi_ssid", ss);
  prefs.putString("wifi_pass", pp);
  server.send(200,"text/plain","Saved");
  delay(200);
  WiFi.begin(ss.c_str(), pp.c_str());
}

void handleCredsPage() {
  // If a token is supplied via query (e.g. /change-creds?token=...), validate it
  if (server.hasArg("token")) {
    String tok = server.arg("token");
    if (tok.length() > 10 && verifyJWT(tok).length()) {
      // set cookie for subsequent requests and serve the page
      String cookie = "session=" + tok + "; Path=/; Max-Age=" + String(JWT_EXP_SECONDS);
      server.sendHeader("Set-Cookie", cookie);
      server.send(200, "text/html", CREDS_PAGE);
      return;
    } else {
      Serial.println("handleCredsPage: token invalid or expired");
      // fall through and still serve the creds page (user can supply current creds)
    }
  }

  // Serve the Change Credentials page even when not logged in.
  // The POST handler (handleCredsSave) enforces authentication or validates provided current credentials.
  server.send(200, "text/html", CREDS_PAGE);
}



void handleCredsSave() {
  bool ok = false;
  if (checkAuth()) {
    ok = true;
  } else if (server.hasArg("cur_user") && server.hasArg("cur_pass")) {
    String cu = server.arg("cur_user");
    String cp = server.arg("cur_pass");
    if (cu == prefGetUser() && cp == prefGetPass()) ok = true;
  }

  if (!ok) {
    server.send(401,"text/plain","Unauthorized - must provide current credentials or be logged in");
    return;
  }

  if (!server.hasArg("user") || !server.hasArg("pass")) { server.send(400,"text/plain","Missing new user or pass"); return; }
  String u = server.arg("user"), p = server.arg("pass");
  prefs.putString("user", u);
  prefs.putString("pass", p);
  server.send(200,"text/plain","Saved");
}


// ---------- WebSocket ----------
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_CONNECTED) {
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("WS client %u connected from %u.%u.%u.%u\n", num, ip[0],ip[1],ip[2],ip[3]);
    String welcome = "[connected]";
    webSocket.sendTXT(num, welcome);
  } else if (type == WStype_DISCONNECTED) {
    Serial.printf("WS client %u disconnected\n", num);
  } else if (type == WStype_TEXT) {
    String msg = (payload && length>0) ? String((char*)payload, length) : String();
    Serial.printf("WS recv: %s\n", msg.c_str());
    if (msg.startsWith("CMD:")) {
      String cmd = msg.substring(4);
      String resp = sendSerialCommand(cmd, 2000);
      webSocket.sendTXT(num, resp);
    } else if (msg == "PING") {
      webSocket.sendTXT(num, "PONG");
    } else {
      String echo = "ECHO:" + msg;
      webSocket.sendTXT(num, echo);
    }
  }
}

// ---------- Wi-Fi helpers ----------
void startAPPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("AquaSmartController-AP");
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
}

void tryConnectStoredWifi() {
  String ss = prefGetWifiSsid();
  String pp = prefGetWifiPass();
  Serial.printf("Trying stored WiFi '%s' ...\n", ss.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ss.c_str(), pp.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(); Serial.print("Connected IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to join stored WiFi. Starting AP for captive portal.");
    startAPPortal();
  }
}


// Blynk optimization variables
unsigned long lastBlynkUpdate = 0;
const unsigned long BLYNK_UPDATE_INTERVAL = 30000; // Update every 30 seconds
int lastSentMode = -1;
int lastSentBrightness = -1;
float lastSentTemperature = -100;
float lastSentPH = -100;
int lastSentWaterLevel = -1;
int lastSentPreset = -1;
int lastSentFilterStatus = -1;
int lastSentWCStatus = -1;
/*
// Global variables to store parsed data
int currentBrightness = 0;
int currentMode = 0;
float currentTemperature = 0;
float currentPH = 0;
int currentWaterLevel = 0;
int currentPreset = 0;
*/
// ========== COMPLETE BLYNK HANDLERS ==========

// Brightness Control (V0)
BLYNK_WRITE(BLYNK_VPIN_BRIGHTNESS) {
  int value = param.asInt();
  String cmd = "SET_BRIGHTNESS:" + String(value);
  sendSerialCommand(cmd);
  // Don't immediately write back - wait for status update from Mega
}

// Preset Control (V2)
// Enhanced RGB Preset Control (V2) - Auto-enables RGB mode and shows name
BLYNK_WRITE(BLYNK_VPIN_PRESET) {
  int preset = param.asInt();
  
  // First set RGB mode, then set preset
  sendSerialCommand("SET_MODE:7"); // RGB mode
  delay(100); // Small delay to ensure mode change
  sendSerialCommand("SET_PRESET:" + String(preset));
  
  // Update status with preset name
  String presetName = presetNames[preset];
  String status = "🎨 RGB Preset: " + presetName;
  Blynk.virtualWrite(BLYNK_VPIN_STATUS, status);
}

// Feed Now Button (V3)
BLYNK_WRITE(BLYNK_VPIN_FEED_NOW) {
  if (param.asInt() == 1) {
    sendSerialCommand("FEED_NOW:3"); // Default 3 seconds
    // Reset button after 2 seconds
    delay(2000);
    Blynk.virtualWrite(BLYNK_VPIN_FEED_NOW, 0);
  }
}

// Water Change Control (V4)
BLYNK_WRITE(BLYNK_VPIN_WATER_CHANGE) {
  int state = param.asInt();
  if (state == 1) {
    sendSerialCommand("START_WC");
  } else {
    sendSerialCommand("STOP_WC");
  }
}

// Dual-mode toggle switch handlers
BLYNK_WRITE(BLYNK_VPIN_SUNRISE_SUNSET) {
  int state = param.asInt();
  if (state == 0) {
    sendSerialCommand("SET_MODE:0"); // Sunrise
  } else {
    sendSerialCommand("SET_MODE:1"); // Sunset
  }
}

BLYNK_WRITE(BLYNK_VPIN_MOONLIGHT_CLOUDY) {
  int state = param.asInt();
  if (state == 0) {
    sendSerialCommand("SET_MODE:2"); // Moonlight
  } else {
    sendSerialCommand("SET_MODE:3"); // Cloudy
  }
}

BLYNK_WRITE(BLYNK_VPIN_THUNDER_RAINY) {
  int state = param.asInt();
  if (state == 0) {
    sendSerialCommand("SET_MODE:4"); // Thunder
  } else {
    sendSerialCommand("SET_MODE:5"); // Rainy
  }
}

BLYNK_WRITE(BLYNK_VPIN_FOG_RGB) {
  int state = param.asInt();
  if (state == 0) {
    sendSerialCommand("SET_MODE:6"); // Fog
  } else {
    sendSerialCommand("SET_MODE:7"); // RGB
    // When switching to RGB mode, show current preset name in status
    String presetName = presetNames[currentPreset];
    String status = "🎨 RGB Mode | Preset: " + presetName;
    Blynk.virtualWrite(BLYNK_VPIN_STATUS, status);
  }
}

// RGB Preset Up Button (V17)
BLYNK_WRITE(BLYNK_VPIN_RGB_PRESET_UP) {
  if (param.asInt() == 1) {
    // Increment preset (wrap around)
    int newPreset = currentPreset + 1;
    if (newPreset >= NUM_PRESETS) newPreset = 0;
    
    String cmd = "SET_PRESET:" + String(newPreset);
    sendSerialCommand(cmd);
    
    // Update status to show preset name
    String presetName = presetNames[newPreset];
    String status = "🎨 RGB Preset: " + presetName;
    Blynk.virtualWrite(BLYNK_VPIN_STATUS, status);
    
    // Reset button after 1 second
    delay(1000);
    Blynk.virtualWrite(BLYNK_VPIN_RGB_PRESET_UP, 0);
    
    // Request updated status to sync
    sendSerialCommand("GET_STATUS");
  }
}

// RGB Preset Down Button (V18)
BLYNK_WRITE(BLYNK_VPIN_RGB_PRESET_DOWN) {
  if (param.asInt() == 1) {
    // Decrement preset (wrap around)
    int newPreset = currentPreset - 1;
    if (newPreset < 0) newPreset = NUM_PRESETS - 1;
    
    String cmd = "SET_PRESET:" + String(newPreset);
    sendSerialCommand(cmd);
    
    // Update status to show preset name
    String presetName = presetNames[newPreset];
    String status = "🎨 RGB Preset: " + presetName;
    Blynk.virtualWrite(BLYNK_VPIN_STATUS, status);
    
    // Reset button after 1 second
    delay(1000);
    Blynk.virtualWrite(BLYNK_VPIN_RGB_PRESET_DOWN, 0);
    
    // Request updated status to sync
    sendSerialCommand("GET_STATUS");
  }
}
/*
BLYNK_WRITE(BLYNK_VPIN_OFF_MODE) {
  if (param.asInt() == 1) {
    sendSerialCommand("SET_MODE:8"); // Off
    // Turn off all other toggle switches when Off is selected
    Blynk.virtualWrite(BLYNK_VPIN_SUNRISE_SUNSET, 0);
    Blynk.virtualWrite(BLYNK_VPIN_MOONLIGHT_CLOUDY, 0);
    Blynk.virtualWrite(BLYNK_VPIN_THUNDER_RAINY, 0);
    Blynk.virtualWrite(BLYNK_VPIN_FOG_RGB, 0);
  }
}
*/
// Show IP Button (V14)
BLYNK_WRITE(BLYNK_VPIN_SHOW_IP) {
  if (param.asInt() == 1) {
    // Check if we're connected to WiFi
    if (WiFi.status() == WL_CONNECTED) {
      String ip = WiFi.localIP().toString();
      
      // Send WIFI_IP: command to Mega (exactly like webpage)
      Serial1.print("WIFI_IP:");
      Serial1.println(ip);
      
      Serial.println("Blynk: Sent WIFI_IP: " + ip + " to Mega TFT");
      
      // Update status with IP confirmation
      String status = "📱 IP on TFT: " + ip;
      Blynk.virtualWrite(BLYNK_VPIN_STATUS, status);
      
    } else {
      // Not connected to WiFi
      String status = "❌ Not connected to WiFi";
      Blynk.virtualWrite(BLYNK_VPIN_STATUS, status);
      Serial.println("Blynk: Cannot show IP - WiFi not connected");
    }
    
    // Reset button after 2 seconds regardless of outcome
    delay(2000);
    Blynk.virtualWrite(BLYNK_VPIN_SHOW_IP, 0);
    
    // Restore original status after 5 seconds
    delay(3000);
    updateBlynkStatusDisplay();
  }
}
// Filter Toggle Switch (V19)
BLYNK_WRITE(BLYNK_VPIN_FILTER_TOGGLE) {
  int state = param.asInt();
  
  if (state == 1) {
    // Switch turned ON
    sendSerialCommand("SET_FILTER:ON");
    
    // Update status immediately for quick feedback
    String status = "🔵 Filter: ON";
    Blynk.virtualWrite(BLYNK_VPIN_STATUS, status);
    
    Serial.println("Blynk: Filter turned ON");
  } else {
    // Switch turned OFF
    sendSerialCommand("SET_FILTER:OFF");
    
    // Update status immediately for quick feedback
    String status = "⚪ Filter: OFF";
    Blynk.virtualWrite(BLYNK_VPIN_STATUS, status);
    
    Serial.println("Blynk: Filter turned OFF");
  }
  
  // Request updated status to sync
  sendSerialCommand("GET_STATUS");
}

void updateRotatingStatus() {
  // Only update if Blynk is connected
  if (!Blynk.connected()) {  // FIXED: Use Blynk.connected() instead of blynkConnected
    return;
  }
  
  unsigned long currentTime = millis();
  if (currentTime - lastStatusRotate < STATUS_ROTATE_INTERVAL) {
    return;
  }
  
  lastStatusRotate = currentTime;
  statusDisplayMode = (statusDisplayMode + 1) % 3;
  
  String filterText = currentFilterStatus ? "🔵 ON" : "⚪ OFF";
  String wcText = currentWCStatus ? "🟢 " + currentWCState : "⚪ Stopped";
  
  switch(statusDisplayMode) {
    case 0:
      // Mode and Brightness
      if (currentMode == 7) {
        String presetName = presetNames[currentPreset];
        Blynk.virtualWrite(BLYNK_VPIN_STATUS, "🎨 RGB | " + presetName + " | " + String(currentBrightness) + "%");
      } else {
        Blynk.virtualWrite(BLYNK_VPIN_STATUS, "Mode: " + String(MODE_MAP[currentMode]) + " | Bright: " + String(currentBrightness) + "%");
      }
      break;
      
    case 1:
      // Filter and Water Change
      Blynk.virtualWrite(BLYNK_VPIN_STATUS, "Filter: " + filterText + " | WC: " + wcText);
      break;
      
    case 2:
      // Sensor Readings
      Blynk.virtualWrite(BLYNK_VPIN_STATUS, "🌡️" + String(currentTemperature, 1) + "°C | 🧪" + String(currentPH, 2) + " | 💧" + String(currentWaterLevel) + "cm");
      break;
  }
}

// Helper function to update status display
void updateBlynkStatusDisplay() {
  String filterText = currentFilterStatus ? "🔵 ON" : "⚪ OFF";
  String wcText = currentWCStatus ? "🟢 " + currentWCState : "⚪ Stopped";
  
  if (currentMode == 7) { // RGB Mode
    String presetName = presetNames[currentPreset];
    String status = "🎨 " + presetName + " | " + String(currentBrightness) + "% | Filter: " + filterText;
    Blynk.virtualWrite(BLYNK_VPIN_STATUS, status);
  } else {
    String modeName = String(MODE_MAP[currentMode]);
    String status = modeName + " | " + String(currentBrightness) + "% | Filter: " + filterText;
    Blynk.virtualWrite(BLYNK_VPIN_STATUS, status);
  }
}

// ========== BLYNK UPDATE FUNCTIONS ==========

// Function to update Blynk only when values change significantly
void updateBlynkValues() {
  unsigned long currentTime = millis();
  
  // Only update Blynk at specified intervals or when important changes occur
  if (currentTime - lastBlynkUpdate < BLYNK_UPDATE_INTERVAL && 
      abs(currentBrightness - lastSentBrightness) < 5 &&
      abs(currentTemperature - lastSentTemperature) < 0.5 &&
      abs(currentPH - lastSentPH) < 0.1 &&
      abs(currentWaterLevel - lastSentWaterLevel) < 2 &&
      currentFilterStatus == lastSentFilterStatus &&
      currentWCStatus == lastSentWCStatus) {
    return;
  }
  
  // Update brightness only if changed significantly
  if (abs(currentBrightness - lastSentBrightness) >= 5) {
    Blynk.virtualWrite(BLYNK_VPIN_BRIGHTNESS, currentBrightness);
    lastSentBrightness = currentBrightness;
  }
  
  // Update mode only if changed
  if (currentMode != lastSentMode) {
    updateBlynkModeButtons(currentMode);
    lastSentMode = currentMode;
  }
  
  // Update temperature only if changed significantly
  if (fabs(currentTemperature - lastSentTemperature) >= 0.5) {
    Blynk.virtualWrite(BLYNK_VPIN_TEMPERATURE, currentTemperature);
    lastSentTemperature = currentTemperature;
  }
  
  // Update pH only if changed significantly
  if (fabs(currentPH - lastSentPH) >= 0.1) {
    Blynk.virtualWrite(BLYNK_VPIN_PH, currentPH);
    lastSentPH = currentPH;
  }
  
  // Update water level only if changed significantly
  if (abs(currentWaterLevel - lastSentWaterLevel) >= 2) {
    Blynk.virtualWrite(BLYNK_VPIN_WATER_LEVEL, currentWaterLevel);
    lastSentWaterLevel = currentWaterLevel;
  }
  
  // Update preset only if changed
  if (currentPreset != lastSentPreset) {
    Blynk.virtualWrite(BLYNK_VPIN_PRESET, currentPreset);
    lastSentPreset = currentPreset;
  }
  
    // Update filter status if changed
  if (currentFilterStatus != lastSentFilterStatus) {
    Blynk.virtualWrite(BLYNK_VPIN_FILTER_STATUS, currentFilterStatus);
    Blynk.virtualWrite(BLYNK_VPIN_FILTER_TOGGLE, currentFilterStatus);
    lastSentFilterStatus = currentFilterStatus;
  }
  
  // Update water change status if changed
  if (currentWCStatus != lastSentWCStatus) {
    Blynk.virtualWrite(BLYNK_VPIN_WC_STATUS, currentWCStatus);
    lastSentWCStatus = currentWCStatus;
  }
  
  // Use rotating status instead of fixed status
  updateRotatingStatus();
  
  lastBlynkUpdate = currentTime;
}

// Update Blynk mode buttons to show current active mode
void updateBlynkModeButtons(int activeMode) {
  // Reset all toggle switches based on current mode
  switch(activeMode) {
    case 0: // Sunrise
      Blynk.virtualWrite(BLYNK_VPIN_SUNRISE_SUNSET, 0);
      Blynk.virtualWrite(BLYNK_VPIN_MOONLIGHT_CLOUDY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_THUNDER_RAINY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_FOG_RGB, 0);
      Blynk.virtualWrite(BLYNK_VPIN_OFF_MODE, 0);
      break;
    case 1: // Sunset
      Blynk.virtualWrite(BLYNK_VPIN_SUNRISE_SUNSET, 1);
      Blynk.virtualWrite(BLYNK_VPIN_MOONLIGHT_CLOUDY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_THUNDER_RAINY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_FOG_RGB, 0);
      Blynk.virtualWrite(BLYNK_VPIN_OFF_MODE, 0);
      break;
    case 2: // Moonlight
      Blynk.virtualWrite(BLYNK_VPIN_SUNRISE_SUNSET, 0);
      Blynk.virtualWrite(BLYNK_VPIN_MOONLIGHT_CLOUDY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_THUNDER_RAINY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_FOG_RGB, 0);
      Blynk.virtualWrite(BLYNK_VPIN_OFF_MODE, 0);
      break;
    case 3: // Cloudy
      Blynk.virtualWrite(BLYNK_VPIN_SUNRISE_SUNSET, 0);
      Blynk.virtualWrite(BLYNK_VPIN_MOONLIGHT_CLOUDY, 1);
      Blynk.virtualWrite(BLYNK_VPIN_THUNDER_RAINY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_FOG_RGB, 0);
      Blynk.virtualWrite(BLYNK_VPIN_OFF_MODE, 0);
      break;
    case 4: // Thunder
      Blynk.virtualWrite(BLYNK_VPIN_SUNRISE_SUNSET, 0);
      Blynk.virtualWrite(BLYNK_VPIN_MOONLIGHT_CLOUDY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_THUNDER_RAINY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_FOG_RGB, 0);
      Blynk.virtualWrite(BLYNK_VPIN_OFF_MODE, 0);
      break;
    case 5: // Rainy
      Blynk.virtualWrite(BLYNK_VPIN_SUNRISE_SUNSET, 0);
      Blynk.virtualWrite(BLYNK_VPIN_MOONLIGHT_CLOUDY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_THUNDER_RAINY, 1);
      Blynk.virtualWrite(BLYNK_VPIN_FOG_RGB, 0);
      Blynk.virtualWrite(BLYNK_VPIN_OFF_MODE, 0);
      break;
    case 6: // Fog
      Blynk.virtualWrite(BLYNK_VPIN_SUNRISE_SUNSET, 0);
      Blynk.virtualWrite(BLYNK_VPIN_MOONLIGHT_CLOUDY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_THUNDER_RAINY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_FOG_RGB, 0);
      Blynk.virtualWrite(BLYNK_VPIN_OFF_MODE, 0);
      break;
    case 7: // RGB
      Blynk.virtualWrite(BLYNK_VPIN_SUNRISE_SUNSET, 0);
      Blynk.virtualWrite(BLYNK_VPIN_MOONLIGHT_CLOUDY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_THUNDER_RAINY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_FOG_RGB, 1);
      Blynk.virtualWrite(BLYNK_VPIN_OFF_MODE, 0);
      break;
    case 8: // Off
      Blynk.virtualWrite(BLYNK_VPIN_SUNRISE_SUNSET, 0);
      Blynk.virtualWrite(BLYNK_VPIN_MOONLIGHT_CLOUDY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_THUNDER_RAINY, 0);
      Blynk.virtualWrite(BLYNK_VPIN_FOG_RGB, 0);
      Blynk.virtualWrite(BLYNK_VPIN_OFF_MODE, 1);
      break;
  }
  
  // Update status display
  String status = "Mode: " + String(MODE_MAP[activeMode]) + " | Bright: " + String(currentBrightness) + "%";
  Blynk.virtualWrite(BLYNK_VPIN_STATUS, status);
}

// ========== BLYNK CONNECTION HANDLING ==========

// ========== IMPROVED BLYNK CONNECTION HANDLING ==========

// ========== FIXED BLYNK CONNECTION HANDLING ==========

BLYNK_CONNECTED() {
  Serial.println("Blynk connected successfully");
  
  // Request initial status once when connected
  sendSerialCommand("GET_STATUS");
  
  // Initial update of all mode buttons
  updateBlynkModeButtons(currentMode);
  
  // Show connection status with mode name
  String modeName = MODE_MAP[currentMode];
  String status = "✅ Blynk Connected | " + modeName;
  if (currentMode == 7) { // RGB Mode
    String presetName = presetNames[currentPreset];
    status = "✅ Blynk Connected | 🎨 " + presetName;
  }
  Blynk.virtualWrite(BLYNK_VPIN_STATUS, status);
  
  Serial.println("Blynk: Connection established and synchronized");
}

BLYNK_DISCONNECTED() {
  Serial.println("Blynk disconnected - automatic reconnection in progress");
  
  // Don't update status here - let Blynk handle reconnection silently
  // The library will automatically attempt to reconnect
}

// ---------- Enhanced WiFi Event Handler ----------
void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case SYSTEM_EVENT_STA_GOT_IP:
      Serial.print("✅ WiFi connected! IP address: ");
      Serial.println(WiFi.localIP());
      wifiConnected = true;
      wifiReconnectCount = 0; // Reset counter on successful connection
      
      // Send IP to Mega TFT immediately when connected
      Serial1.print("WIFI_IP:");
      Serial1.println(WiFi.localIP().toString());
      
      // Update Blynk status
      if (Blynk.connected()) {
        Blynk.virtualWrite(BLYNK_VPIN_STATUS, "🟢 WiFi Connected | IP: " + WiFi.localIP().toString());
      }
      break;
      
    case SYSTEM_EVENT_STA_DISCONNECTED:
      Serial.println("❌ WiFi disconnected");
      wifiConnected = false;
      
      // Update Blynk status if connected
      if (Blynk.connected()) {
        Blynk.virtualWrite(BLYNK_VPIN_STATUS, "🔴 WiFi Disconnected - Reconnecting...");
      }
      
      // Force a reconnection attempt
      lastWiFiReconnectAttempt = 0; // This will trigger immediate reconnection attempt
      break;
      
    case SYSTEM_EVENT_STA_CONNECTED:
      Serial.println("📡 WiFi connected to AP, waiting for IP...");
      break;
      
    case SYSTEM_EVENT_STA_LOST_IP:
      Serial.println("⚠️ WiFi lost IP address");
      wifiConnected = false;
      break;
      
    default:
      break;
  }
}

// ---------- Robust WiFi Reconnection Function ----------
bool reconnectWiFi() {
  if (wifiReconnectCount >= MAX_WIFI_RECONNECT_ATTEMPTS) {
    Serial.println("❌ Max WiFi reconnection attempts reached. Restarting ESP...");
    delay(1000);
    ESP.restart(); // Hard reset if too many failures
    return false;
  }
  
  Serial.print("🔄 Attempting WiFi reconnection (attempt ");
  Serial.print(wifiReconnectCount + 1);
  Serial.print(" of ");
  Serial.print(MAX_WIFI_RECONNECT_ATTEMPTS);
  Serial.println(")");
  
  // Disconnect first to clean up any existing connection
  WiFi.disconnect();
  delay(1000);
  
  // Get stored credentials
  String ssid = prefGetWifiSsid();
  String password = prefGetWifiPass();
  
  Serial.print("Connecting to: ");
  Serial.println(ssid);
  
  // Begin connection with timeout
  WiFi.begin(ssid.c_str(), password.c_str());
  
  // Wait for connection with timeout
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    wifiReconnectCount = 0;
    Serial.println("\n✅ WiFi reconnected successfully!");
    return true;
  } else {
    wifiReconnectCount++;
    Serial.println("\n❌ WiFi reconnection failed");
    return false;
  }
}

// ---------- WiFi Connection Monitor ----------
void checkWiFiConnection() {
  unsigned long currentMillis = millis();
  
  // Check if WiFi is supposed to be connected but isn't
  if (!wifiConnected && WiFi.status() == WL_CONNECTED) {
    // We have a connection but our flag wasn't set
    wifiConnected = true;
    Serial.println("✅ WiFi connection restored (state sync)");
    
    // Send IP to Mega TFT
    Serial1.print("WIFI_IP:");
    Serial1.println(WiFi.localIP().toString());
  }
  
  // If WiFi is disconnected and it's time to attempt reconnection
  if (WiFi.status() != WL_CONNECTED && 
      currentMillis - lastWiFiReconnectAttempt >= WIFI_RECONNECT_INTERVAL) {
    
    lastWiFiReconnectAttempt = currentMillis;
    
    if (!reconnectWiFi()) {
      Serial.print("WiFi reconnection failed. Next attempt in ");
      Serial.print(WIFI_RECONNECT_INTERVAL / 1000);
      Serial.println(" seconds");
    }
  }
}

void setupNTP() {
  timeClient.begin();
  timeClient.setTimeOffset(10800); // UTC time
  Serial.println("NTP client started with local timezone");
  
  // Force initial time update
  timeClient.forceUpdate();
  delay(1000);
}

String getCurrentTimeString() {
  // Force update and wait a bit for NTP response
  timeClient.forceUpdate();
  delay(500);
  
  if (!timeClient.update()) {
    Serial.println("Failed to update NTP time on demand");
    // Try one more time
    timeClient.forceUpdate();
    delay(500);
  }
  
  unsigned long epochTime = timeClient.getEpochTime();
  
  // Check if epoch time is reasonable (after 2020)
  if (epochTime < 1577836800) { // Before 2020
    Serial.println("NTP time appears invalid: " + String(epochTime));
    return "";
  }
  
  // Convert epoch time to local time structure
  time_t rawTime = (time_t)epochTime;
  struct tm *ptm = localtime(&rawTime);
  
  char timeString[20];
  snprintf(timeString, sizeof(timeString), "%04d-%02d-%02dT%02d:%02d",
           ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
           ptm->tm_hour, ptm->tm_min);
  
  Serial.println("Generated time string: " + String(timeString));
  return String(timeString);
}

// Get formatted time for display
String getFormattedLocalTime() {
  if (!timeClient.update()) {
    timeClient.forceUpdate();
  }
  
  // Get the formatted time string from NTPClient
  String formattedTime = timeClient.getFormattedTime();
  
  // Get the local date
  unsigned long epochTime = timeClient.getEpochTime();
  time_t rawTime = (time_t)epochTime;
  struct tm *ptm = localtime(&rawTime);
  
  char dateString[20];
  snprintf(dateString, sizeof(dateString), "%04d-%02d-%02d",
           ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
  
  return String(dateString) + " " + formattedTime;
}

bool isTimeValid() {
  timeClient.forceUpdate();
  unsigned long epochTime = timeClient.getEpochTime();
  bool valid = (epochTime > 1577836800); // Basic validity check (after 2020)
  Serial.println("Time validity check: " + String(valid) + " (epoch: " + String(epochTime) + ")");
  return valid;
}

// Enhanced time sync with retry logic
void syncTimeWithMega() {
  if (!isTimeValid()) {
    Serial.println("NTP time not valid, cannot sync");
    wsBroadcast("TIME_SYNC_ERROR:NTP time not available");
    return;
  }
  
  String currentTime = getCurrentTimeString();
  if (currentTime.length() == 0) {
    Serial.println("Failed to get valid time string");
    return;
  }
  
  // Send command directly to Mega2560
  Serial1.print("SET_DATETIME:");
  Serial1.println(currentTime);
  
  Serial.println("Syncing local time with Mega: " + currentTime);
  
  // Success
  wsBroadcast("TIME_SYNC:Success - " + currentTime);
  
  // FIXED: Use Blynk.connected() instead of blynkConnected variable
  if (Blynk.connected()) {
    Blynk.virtualWrite(BLYNK_VPIN_STATUS, "🕒 Time Synced: " + currentTime.substring(11));
  }
  Serial.println("Time sync successful");
}

// Handle time sync requests from Mega2560
void handleTimeSyncRequest() {
  Serial.println("Received time sync request from Mega2560");
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot sync time - WiFi disconnected");
    wsBroadcast("TIME_SYNC_ERROR:WiFi disconnected");
    return;
  }
  
  syncTimeWithMega();
}

// Check if time sync is needed (call this periodically)
void checkTimeSync() {
  static unsigned long lastTimeCheck = 0;
  const unsigned long TIME_CHECK_INTERVAL = 10 * 60 * 1000; // Check 10 minutes
  
  if (millis() - lastTimeCheck > TIME_CHECK_INTERVAL) {
    lastTimeCheck = millis();
    
    if (WiFi.status() == WL_CONNECTED && isTimeValid()) {
      // Only auto-sync during first hour after boot or if specifically requested
      static bool firstSyncDone = false;
      if (!firstSyncDone) {
        syncTimeWithMega();
        firstSyncDone = true;
      }
    }
  }
}

// Manual time sync command handler
void handleTimeSync() {
  if (WiFi.status() == WL_CONNECTED && isTimeValid()) {
    syncTimeWithMega();
    
    // Return formatted local time in response
    String localTime = getFormattedLocalTime();
    server.send(200, "text/plain", "Time sync initiated: " + localTime);
  } else {
    server.send(400, "text/plain", "Cannot sync time - WiFi disconnected or NTP unavailable");
  }
}

// Helper - send a compact binary command: [cmd_byte][value_byte][\n]
void sendBinaryCmd(Stream &mega, uint8_t cmdByte, uint8_t valueByte) {
  mega.write(cmdByte);
  mega.write(valueByte);
  mega.write('\n'); // Mega's binary handler expects a terminator to flush
}

// Lookup mode index by name (case-insensitive). Returns -1 if not found.
int8_t modeNameToIndex(const String &name) {
  String s = name;
  s.toUpperCase();
  for (uint8_t i=0; i<MODE_COUNT; ++i) {
    String m = MODE_MAP[i];
    m.toUpperCase();
    if (m == s) return i;
  }
  return -1;
}
// ---------- OTA Setup Function ----------
void setupOTA() {
  // Set OTA password (optional - you can remove this line for no password)
  ArduinoOTA.setPassword("admin123");
  
  // Set hostname
  ArduinoOTA.setHostname("AquaSmartController");
  
  // OTA callbacks
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }
    Serial.println("Start updating " + type);
    // Notify WebSocket clients
    wsBroadcast("OTA Update Started");
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA Update Complete");
    wsBroadcast("OTA Update Complete - Restarting");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
      wsBroadcast("OTA Error: Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
      wsBroadcast("OTA Error: Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
      wsBroadcast("OTA Error: Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
      wsBroadcast("OTA Error: Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
      wsBroadcast("OTA Error: End Failed");
    }
  });
  
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
}

// ----------  setup function ----------
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(50);
  prefs.begin(PREF_NAMESPACE, false);
  ensurePrefsDefaults();

  // Register WiFi event handler BEFORE connecting
  WiFi.onEvent(WiFiEvent);

  // Better WiFi configuration for stability
  WiFi.setSleep(false); // Disable WiFi sleep for better stability
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  
  Serial1.begin(SERIAL_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
  delay(50);
  // Test Serial1 connection
  Serial1.println("PING");
  Serial.println("Sent PING to Mega2560");
  
  // Wait for response
  unsigned long start = millis();
  while (millis() - start < 2000) {
    if (Serial1.available()) {
      String response = Serial1.readString();
      Serial.println("Mega2560 response: " + response);
      break;
    }
  }
  jwtSecret = prefs.getString("jwt_secret", "");
  if (jwtSecret.length()==0) {
    jwtSecret = String(millis(), HEX) + String(random(0xFFFF), HEX);
    prefs.putString("jwt_secret", jwtSecret);
  }

  tryConnectStoredWifi();

  // Initialize Blynk
Blynk.config(BLYNK_AUTH_TOKEN);
// Connect with 10 second timeout
Blynk.connect(10000); // 10 second connection timeout

// Check initial connection
if (Blynk.connected()) {
  Serial.println("Blynk connected successfully during setup");
} else {
  Serial.println("Blynk initial connection failed - will auto-reconnect");
}

  // Setup OTA
  setupOTA();

   // Initialize NTP client after WiFi connection
  if (WiFi.status() == WL_CONNECTED) {
    setupNTP();
    
    // Initial time sync after 8 seconds (wait for NTP to get time)
    delay(8000);
    if (isTimeValid()) {
      syncTimeWithMega();
    }
  }
  
  // routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/logout", HTTP_POST, handleLogout);
  server.on("/api/cmd", HTTP_POST, handleApiCmd);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/wifi", HTTP_GET, handleWifiPage);
  server.on("/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/wifi/save", HTTP_POST, handleWifiSave);
  server.on("/change-creds", HTTP_GET, handleCredsPage);
  server.on("/change-creds", HTTP_POST, handleCredsSave);

    // OTA Update routes - NO AUTHENTICATION REQUIRED
  server.on("/ota", HTTP_GET, handleOTAPage);
  server.on("/update", HTTP_GET, handleOTAPage);
  server.on("/update", HTTP_POST, handleOTAUpdate, handleOTAUpload);

  server.begin();
  Serial.println("HTTP server started");
  Serial.println("OTA Update available at: http://" + WiFi.localIP().toString() + "/ota");

  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
  Serial.printf("WebSocket started on port %d\n", WS_PORT);
}


void loop() {
  // Handle web server clients
  server.handleClient();
  
  // Handle WebSocket connections
  webSocket.loop();

  // Update NTP client
  timeClient.update();
  
  // Check for periodic time sync
  checkTimeSync();
  
  // ALWAYS run Blynk - it handles reconnection automatically
  Blynk.run();
  
  // Handle OTA updates
  ArduinoOTA.handle();
  
  // Improved Mega2560 communication
  handleMegaCommunication();
  
  // ==== Monitor and maintain WiFi connection ====
  checkWiFiConnection();
  
  // Periodic tasks
  static unsigned long lastKeep = 0;
  if (millis() - lastKeep > 15000) { // Every 15 seconds
    lastKeep = millis();
    
    // WebSocket keepalive
    String ka = "KEEPALIVE:" + String(millis());
    webSocket.broadcastTXT(ka);
    
    // Sync status with Blynk every 15 seconds (only if connected)
    if (Blynk.connected()) {
      sendSerialCommand("GET_STATUS");
      
      // Update Blynk status display
      updateRotatingStatus();
    }
    
    // Print connection status for debugging
    Serial.print("WiFi: ");
    Serial.print(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    Serial.print(" | Blynk: ");
    Serial.println(Blynk.connected() ? "Connected" : "Disconnected");
    
    // Print WiFi signal strength if connected
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("WiFi RSSI: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
    }
  }
  
  // Update Blynk values periodically (only if connected)
  if (Blynk.connected()) {
    updateBlynkValues();
  }
  
  // Small delay to prevent watchdog timer issues
  delay(10);
}
