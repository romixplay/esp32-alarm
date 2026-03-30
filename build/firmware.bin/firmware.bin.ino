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
unsigned long lastHeartbeat = 0;
unsigned long lastAlarmPoll = 0;
unsigned long lastAdminPoll = 0;
double lastProcessedTrigger = 0; 

// --- DYNAMIC AUDIO ENGINE CONFIG ---
volatile int audioMode = 0; // 0 = Silence, 1 = Siren, 2 = Periodic Beep

// Siren Settings
volatile float mainVolume = 0.5;
volatile int sirenMinFreq = 800;
volatile int sirenMaxFreq = 1600;
volatile int sirenSpeed = 10;

// Periodic Settings
volatile float periodicVolume = 0.5;
volatile int periodicFreq = 1000;
volatile int periodicSecs = 30;
volatile float periodicLen = 0.1; // Seconds
bool periodicActive = false;
unsigned long lastPeriodicTrigger = 0;
unsigned long periodicEndTime = 0;

// Timers
unsigned long alarmEndTime = 0;
bool holdTriggerActive = false;

// =========================================================================
// FREERTOS AUDIO TASK (Real-Time Phase Accumulator Synthesizer)
// =========================================================================
void audioTask(void * pvParameters) {
  const int BATCH_SIZE = 512;
  int16_t sample[BATCH_SIZE];
  size_t bytes_written;
  bool wasPlaying = false; 

  // Synth Engine State
  uint32_t phase = 0;
  float currentFreq = 800;
  int direction = 1;
  
  while(true) {
    if (audioMode > 0) {
      if (!wasPlaying) {
        digitalWrite(PIN_AMP_SD, HIGH); 
        wasPlaying = true;
        currentFreq = (audioMode == 1) ? sirenMinFreq : periodicFreq;
        phase = 0;
      }
      
      // Determine Amplitude based on mode
      int16_t amplitude = (int16_t)(15000 * ((audioMode == 1) ? mainVolume : periodicVolume));

      // Calculate the audio buffer
      for(int i = 0; i < BATCH_SIZE; i++) {
        
        // If it's a Siren, smoothly slide the frequency per sample batch
        if (audioMode == 1 && i == 0) {
          currentFreq += (sirenSpeed * direction);
          if (currentFreq >= sirenMaxFreq) { currentFreq = sirenMaxFreq; direction = -1; }
          if (currentFreq <= sirenMinFreq) { currentFreq = sirenMinFreq; direction = 1; }
        }

        // Fixed-point phase math (Incredibly fast, perfectly smooth)
        uint32_t phaseStep = (uint32_t)((currentFreq * 65536.0) / 44100.0);
        phase += phaseStep;
        
        // Square wave generation based on phase rollover
        sample[i] = ((phase & 0x8000) > 0) ? amplitude : -amplitude;
      }

      i2s_write(I2S_NUM_0, &sample, sizeof(sample), &bytes_written, portMAX_DELAY);
      vTaskDelay(1 / portTICK_PERIOD_MS); 
      
    } else {
      if (wasPlaying) {
        digitalWrite(PIN_AMP_SD, LOW); 
        i2s_zero_dma_buffer(I2S_NUM_0); 
        wasPlaying = false;
      }
      vTaskDelay(50 / portTICK_PERIOD_MS); 
    }
  }
}

// =========================================================================
// CLOUD LOGGING UTILITY
// =========================================================================
void logToCloud(String message) {
  Serial.println(message);
  if (Firebase.ready() && signupOK) {
    // Inject the ESP32 uptime in seconds so the string is ALWAYS unique
    String uniqueLog = "[T+" + String(millis() / 1000) + "s] " + message;
    Firebase.RTDB.setString(&fbdo, "/system/latest_log", uniqueLog);
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW); 

  // 2. Clean, standard Wi-Fi initialization
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Prevent the antenna from micro-sleeping
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N); // Force Wi-Fi 4 to prevent SSL packet corruption on your router!

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
  delay(3000);
  
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
    
    // ---------------------------------------------------------
    // 1. FAST HARDWARE LOGIC (Evaluates Audio Routing)
    // ---------------------------------------------------------
    if (holdTriggerActive || (millis() < alarmEndTime)) {
      audioMode = 1; // Play Siren
    } 
    else if (periodicActive && (millis() - lastPeriodicTrigger > (periodicSecs * 1000))) {
      lastPeriodicTrigger = millis();
      periodicEndTime = millis() + (periodicLen * 1000);
      logToCloud("Periodic beep triggered.");
    }

    // Route the periodic beep if the main alarm is quiet
    if (audioMode != 1) {
      if (millis() < periodicEndTime) audioMode = 2; // Play Beep
      else audioMode = 0; // Silence
    }

    // ---------------------------------------------------------
    // 2. ALARM POLLING (Every 1 Second)
    // ---------------------------------------------------------
    if (millis() - lastAlarmPoll > 1000) {
      lastAlarmPoll = millis();

      if (Firebase.RTDB.getJSON(&fbdo, "/alarm_state")) {
        StaticJsonDocument<1024> doc;
        deserializeJson(doc, fbdo.to<String>());

        // Sync all our new custom sliders
        if (doc.containsKey("volume")) mainVolume = constrain(doc["volume"].as<int>(), 0, 100) / 100.0;
        if (doc.containsKey("siren_min")) sirenMinFreq = doc["siren_min"];
        if (doc.containsKey("siren_max")) sirenMaxFreq = doc["siren_max"];
        if (doc.containsKey("siren_speed")) sirenSpeed = doc["siren_speed"];
        
        if (doc.containsKey("periodic_active")) periodicActive = doc["periodic_active"];
        if (doc.containsKey("periodic_sec")) periodicSecs = doc["periodic_sec"];
        if (doc.containsKey("periodic_vol")) periodicVolume = constrain(doc["periodic_vol"].as<int>(), 0, 100) / 100.0;
        if (doc.containsKey("periodic_freq")) periodicFreq = doc["periodic_freq"];
        if (doc.containsKey("periodic_len")) periodicLen = doc["periodic_len"].as<float>();
        
        if (doc.containsKey("hold_trigger")) holdTriggerActive = doc["hold_trigger"];

        // Check for new trigger
        if (doc.containsKey("trigger_time")) {
          double currentTrigger = doc["trigger_time"].as<double>();
          if (currentTrigger > lastProcessedTrigger) {
            lastProcessedTrigger = currentTrigger; 
            int duration = doc["duration"] ? doc["duration"].as<int>() : 3;
            logToCloud("Timed alarm triggered for " + String(duration) + "s.");
            alarmEndTime = millis() + (duration * 1000);
          }
        }
      }
    }

    // ---------------------------------------------------------
    // 3. SLOW ADMIN POLLING (Every 10 Seconds)
    // ---------------------------------------------------------
    if (millis() - lastAdminPoll > 10000) {
      lastAdminPoll = millis();
      Firebase.RTDB.setTimestamp(&fbdo, "/system/last_ping");

      // ... keep your existing OTA check code here ...
    }

    delay(50); 
  }
}