#include <WiFi.h>
#include <LittleFS.h>
#include <WiFiMulti.h>
#include <Firebase_ESP_Client.h>
#include <ArduinoOTA.h>
#include <driver/i2s.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <Update.h> // REQUIRED FOR NATIVE HARDWARE ROLLBACK

#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- SAFE BOOT VAULT (C6 Bulletproof) ---
RTC_NOINIT_ATTR int crashCounter;
RTC_NOINIT_ATTR uint32_t rtcMagic;

// --- CREDENTIALS ---
WiFiMulti wifiMulti; 
#define DATABASE_SECRET "l8mdVlybE4p0BDRcj5Z0n2lVAToOr1oRQ8TTMu53"
#define DATABASE_URL "https://untitledcafe-bfd05-default-rtdb.europe-west1.firebasedatabase.app"

// --- HARDWARE PINS ---
static const int PIN_I2S_LRC  = 9;  
static const int PIN_I2S_BCLK = 8;  
static const int PIN_I2S_DIN  = 15;  
static const int PIN_AMP_SD   = 14;  

// --- FIREBASE OBJECTS ---
FirebaseData fbdo;          
FirebaseData streamData;    
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

// --- SYSTEM STATE VARIABLES ---
bool forceDataSync = false;     
unsigned long localAlarmStartTime = 0;
unsigned long localAlarmDuration = 0;
bool isLocalAlarmActive = false;
unsigned long lastHeartbeat = 0; 
double lastProcessedTrigger = 0;
double lastStopTrigger = 0;
double lastSlotTrigger = 0;      
bool isFirstBootSync = true;    

// Smart Slot Tracking
String currentWavPath = "/horn.wav";           
double localSlotVersions[6] = {0, 0, 0, 0, 0, 0}; 

// --- DYNAMIC AUDIO ENGINE CONFIG ---
volatile int audioMode = 0; 
File wavFile;
volatile bool ampEnabled = true;

// Settings Variables
volatile float mainVolume = 0.5;
volatile float wavVolume = 0.5; 
volatile int sirenMinFreq = 800;
volatile int sirenMaxFreq = 1600;
volatile int sirenSpeed = 10;

volatile float periodicVolume = 0.5;
volatile int periodicFreq = 1000;
volatile int periodicSecs = 30;
volatile float periodicLen = 0.1; 
bool periodicActive = false;
unsigned long lastPeriodicTrigger = 0;
unsigned long periodicEndTime = 0;

unsigned long alarmEndTime = 0;
bool holdTriggerActive = false;

