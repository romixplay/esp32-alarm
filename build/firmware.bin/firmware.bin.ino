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

#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- CREDENTIALS ---
WiFiMulti wifiMulti; 
#define DATABASE_SECRET "l8mdVlybE4p0BDRcj5Z0n2lVAToOr1oRQ8TTMu53"
#define DATABASE_URL "https://untitledcafe-bfd05-default-rtdb.europe-west1.firebasedatabase.app"

// --- HARDWARE PINS ---
static const int PIN_I2S_LRC  = 15;  
static const int PIN_I2S_BCLK = 18;  
static const int PIN_I2S_DIN  = 19;  
static const int PIN_AMP_SD   = 20;  

// --- FIREBASE OBJECTS (Dedicated Memory Lanes) ---
FirebaseData fbdo;          
FirebaseData fbdo_log;      
FirebaseData streamData;    
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

// --- SYSTEM STATE VARIABLES ---
bool forceDataSync = false;     
unsigned long localAlarmStartTime = 0;
unsigned long localAlarmDuration = 0;
bool isLocalAlarmActive = false;
unsigned long lastAdminPoll = 0;
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

// Siren Settings
volatile float mainVolume = 0.5;
volatile float wavVolume = 0.5; // NEW: Separate Volume for MP3s!
volatile int sirenMinFreq = 800;
volatile int sirenMaxFreq = 1600;
volatile int sirenSpeed = 10;
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
// FREERTOS AUDIO TASK (Native I2S + Synth Engine)
// =========================================================================
void audioTask(void * pvParameters) {
  const int BATCH_SIZE = 512;
  int16_t sample[BATCH_SIZE];
  uint8_t wavBuffer[1024]; 
  size_t bytes_written;
  bool wasPlaying = false; 

  uint32_t phase = 0; float currentFreq = 800;
  int direction = 1; int wobblePhase = 0; int wobbleDir = 1;
  
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
      
      // WAV MODE
      if (audioMode == 3) {
        if (wavFile && wavFile.available()) {
          size_t bytesRead = wavFile.read(wavBuffer, sizeof(wavBuffer));
          int16_t* pcmData = (int16_t*)wavBuffer;
          for(int i = 0; i < bytesRead / 2; i++) {
             // THE FIX: Uses the dedicated wavVolume slider!
             pcmData[i] = pcmData[i] * wavVolume; 
          }
          i2s_write(I2S_NUM_0, wavBuffer, bytesRead, &bytes_written, portMAX_DELAY);
        } else {
          if (wavFile) wavFile.close();
          audioMode = 0; 
        }
        vTaskDelay(1 / portTICK_PERIOD_MS); 
      }
      
      // SYNTH MODE
      else {
        int16_t amplitude = (int16_t)(15000 * ((audioMode == 1) ? mainVolume : periodicVolume));
        for(int i = 0; i < BATCH_SIZE; i++) {
          if (audioMode == 1 && i == 0) {
            currentFreq += (sirenSpeed * direction);
            if (currentFreq >= sirenMaxFreq) { currentFreq = sirenMaxFreq; direction = -1; }
            if (currentFreq <= sirenMinFreq) { currentFreq = sirenMinFreq; direction = 1; }
          }
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
    Firebase.RTDB.setString(&fbdo_log, "/system/latest_log", uniqueLog); 
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
  
  // THE BUG FIX: Force the ESP32 to follow Google's redirect maze!
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    
    // Accept either a direct OK or a successful redirect
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

  Serial.println("Mounting LittleFS Hard Drive...");
  if(!LittleFS.begin(true)){
    Serial.println("LittleFS Mount Failed!");
    return;
  }
  Serial.println("LittleFS Mounted Successfully.");
  
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW); 

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); 
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

  wifiMulti.addAP("Sadan", "shamanshaman");
  wifiMulti.addAP("Ra", "88888888");
  wifiMulti.addAP("Dekel26", "100200300");
  wifiMulti.addAP("Untitled Cafe", "onemorecup"); 

  Serial.print("Connecting to Wi-Fi");
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
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
  Serial.println("\nClock synced! We are in the present.");

  ArduinoOTA.setHostname("cafe-alarm-esp32c6");
  ArduinoOTA.begin();

  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET; 
  config.timeout.socketConnection = 10 * 1000;
  config.timeout.serverResponse = 10 * 1000;

  fbdo.setBSSLBufferSize(4096, 1024);
  fbdo.setResponseSize(2048);
  fbdo_log.setBSSLBufferSize(2048, 512);
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
  logToCloud("System Booted v4.2. Wi-Fi: " + WiFi.SSID() + "; IP: " + WiFi.localIP().toString());
}

