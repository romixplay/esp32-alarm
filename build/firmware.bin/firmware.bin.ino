#include <WiFi.h>
#include <WiFiMulti.h>
#include <Firebase_ESP_Client.h>
#include <ArduinoOTA.h>
#include <driver/i2s.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>

#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- CREDENTIALS ---
WiFiMulti wifiMulti; // Create the Multi-WiFi object
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
unsigned long lastProcessedTrigger = 0;
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

// --- OPTIMIZED AUDIO BUFFER ---
const int AUDIO_BUFFER_SIZE = 1000;
int16_t precalculatedAudio[AUDIO_BUFFER_SIZE];

void updateAudioBuffer() {
  for(int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
    int rawWave = (i % 20 < 10) ? 15000 : -15000; 
    precalculatedAudio[i] = (int16_t)(rawWave * currentVolume);
  }
}

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
// FREERTOS AUDIO TASK (Zero-Math Optimization)
// =========================================================================
void audioTask(void * pvParameters) {
  size_t bytes_written;
  bool wasPlaying = false; 
  
  while(true) {
    if (isPlaying) {
      if (!wasPlaying) {
        digitalWrite(PIN_AMP_SD, HIGH); 
        wasPlaying = true;
      }
      
      // Blast the pre-computed memory directly to the hardware. No math required!
      i2s_write(I2S_NUM_0, precalculatedAudio, sizeof(precalculatedAudio), &bytes_written, portMAX_DELAY);
      
      // Let the Wi-Fi radio breathe
      vTaskDelay(2 / portTICK_PERIOD_MS); 
      
    } else {
      if (wasPlaying) {
        digitalWrite(PIN_AMP_SD, LOW); 
        i2s_zero_dma_buffer(I2S_NUM_0); 
        wasPlaying = false;
      }
      vTaskDelay(100 / portTICK_PERIOD_MS); 
    }
  }
}
// =========================================================================

unsigned long lastAlarmPoll = 0;
unsigned long lastAdminPoll = 0;

void setup() {
  Serial.begin(115200);
  
  // 1. Initialize the static sound wave immediately
  updateAudioBuffer(); 

  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW); 

  // 2. Clean, standard Wi-Fi initialization
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Prevent the antenna from micro-sleeping

  // --- START MULTI-WIFI SETUP ---
  wifiMulti.addAP("Sadan", "shamanshaman");
  wifiMulti.addAP("Ra", "88888888");
  wifiMulti.addAP("Dekel26", "100200300");
  wifiMulti.addAP("Untitled Cafe - 5Ghz", "onemorecup"); 

  Serial.print("Connecting to Wi-Fi");
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  
  Serial.println("\nWi-Fi Connected!");
  Serial.printf("Free RAM: %d bytes\n", ESP.getFreeHeap());
  Serial.print("Network: ");
  Serial.println(WiFi.SSID()); 
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  ArduinoOTA.setHostname("cafe-alarm-esp32c6");
  ArduinoOTA.begin();

  // =================================================================
  // FIREBASE CONFIGURATION (Clean & Default)
  // =================================================================
  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET; 
  config.timeout.socketConnection = 10 * 1000;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // THE FIX: Give Firebase 2 full seconds to warm up the SSL socket in the background
  Serial.println("Warming up secure cloud connection...");
  delay(2000);
  
  signupOK = true; 

  // =================================================================
  // HARDWARE CONFIGURATION
  // =================================================================
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024, 
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

  // Start the isolated, math-free Audio Task
  xTaskCreate(audioTask, "AudioTask", 4096, NULL, 1, NULL);

  // First cloud ping!
  logToCloud("System Booted v2.1 (Optimized Memory). IP: " + WiFi.localIP().toString());
}

void loop() {
  ArduinoOTA.handle();

  if (Firebase.ready() && signupOK) {
    
    // ==========================================
    // 1. FAST HARDWARE LOGIC (Runs continuously)
    // ==========================================
    if (holdTriggerActive || (millis() < timedPlayEndTime)) {
      isPlaying = true;
    } else {
      isPlaying = false;
    }

    if (periodicActive && (millis() - lastPeriodicBeep > (periodicMins * 60000))) {
      lastPeriodicBeep = millis();
      timedPlayEndTime = millis() + 1000; 
      logToCloud("Periodic beep triggered.");
    }

    // ==========================================
    // 2. URGENT ALARM POLLING (Every 1 Second)
    // ==========================================
    if (millis() - lastAlarmPoll > 1000) {
      lastAlarmPoll = millis();

      if (Firebase.RTDB.getJSON(&fbdo, "/alarm_state")) {
        StaticJsonDocument<512> doc;
        deserializeJson(doc, fbdo.to<String>());

        if (doc.containsKey("volume")) {
          int v = doc["volume"];
          float newVol = constrain(v, 0, 100) / 100.0;
          if (newVol != currentVolume) {
            currentVolume = newVol;
            updateAudioBuffer(); 
          }
        }
        if (doc.containsKey("periodic_active")) periodicActive = doc["periodic_active"];
        if (doc.containsKey("periodic_mins")) periodicMins = doc["periodic_mins"];
        if (doc.containsKey("hold_trigger")) holdTriggerActive = doc["hold_trigger"];

        if (doc.containsKey("trigger_time")) {
          unsigned long currentTrigger = doc["trigger_time"].as<unsigned long>();
          
          // Only play if this is a brand new button press we haven't seen before
          if (currentTrigger > lastProcessedTrigger) {
            lastProcessedTrigger = currentTrigger; // Remember this press
            
            int duration = doc["duration"] ? doc["duration"].as<int>() : 3;
            logToCloud("Timed alarm triggered for " + String(duration) + "s.");
            timedPlayEndTime = millis() + (duration * 1000);
          }
        }
      }
    }

    // ==========================================
    // 3. SLOW ADMIN POLLING (Every 10 Seconds)
    // ==========================================
    if (millis() - lastAdminPoll > 10000) {
      lastAdminPoll = millis();

      // Send Heartbeat Ping
      Firebase.RTDB.setTimestamp(&fbdo, "/system/last_ping");

      // Check for GitHub OTA Updates
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
    }

    // Give FreeRTOS breathing room
    delay(50); 
  }
}