// =========================================================================
// FREERTOS AUDIO TASK
// =========================================================================
void audioTask(void * pvParameters) {
  const int BATCH_SIZE = 512;
  int16_t sample[BATCH_SIZE];
  uint8_t wavBuffer[1024]; 
  size_t bytes_written;
  bool wasPlaying = false; 

  uint32_t phase = 0; float currentFreq = 800;
  int direction = 1;
  
  while(true) {
    if (audioMode > 0 && ampEnabled) {
      if (!wasPlaying) {
        digitalWrite(PIN_AMP_SD, HIGH); 
        wasPlaying = true;
        
        i2s_driver_uninstall(I2S_NUM_0); 
        
        if (audioMode == 3) {
          i2s_config_t i2s_config_wav = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX), .sample_rate = 8000, .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, .dma_buf_count = 8, .dma_buf_len = 512, .use_apll = false, .tx_desc_auto_clear = true
          };
          i2s_pin_config_t pin_config = { .bck_io_num = PIN_I2S_BCLK, .ws_io_num = PIN_I2S_LRC, .data_out_num = PIN_I2S_DIN, .data_in_num = I2S_PIN_NO_CHANGE };
          i2s_driver_install(I2S_NUM_0, &i2s_config_wav, 0, NULL);
          i2s_set_pin(I2S_NUM_0, &pin_config);
          
          wavFile = LittleFS.open(currentWavPath, FILE_READ);
          if (wavFile) wavFile.seek(44); 
          
        } else {
          i2s_config_t i2s_config_synth = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX), .sample_rate = 44100, .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, .dma_buf_count = 8, .dma_buf_len = 1024, .use_apll = false, .tx_desc_auto_clear = true
          };
          i2s_pin_config_t pin_config = { .bck_io_num = PIN_I2S_BCLK, .ws_io_num = PIN_I2S_LRC, .data_out_num = PIN_I2S_DIN, .data_in_num = I2S_PIN_NO_CHANGE };
          i2s_driver_install(I2S_NUM_0, &i2s_config_synth, 0, NULL);
          i2s_set_pin(I2S_NUM_0, &pin_config);
          
          currentFreq = (audioMode == 1) ? sirenMinFreq : periodicFreq;
          phase = 0;
        }
      }
      
      if (audioMode == 3) {
        if (wavFile && wavFile.available()) {
          size_t bytesRead = wavFile.read(wavBuffer, sizeof(wavBuffer));
          int16_t* pcmData = (int16_t*)wavBuffer;
          for(int i = 0; i < bytesRead / 2; i++) {
             pcmData[i] = pcmData[i] * wavVolume; 
          }
          i2s_write(I2S_NUM_0, wavBuffer, bytesRead, &bytes_written, portMAX_DELAY);
        } else {
          if (wavFile) wavFile.close();
          audioMode = 0; 
        }
        vTaskDelay(1 / portTICK_PERIOD_MS); 
      }
      
      else {
        int16_t amplitude = (int16_t)(15000 * ((audioMode == 1) ? mainVolume : periodicVolume));
        for(int i = 0; i < BATCH_SIZE; i++) {
          if (audioMode == 1 && i == 0) {
            currentFreq += (sirenSpeed * direction);
            if (currentFreq >= sirenMaxFreq) { currentFreq = sirenMaxFreq; direction = -1; }
            if (currentFreq <= sirenMinFreq) { currentFreq = sirenMinFreq; direction = 1; }
          }
          
          uint32_t phaseStep = (uint32_t)((currentFreq * 65536.0) / 44100.0);
          phase += phaseStep;
          sample[i] = ((phase & 0x8000) > 0) ? amplitude : -amplitude;
        }
        i2s_write(I2S_NUM_0, &sample, sizeof(sample), &bytes_written, portMAX_DELAY);
        vTaskDelay(1 / portTICK_PERIOD_MS); 
      }
      
    } else {
      if (wasPlaying) {
        digitalWrite(PIN_AMP_SD, LOW); 
        wasPlaying = false;
        if (wavFile) wavFile.close();
        i2s_zero_dma_buffer(I2S_NUM_0); 
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
    fbdo.clear(); 
  }
}

// =========================================================================
// SMART SLOT DOWNLOADER
// =========================================================================
bool downloadToSlot(String url, String filename) {
  logToCloud("Updating local file: " + filename);
  
  WiFiClientSecure client;
  client.setInsecure(); 
  HTTPClient http;
  
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
      File f = LittleFS.open(filename, FILE_WRITE);
      if (!f) {
        logToCloud("Error: LittleFS write failed for " + filename);
        return false;
      }
      http.writeToStream(&f);
      f.close();
      http.end();
      logToCloud("Download Complete: " + filename);
      return true;
    } else {
      logToCloud("HTTP Error during download: " + String(httpCode));
    }
    http.end();
  }
  return false;
}

