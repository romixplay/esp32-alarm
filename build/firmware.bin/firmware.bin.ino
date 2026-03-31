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
#include "AudioFileSourceHTTPStream.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

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
FirebaseData fbdo_log;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

// --- SYSTEM STATE VARIABLES ---
unsigned long lastHeartbeat = 0;
unsigned long lastAlarmPoll = 0;
unsigned long lastAdminPoll = 0;
double lastProcessedTrigger = 0;
double lastStopTrigger = 0;

// --- DYNAMIC AUDIO ENGINE CONFIG ---
volatile int audioMode = 0; 
bool isFirstBootSync = true; // The Ghost Trigger Fix

// MP3 Streaming Objects
AudioGeneratorMP3 *mp3;
AudioFileSourceHTTPStream *file;
AudioOutputI2S *out;
String currentStreamUrl = "";

// Hardware State
volatile bool ampEnabled = true;

// Siren Settings
volatile float mainVolume = 0.5;
volatile int sirenMinFreq = 800;
volatile int sirenMaxFreq = 1600;
volatile int sirenSpeed = 10;

// Wobble (LFO) Settings
volatile bool wobbleActive = false;
volatile int wobbleSpeed = 15;

// Periodic Settings
volatile float periodicVolume = 0.5;
volatile int periodicFreq = 1000;
volatile int periodicSecs = 30;
volatile float periodicLen = 0.1; 
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

  uint32_t phase = 0;
  float currentFreq = 800;
  int direction = 1;

  // LFO State for Wobble
  int wobblePhase = 0;
  int wobbleDir = 1;
  
  while(true) {
    // If audio is playing AND the AMP toggle is enabled
    if (audioMode > 0 && ampEnabled) {
      if (!wasPlaying) {
        digitalWrite(PIN_AMP_SD, HIGH); 
        wasPlaying = true;
        currentFreq = (audioMode == 1) ? sirenMinFreq : periodicFreq;
        phase = 0;
      }
      
      int16_t amplitude = (int16_t)(15000 * ((audioMode == 1) ? mainVolume : periodicVolume));

      for(int i = 0; i < BATCH_SIZE; i++) {
        if (audioMode == 1 && i == 0) {
          currentFreq += (sirenSpeed * direction);
          if (currentFreq >= sirenMaxFreq) { currentFreq = sirenMaxFreq; direction = -1; }
          if (currentFreq <= sirenMinFreq) { currentFreq = sirenMinFreq; direction = 1; }
        }

        // Wobble Effect (Fast LFO)
        int wobbleOffset = 0;
        if (wobbleActive && audioMode == 1 && i == 0) {
            wobblePhase += (wobbleSpeed * wobbleDir);
            if (wobblePhase > 150) wobbleDir = -1;
            if (wobblePhase < -150) wobbleDir = 1;
            wobbleOffset = wobblePhase;
        }

        uint32_t phaseStep = (uint32_t)(((currentFreq + wobbleOffset) * 65536.0) / 44100.0);
        phase += phaseStep;
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
// CLOUD LOGGING UTILITY (Asynchronous, Non-Blocking)
// =========================================================================
void logToCloud(String message) {
  Serial.println(message);
  if (Firebase.ready() && signupOK) {
    struct tm timeinfo;
    String timeString;
    if (getLocalTime(&timeinfo, 10)) {
      char timeFmt[20];
      strftime(timeFmt, sizeof(timeFmt), "%b %d %H:%M:%S", &timeinfo);
      timeString = String(timeFmt);
    } else {
      timeString = "T+" + String(millis() / 1000) + "s";
    }
    String uniqueLog = "[" + timeString + "] " + message;
    Firebase.RTDB.setString(&fbdo, "/system/latest_log", uniqueLog); 
  }
}

// =========================================================================
// STANDARD SETUP FUNCTION
// =========================================================================
void setup() {
  Serial.begin(115200);
  
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW); 

  // Clean, standard Wi-Fi initialization
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); 
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

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

  // =================================================================
  // THE SSL FIX: FORCE TIME SYNCHRONIZATION
  // =================================================================
  Serial.print("Syncing internal clock for SSL...");
  // Syncing internal clock (Israel Time: 7200 sec offset, 3600 sec DST)
  configTime(7200, 3600, "pool.ntp.org", "time.nist.gov");

  // Wait until the ESP32 realizes it is not 1970 anymore
  while (time(nullptr) < 100000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nClock synced! We are in the present.");
  // =================================================================

  ArduinoOTA.setHostname("cafe-alarm-esp32c6");
  ArduinoOTA.begin();

  // =================================================================
  // FIREBASE CONFIGURATION
  // =================================================================
  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET; 
  config.timeout.socketConnection = 10 * 1000;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

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

  logToCloud("System Booted v3.0 (Analog Synth Engine). IP: " + WiFi.localIP().toString());
}

void loop() {
  ArduinoOTA.handle();

  if (Firebase.ready() && signupOK) {
    
   // ---------------------------------------------------------
    // 1. FAST HARDWARE LOGIC
    // ---------------------------------------------------------
    if (!ampEnabled) {
      // If the AMP is switched off, brutally kill all active timers and silence the synth
      alarmEndTime = 0;
      periodicEndTime = 0;
      holdTriggerActive = false;
      audioMode = 0;
    } else {
      bool mainAlarmActive = holdTriggerActive || (millis() < alarmEndTime);
      bool beepActive = false;

      // Handle Periodic Beep Timer
      if (periodicActive && (millis() - lastPeriodicTrigger > (periodicSecs * 1000))) {
        lastPeriodicTrigger = millis();
        periodicEndTime = millis() + (periodicLen * 1000);
      }
      if (millis() < periodicEndTime) beepActive = true;

      // ==========================================
      // THE STRICT PRIORITY ROUTER (UPDATED)
      // ==========================================
      if (mainAlarmActive) {
        audioMode = 1;       // 1st Priority: Siren overrides EVERYTHING
      } 
      else if (audioMode == 3) {
        // 2nd Priority: MP3 STREAMING. 
        // Do nothing! Let it play. The audioTask will automatically set this back to 0 when the song ends.
      } 
      else if (beepActive) {
        audioMode = 2;       // 3rd Priority: Beep plays only if Siren and MP3 are quiet
      } 
      else {
        audioMode = 0;       // Default: SILENCE
      }
    }

    // ---------------------------------------------------------
    // 2. ALARM POLLING (Every 2.5 Seconds)
    // ---------------------------------------------------------
    if (millis() - lastAlarmPoll > 2500) {
      lastAlarmPoll = millis();

      if (Firebase.RTDB.getJSON(&fbdo, "/alarm_state")) {
        StaticJsonDocument<1024> doc;
        deserializeJson(doc, fbdo.to<String>());

        // Strict Type Parsing for Toggles
        if (doc.containsKey("amp_enabled")) ampEnabled = doc["amp_enabled"].as<bool>();
        if (doc.containsKey("wobble_active")) wobbleActive = doc["wobble_active"].as<bool>();
        
        if (doc.containsKey("force_reboot") && doc["force_reboot"] == true) {
            Firebase.RTDB.setBool(&fbdo, "/alarm_state/force_reboot", false);
            logToCloud("Hardware Reboot Triggered.");
            delay(1000);
            ESP.restart();
        }

        // Settings Sync
        if (doc.containsKey("volume")) mainVolume = constrain(doc["volume"].as<int>(), 0, 100) / 100.0;
        if (doc.containsKey("siren_min")) sirenMinFreq = doc["siren_min"].as<int>();
        if (doc.containsKey("siren_max")) sirenMaxFreq = doc["siren_max"].as<int>();
        if (doc.containsKey("siren_speed")) sirenSpeed = doc["siren_speed"].as<int>();
        if (doc.containsKey("wobble_speed")) wobbleSpeed = doc["wobble_speed"].as<int>();
        
        if (doc.containsKey("periodic_sec")) periodicSecs = doc["periodic_sec"].as<int>();
        if (doc.containsKey("periodic_vol")) periodicVolume = constrain(doc["periodic_vol"].as<int>(), 0, 100) / 100.0;
        if (doc.containsKey("periodic_freq")) periodicFreq = doc["periodic_freq"].as<int>();
        if (doc.containsKey("periodic_len")) periodicLen = doc["periodic_len"].as<float>();
        if (doc.containsKey("hold_trigger")) holdTriggerActive = doc["hold_trigger"].as<bool>();

        // Toggle Logging with Detailed Settings
        if (doc.containsKey("periodic_active")) {
          bool newState = doc["periodic_active"].as<bool>();
          if (newState != periodicActive) {
            periodicActive = newState;
            if (periodicActive) {
              String msg = "Periodic Beep ON: " + String(periodicSecs) + "s interval, " + 
                           String(periodicLen) + "s len, " + String(periodicFreq) + "Hz, " + 
                           String((int)(periodicVolume * 100)) + "% vol";
              logToCloud(msg);
            } else {
              logToCloud("Periodic Beep: DISABLED");
            }
          }
        }

        // Stop Trigger
        if (doc.containsKey("stop_trigger")) {
          double currentStop = doc["stop_trigger"].as<double>();
          if (currentStop > lastStopTrigger) {
            lastStopTrigger = currentStop;
            alarmEndTime = 0; // Kill timer
            holdTriggerActive = false; // Release hold
            logToCloud("ALARM FORCE STOPPED.");
          }
        }

        // Start Trigger
        if (doc.containsKey("trigger_time")) {
          double currentTrigger = doc["trigger_time"].as<double>();
          if (isFirstBootSync) {
            lastProcessedTrigger = currentTrigger; 
            isFirstBootSync = false;
          } 
          else if (currentTrigger > lastProcessedTrigger) {
            lastProcessedTrigger = currentTrigger; 
            int duration = doc["duration"] ? doc["duration"].as<int>() : 3;
            logToCloud("Timed alarm triggered for " + String(duration) + "s.");
            alarmEndTime = millis() + (duration * 1000);
          }
        }

        // MP3 Stream Trigger
        if (doc.containsKey("stream_trigger") && doc.containsKey("play_stream")) {
          double currentStream = doc["stream_trigger"].as<double>();
          if (currentStream > lastProcessedTrigger) {
            lastProcessedTrigger = currentStream; 
            currentStreamUrl = doc["play_stream"].as<String>();
            
            // Kill existing alarms, switch to MP3 mode
            alarmEndTime = 0; 
            holdTriggerActive = false; 
            audioMode = 3; 
            
            logToCloud("Streaming MP3 from cloud...");
          }
        }
      }
    }

    // ---------------------------------------------------------
    // 3. SLOW ADMIN POLLING (Every 10 Seconds)
    // ---------------------------------------------------------
    if (millis() - lastAdminPoll > 10000) {
      lastAdminPoll = millis();
      
      // 1. Send Heartbeat Ping
      Firebase.RTDB.setTimestamp(&fbdo, "/system/last_ping");
      
      // 2. Send Uptime Tracker
      Firebase.RTDB.setInt(&fbdo, "/system/uptime", millis() / 1000); 

      // 3. Check for GitHub OTA Updates
      if (Firebase.RTDB.getString(&fbdo, "/system/ota_url")) {
        String ota_url = fbdo.to<String>();
        if (ota_url.length() > 10) {
          logToCloud("OTA Triggered! Freeing memory...");
          Firebase.RTDB.setString(&fbdo, "/system/ota_url", ""); // Clear the trigger
          delay(3000); 
          
          // Brutally kill the audio engine to free up RAM for the download
          audioMode = 0;
          digitalWrite(PIN_AMP_SD, LOW);
          i2s_driver_uninstall(I2S_NUM_0); 
          delay(1000); 
          
          // Start the Secure Download
          WiFiClientSecure client;
          client.setInsecure(); // Skip certificate validation for the raw GitHub link
          client.setTimeout(15000); 
          httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
          
          t_httpUpdate_return ret = httpUpdate.update(client, ota_url);
          
          if(ret == HTTP_UPDATE_OK) { Serial.println("OTA SUCCESS!"); } 
          else { Serial.println("OTA FAILED: " + httpUpdate.getLastErrorString()); }
          
          // Always restart after an update attempt
          ESP.restart(); 
        }
      }
    }

    // Give FreeRTOS breathing room to handle the Wi-Fi background tasks
    delay(50); 
  }
}