// =========================================================================
// MAIN LOOP
// =========================================================================
void loop() {
  ArduinoOTA.handle();

  if (isLocalAlarmActive) {
    if (millis() - localAlarmStartTime >= localAlarmDuration) {
      logToCloud("Hardware Timer: Stopping audio.");
      audioMode = 0;              
      holdTriggerActive = false;  
      isLocalAlarmActive = false; 
    }
  }

  if (Firebase.ready() && signupOK) {
    
    if (!ampEnabled) {
      alarmEndTime = 0; periodicEndTime = 0; holdTriggerActive = false; isLocalAlarmActive = false; audioMode = 0;
    } else {
      bool mainAlarmActive = holdTriggerActive || isLocalAlarmActive;
      bool beepActive = false;

      // FIX: Force Unsigned Long casting to prevent timer corruption
      if (periodicActive && (millis() - lastPeriodicTrigger > (unsigned long)(periodicSecs * 1000))) {
        lastPeriodicTrigger = millis(); 
        periodicEndTime = millis() + (unsigned long)(periodicLen * 1000);
      }
      if (millis() < periodicEndTime) beepActive = true;

      if (mainAlarmActive) { audioMode = 1; } 
      else if (audioMode == 3) { /* Let WAV play */ } 
      else if (beepActive) { audioMode = 2; } 
      else { audioMode = 0; }
    }
  
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

        if (doc.containsKey("force_reboot") && doc["force_reboot"].as<bool>() == true) {
            logToCloud("Reboot command received. Restarting...");
            Firebase.RTDB.setBool(&fbdo, "/alarm_state/force_reboot", false); 
            delay(1000); 
            ESP.restart(); 
        }

        if (doc.containsKey("amp_enabled")) ampEnabled = doc["amp_enabled"].as<bool>();
        if (doc.containsKey("wobble_active")) wobbleActive = doc["wobble_active"].as<bool>();
        if (doc.containsKey("volume")) mainVolume = constrain(doc["volume"].as<int>(), 0, 100) / 100.0;
        if (doc.containsKey("wav_volume")) wavVolume = constrain(doc["wav_volume"].as<int>(), 0, 100) / 100.0; 
        if (doc.containsKey("siren_min")) sirenMinFreq = doc["siren_min"].as<int>();
        if (doc.containsKey("siren_max")) sirenMaxFreq = doc["siren_max"].as<int>();
        if (doc.containsKey("siren_speed")) sirenSpeed = doc["siren_speed"].as<int>();
        if (doc.containsKey("wobble_speed")) wobbleSpeed = doc["wobble_speed"].as<int>();
        
        if (doc.containsKey("periodic_sec")) periodicSecs = doc["periodic_sec"].as<int>();
        if (doc.containsKey("periodic_vol")) periodicVolume = constrain(doc["periodic_vol"].as<int>(), 0, 100) / 100.0;
        if (doc.containsKey("periodic_freq")) periodicFreq = doc["periodic_freq"].as<int>();
        if (doc.containsKey("periodic_len")) periodicLen = doc["periodic_len"].as<float>();
        if (doc.containsKey("hold_trigger")) holdTriggerActive = doc["hold_trigger"].as<bool>();

        // FIX: Re-added the detailed logging when turned on!
        if (doc.containsKey("periodic_active")) {
          bool newState = doc["periodic_active"].as<bool>();
          if (newState != periodicActive) {
            periodicActive = newState;
            if (periodicActive) {
              lastPeriodicTrigger = millis(); // Reset timer so it beeps immediately
              periodicEndTime = millis() + (unsigned long)(periodicLen * 1000);
              String msg = "Periodic Beep ON: " + String(periodicSecs) + "s interval, " +
                           String(periodicLen) + "s len, " + String(periodicFreq) + "Hz";
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

    if (millis() - lastHeartbeat > 15000) {
      lastHeartbeat = millis();
      Firebase.RTDB.setTimestamp(&fbdo_log, "/system/last_ping");
      Firebase.RTDB.setInt(&fbdo_log, "/system/uptime", millis() / 1000); 
    }

    if (audioMode == 0 && (millis() - lastAdminPoll > 60000)) {
      lastAdminPoll = millis();
      
      if (Firebase.RTDB.getString(&fbdo, "/system/ota_url")) {
        String ota_url = fbdo.to<String>();
        if (ota_url.length() > 10) {
          logToCloud("OTA Triggered! Freeing memory...");
          while (!Firebase.RTDB.deleteNode(&fbdo, "/system/ota_url")) { delay(500); }
          delay(1000); 
          
          audioMode = 0; digitalWrite(PIN_AMP_SD, LOW); i2s_driver_uninstall(I2S_NUM_0); 
          
          WiFiClientSecure client; client.setInsecure(); client.setTimeout(15000); 
          httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
          httpUpdate.update(client, ota_url);
          ESP.restart(); 
        }
      }
      fbdo.clear();
    }

    delay(20); 
  }
}