// =========================================================================
// SETUP
// =========================================================================
void setup() {
  Serial.begin(115200);

  // =================================================================
  // 🚨 DEATH LOOP DETECTOR 🚨
  // =================================================================
  // If the magic number is missing, the board was physically unplugged 
  // from the wall or the RST button was pressed. Reset the vault!
  if (rtcMagic != 0x5A5A5A5A) {
    rtcMagic = 0x5A5A5A5A;
    crashCounter = 0;
  }

  crashCounter++;
  Serial.println("\n--- BOOT ATTEMPT: " + String(crashCounter) + " ---");

  if (crashCounter >= 3) {
    Serial.println("CRITICAL: Death Loop Detected!");
    if (Update.canRollBack()) {
        Serial.println("Hardware Rollback Available! Reverting to previous firmware...");
        Update.rollBack();
        crashCounter = 0; // Reset before jumping back in time
        ESP.restart();    // Instantly boots into the previous working build!
    } else {
        Serial.println("Rollback partition empty or invalid. Halting.");
        while(true) { delay(1000); }
    }
  }
  // =================================================================

  Serial.println("Mounting LittleFS Hard Drive...");
  if(!LittleFS.begin(true)){
    Serial.println("LittleFS Mount Failed!");
    return;
  }
  
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW); 

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); 
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

  // --- INFINITE MULTI-WIFI ROAMING ---
  wifiMulti.addAP("Sadan", "shamanshaman");
  wifiMulti.addAP("Ra", "88888888");
  wifiMulti.addAP("Untitled Cafe", "onemorecup"); 

  Serial.print("Scanning and Connecting to Wi-Fi...");
  // This will loop until the end of the world until one of the 3 networks appears
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }

  WiFi.setSleep(false);
  Serial.println("\nWi-Fi Connected!");
  Serial.print("Network: "); Serial.println(WiFi.SSID());
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());

  Serial.print("Syncing internal clock for SSL...");
  configTime(7200, 3600, "pool.ntp.org", "time.nist.gov");
  while (time(nullptr) < 100000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nClock synced!");

  ArduinoOTA.setHostname("cafe-alarm-esp32c6");
  ArduinoOTA.begin();

  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET; 
  config.timeout.socketConnection = 10 * 1000;
  config.timeout.serverResponse = 10 * 1000;

  fbdo.setBSSLBufferSize(4096, 1024); 
  fbdo.setResponseSize(2048);
  streamData.setBSSLBufferSize(2048, 1024);

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);       

  Serial.println("Warming up secure cloud connection...");
  delay(2000);
  
  if (!Firebase.RTDB.beginStream(&streamData, "/alarm_state")) {
    Serial.println("Stream Connection Failed: " + streamData.errorReason());
  } else {
    Serial.println("Stream Pipe Open! Listening for pushes...");
  }
  
  signupOK = true;

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX), .sample_rate = 44100, .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, .dma_buf_count = 8, .dma_buf_len = 1024, .use_apll = false, .tx_desc_auto_clear = true
  };
  i2s_pin_config_t pin_config = { .bck_io_num = PIN_I2S_BCLK, .ws_io_num = PIN_I2S_LRC, .data_out_num = PIN_I2S_DIN, .data_in_num = I2S_PIN_NO_CHANGE };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_zero_dma_buffer(I2S_NUM_0);

  xTaskCreate(audioTask, "AudioTask", 4096, NULL, 3, NULL);
  
  logToCloud("System Booted v7.0. Connected to: " + WiFi.SSID());
}

