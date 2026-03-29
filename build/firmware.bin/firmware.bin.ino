#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <ArduinoOTA.h>
#include <driver/i2s.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- CREDENTIALS ---
#define WIFI_SSID "Sadan"
#define WIFI_PASSWORD "shamanshaman"
// REPLACE API_KEY WITH YOUR SECRET
#define DATABASE_SECRET "l8mdVlybE4p0BDRcj5Z0n2lVAToOr1oRQ8TTMu53"
#define DATABASE_URL "https://untitledcafe-bfd05-default-rtdb.europe-west1.firebasedatabase.app"

// --- HARDWARE PINS ---
static const int PIN_I2S_LRC  = 15;  
static const int PIN_I2S_BCLK = 18;  
static const int PIN_I2S_DIN  = 19;  
static const int PIN_AMP_SD   = 20;  

// --- FIREBASE OBJECTS ---
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

// --- SYSTEM STATE VARIABLES ---
unsigned long lastHeartbeat = 0;
const long heartbeatInterval = 10000;

// Audio State (Volatile because they are shared between FreeRTOS tasks)
volatile bool isPlaying = false;
volatile float currentVolume = 0.5; // 0.0 to 1.0 multiplier

// Logic State
unsigned long timedPlayEndTime = 0;
bool holdTriggerActive = false;
bool periodicActive = false;
int periodicMins = 5;
unsigned long lastPeriodicBeep = 0;


// --- CLOUD LOGGING ---
void logToCloud(String message) {
  Serial.println(message);
  if (Firebase.ready() && signupOK) {
    // Inject the ESP32 uptime in seconds so the string is ALWAYS unique
    String uniqueLog = "[T+" + String(millis() / 1000) + "s] " + message;
    Firebase.RTDB.setString(&fbdo, "/system/latest_log", uniqueLog);
  }
}

// =========================================================================
// FREERTOS AUDIO TASK (Runs independently in the background)
// =========================================================================
void audioTask(void * pvParameters) {
  uint16_t sample[64];
  size_t bytes_written;
  
  while(true) {
    if (isPlaying) {
      digitalWrite(PIN_AMP_SD, HIGH); // Turn on Amp
      
      // Generate a square wave and scale it by the current volume
      for(int i = 0; i < 64; i++) {
        float rawWave = (i % 20 < 10) ? 15000.0 : -15000.0; 
        sample[i] = (int16_t)(rawWave * currentVolume);
      }
      i2s_write(I2S_NUM_0, &sample, sizeof(sample), &bytes_written, portMAX_DELAY);
      
    } else {
      digitalWrite(PIN_AMP_SD, LOW); // Turn off Amp to prevent static
      i2s_zero_dma_buffer(I2S_NUM_0);
      vTaskDelay(10 / portTICK_PERIOD_MS); // Sleep to give CPU back to WiFi
    }
  }
}
// =========================================================================

unsigned long lastFirebasePoll = 0;

void setup() {
  Serial.begin(115200);
  
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW); 

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(300); }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  ArduinoOTA.setHostname("cafe-alarm-esp32c6");
  ArduinoOTA.begin();

  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET; // This gives the ESP32 God Mode
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  signupOK = true; // Manually flag that we are good to go

  // Setup I2S for MAX98357A
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = PIN_I2S_BCLK,
    .ws_io_num = PIN_I2S_LRC,
    .data_out_num = PIN_I2S_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_zero_dma_buffer(I2S_NUM_0);

  // Start the isolated Audio Task
  xTaskCreate(audioTask, "AudioTask", 4096, NULL, 1, NULL);

  logToCloud("System Booted v2.0 (Non-Blocking). IP: " + WiFi.localIP().toString());
}

void loop() {
  ArduinoOTA.handle();

  if (Firebase.ready() && signupOK) {
    
    // ==========================================
    // 1. FAST HARDWARE LOGIC (Runs every 50ms)
    // ==========================================
    
    // Evaluate if the alarm should be making noise right now
    if (holdTriggerActive || (millis() < timedPlayEndTime)) {
      isPlaying = true;
    } else {
      isPlaying = false;
    }

    // Periodic Beep Logic
    if (periodicActive && (millis() - lastPeriodicBeep > (periodicMins * 60000))) {
      lastPeriodicBeep = millis();
      timedPlayEndTime = millis() + 1000; 
      logToCloud("Periodic beep triggered.");
    }


    // ==========================================
    // 2. SLOW NETWORK LOGIC (Runs every 3 seconds)
    // ==========================================
    if (millis() - lastFirebasePoll > 3000) {
      lastFirebasePoll = millis();

      // A. HEARTBEAT
      if (millis() - lastHeartbeat > heartbeatInterval) {
        lastHeartbeat = millis();
        Firebase.RTDB.setTimestamp(&fbdo, "/system/last_ping");
      }

      // B. HTTP OTA CHECK
      if (Firebase.RTDB.getString(&fbdo, "/system/ota_url")) {
        String ota_url = fbdo.to<String>();
        if (ota_url.length() > 10) {
          logToCloud("OTA Triggered! Freeing memory...");
          Firebase.RTDB.setString(&fbdo, "/system/ota_url", "");
          delay(3000); 
          
          isPlaying = false;
          digitalWrite(PIN_AMP_SD, LOW);
          i2s_driver_uninstall(I2S_NUM_0); 
          delay(1000); 
          
          WiFiClientSecure client;
          client.setInsecure();
          client.setTimeout(15000); 
          httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
          
          t_httpUpdate_return ret = httpUpdate.update(client, ota_url);
          
          if(ret == HTTP_UPDATE_OK) { Serial.println("OTA SUCCESS!"); } 
          else { Serial.println("OTA FAILED: " + httpUpdate.getLastErrorString()); }
          ESP.restart(); 
        }
      }

      // C. FETCH STATE
      if (Firebase.RTDB.getJSON(&fbdo, "/alarm_state")) {
        StaticJsonDocument<512> doc;
        deserializeJson(doc, fbdo.to<String>());

        if (doc.containsKey("volume")) {
          int v = doc["volume"];
          currentVolume = constrain(v, 0, 100) / 100.0;
        }
        if (doc.containsKey("periodic_active")) periodicActive = doc["periodic_active"];
        if (doc.containsKey("periodic_mins")) periodicMins = doc["periodic_mins"];
        if (doc.containsKey("hold_trigger")) holdTriggerActive = doc["hold_trigger"];

        if (doc.containsKey("trigger") && doc["trigger"] == true) {
          int duration = doc["duration"] ? doc["duration"].as<int>() : 3;
          timedPlayEndTime = millis() + (duration * 1000);
          logToCloud("Timed alarm triggered for " + String(duration) + "s.");
          Firebase.RTDB.setBool(&fbdo, "/alarm_state/trigger", false); 
        }
      }
    }

    // Give FreeRTOS breathing room, but keep UI physically responsive
    delay(50); 
  }
}