// =========================================================================
// MAIN LOOP
// =========================================================================
void loop() {
  ArduinoOTA.handle();

  // =========================================================================
  // THE "ALL CLEAR" SIGNAL FOR THE ROLLBACK PROTOCOL
  // =========================================================================
  if (crashCounter > 0 && millis() > 20000) {
    crashCounter = 0;
    Serial.println("System Stable. Rollback counter cleared.");
  }

  if (isLocalAlarmActive) {
    if (millis() - localAlarmStartTime >= localAlarmDuration) {
      logToCloud("Hardware Timer: Stopping audio.");
      audioMode = 0;              
      holdTriggerActive = false;  
      isLocalAlarmActive = false; 
    }
  }

  if (WiFi.status() == WL_CONNECTED && Firebase.ready() && signupOK) {
    
    if (!ampEnabled) {
      alarmEndTime = 0; periodicEndTime = 0; holdTriggerActive = false; isLocalAlarmActive = false; audioMode = 0;
    } else {
      bool mainAlarmActive = holdTriggerActive || isLocalAlarmActive;
      bool beepActive = false;

      if (periodicActive && (millis() - lastPeriodicTrigger > (unsigned long)(periodicSecs * 1000))) {
        lastPeriodicTrigger = millis(); periodicEndTime = millis() + (unsigned long)(periodicLen * 1000);
      }
      if (millis() < periodicEndTime) beepActive = true;

      if (mainAlarmActive) { audioMode = 1; } 
      else if (audioMode == 3) { /* Let WAV play */ } 
      else if (beepActive) { audioMode = 2; } 
      else { audioMode = 0; }
    }
  
    // =========================================================================
    // ZONE 3: CLOUD SYNC (ZERO-LATENCY STREAM PUSH)
    // =========================================================================
    if (Firebase.RTDB.readStream(&streamData)) {
      if (streamData.streamTimeout()) {
        Serial.println("Stream timed out, refreshing pipe...");
      }
      if (streamData.streamAvailable()) {
        forceDataSync = true; 
      }
    }

    if (forceDataSync || isFirstBootSync) {
      forceDataSync = false; 
      
      if (Firebase.RTDB.getJSON(&fbdo, "/alarm_state")) {
        StaticJsonDocument<1024> doc;
        deserializeJson(doc, fbdo.to<String>());

        // --- OTA TRIGGER ---
        if (doc.containsKey("ota_url")) {
          String ota_url = doc["ota_url"].as<String>();
          if (ota_url.length() > 10) {
            logToCloud("OTA Triggered via Stream! Downloading...");
            Firebase.RTDB.deleteNode(&fbdo, "/alarm_state/ota_url"); 
            delay(1000); 
            
            audioMode = 0; digitalWrite(PIN_AMP_SD, LOW); i2s_driver_uninstall(I2S_NUM_0); 
            WiFiClientSecure client; client.setInsecure(); client.setTimeout(15000); 
            httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            httpUpdate.update(client, ota_url);
            ESP.restart(); 
          }
        }

        if (doc.containsKey("force_reboot") && doc["force_reboot"].as<bool>() == true) {
            logToCloud("Reboot command received. Restarting...");
            Firebase.RTDB.setBool(&fbdo, "/alarm_state/force_reboot", false); 
            delay(1000); 
            ESP.restart(); 
        }

        if (doc.containsKey("amp_enabled")) ampEnabled = doc["amp_enabled"].as<bool>();
        if (doc.containsKey("volume")) mainVolume = constrain(doc["volume"].as<int>(), 0, 100) / 100.0;
        if (doc.containsKey("wav_volume")) wavVolume = constrain(doc["wav_volume"].as<int>(), 0, 100) / 100.0; 
        if (doc.containsKey("siren_min")) sirenMinFreq = doc["siren_min"].as<int>();
        if (doc.containsKey("siren_max")) sirenMaxFreq = doc["siren_max"].as<int>();
        if (doc.containsKey("siren_speed")) sirenSpeed = doc["siren_speed"].as<int>();
        
        if (doc.containsKey("periodic_sec")) periodicSecs = doc["periodic_sec"].as<int>();
        if (doc.containsKey("periodic_vol")) periodicVolume = constrain(doc["periodic_vol"].as<int>(), 0, 100) / 100.0;
        if (doc.containsKey("periodic_freq")) periodicFreq = doc["periodic_freq"].as<int>();
        if (doc.containsKey("periodic_len")) periodicLen = doc["periodic_len"].as<float>();
        if (doc.containsKey("hold_trigger")) holdTriggerActive = doc["hold_trigger"].as<bool>();

        if (doc.containsKey("periodic_active")) {
          bool newState = doc["periodic_active"].as<bool>();
          if (newState != periodicActive) {
            periodicActive = newState;
            if (periodicActive) {
              lastPeriodicTrigger = millis(); 
              periodicEndTime = millis() + (unsigned long)(periodicLen * 1000);
              String msg = "Periodic Beep ON: " + String(periodicSecs) + "s interval, " + String(periodicLen) + "s len, " + String(periodicFreq) + "Hz";
              logToCloud(msg);
            } else {
              logToCloud("Periodic Beep: DISABLED");
            }
          }
        }

        if (isFirstBootSync) {
          if (doc.containsKey("trigger_time")) lastProcessedTrigger = doc["trigger_time"].as<double>();
          if (doc.containsKey("stop_trigger")) lastStopTrigger = doc["stop_trigger"].as<double>();
          if (doc.containsKey("slot_trigger")) lastSlotTrigger = doc["slot_trigger"].as<double>();

          if (doc.containsKey("slots")) {
              for (int i = 0; i <= 4; i++) {
                String sName = (i == 0) ? "horn" : "slot" + String(i);
                if (doc["slots"].containsKey(sName)) {
                    localSlotVersions[i] = doc["slots"][sName]["version"].as<double>();
                }
              }
          }
          isFirstBootSync = false;
          Serial.println("First boot sync complete. Hardware is armed.");
        } 
        
        else {
          if (doc.containsKey("stop_trigger")) {
            double currentStop = doc["stop_trigger"].as<double>();
            if (currentStop > lastStopTrigger) {
              lastStopTrigger = currentStop;
              isLocalAlarmActive = false; holdTriggerActive = false; audioMode = 0;              
              logToCloud("ALARM FORCE STOPPED.");
            }
          }

          if (doc.containsKey("trigger_time")) {
            double currentTrigger = doc["trigger_time"].as<double>();
            if (currentTrigger > lastProcessedTrigger) {
              lastProcessedTrigger = currentTrigger; 
              int durationSecs = doc["duration"] ? doc["duration"].as<int>() : 3;
              logToCloud("Timed alarm triggered for " + String(durationSecs) + "s.");
              localAlarmDuration = durationSecs * 1000;
              localAlarmStartTime = millis();
              isLocalAlarmActive = true;
            }
          }

          if (doc.containsKey("slot_trigger") && doc.containsKey("active_slot")) {
            double currentSlotTrigger = doc["slot_trigger"].as<double>();
            if (currentSlotTrigger > lastSlotTrigger) {
              lastSlotTrigger = currentSlotTrigger;
              
              int slotID = doc["active_slot"].as<int>();
              String slotName = (slotID == 0) ? "horn" : "slot" + String(slotID);
              String path = "/" + slotName + ".wav";
              
              if (doc["slots"].containsKey(slotName)) {
                  double cloudVersion = doc["slots"][slotName]["version"].as<double>();

                  if (cloudVersion == -1) {
                    LittleFS.remove(path); localSlotVersions[slotID] = -1;
                    logToCloud("Slot " + String(slotID) + " deleted from disk.");
                  }
                  else if (cloudVersion > localSlotVersions[slotID] || !LittleFS.exists(path)) {
                    String url = doc["slots"][slotName]["url"].as<String>();
                    if (downloadToSlot(url, path)) { localSlotVersions[slotID] = cloudVersion; }
                  }
              }
              
              if (LittleFS.exists(path)) {
                  currentWavPath = path;
                  audioMode = 3; 
              }
            }
          }
        }
      }
      fbdo.clear(); 
    }

    // =========================================================================
    // ZONE 4: UNIFIED TELEMETRY (17 Second Prime Timer)
    // =========================================================================
    if (millis() - lastHeartbeat > 17000) {
      lastHeartbeat = millis();
      
      FirebaseJson hbJson;
      double localTimeMs = (double)time(nullptr) * 1000.0; 
      hbJson.set("last_ping", localTimeMs);
      hbJson.set("uptime", millis() / 1000);
      
      Firebase.RTDB.updateNode(&fbdo, "/system", &hbJson);
      fbdo.clear(); 
    }

    delay(20); 
  }
  // IF WI-FI DROPS, reconnect WiFiMulti inside the loop!
  else if (WiFi.status() != WL_CONNECTED) {
     wifiMulti.run();
  }
}