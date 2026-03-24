#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <ArduinoOTA.h>
#include <driver/i2s.h>

// Provide the token generation process info.
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- CREDENTIALS ---
#define WIFI_SSID "Ra"
#define WIFI_PASSWORD "88888888"
#define API_KEY "AIzaSyDAbQ4ZNMUfbloh9ePIudCoYFZeYf4ledA"
#define DATABASE_URL "https://untitledcafe-bfd05-default-rtdb.europe-west1.firebasedatabase.app" 

// --- HARDWARE PINS ---
static const int PIN_I2S_LRC  = 15;  // WS/Word Select
static const int PIN_I2S_BCLK = 18;  // Bit Clock
static const int PIN_I2S_DIN  = 19;  // Data In
static const int PIN_AMP_SD = 20;  // Shutdown/Enable

// --- FIREBASE OBJECTS ---
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

// --- CLOUD LOGGING FUNCTION ---
void logToCloud(String message) {
  Serial.println(message); // Still print locally via USB
  if (Firebase.ready() && signupOK) {
    // We overwrite the same node so it acts as a "latest status" stream
    Firebase.RTDB.setString(&fbdo, "/system/latest_log", message);
  }
}

void setup() {
  Serial.begin(115200);
  
  // 1. Setup Amp Shutdown Pin (Start OFF)
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW); 

  // 2. Connect to Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  // 3. Setup Arduino OTA (Crucial so you don't lose wireless flashing)
  ArduinoOTA.setHostname("cafe-alarm-esp32c6");
  ArduinoOTA.begin();

  // 4. Configure Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase Auth OK");
    signupOK = true;
  } else {
    Serial.printf("Firebase Error: %s\n", config.signer.signupError.message.c_str());
  }
  config.token_status_callback = tokenStatusCallback; 
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // 5. Setup I2S for the MAX98357A
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

  logToCloud("System Booted. IP: " + WiFi.localIP().toString());
}

void playTone(int durationSeconds) {
  logToCloud("Playing tone for " + String(durationSeconds) + " seconds...");
  
  // Enable the amplifier
  digitalWrite(PIN_AMP_SD, HIGH); 
  
  // Very basic square wave generation for testing the amp
  // In the future, we will replace this with MP3/WAV playback
  uint32_t sampleRate = 44100;
  uint16_t sample[64];
  size_t bytes_written;
  
  unsigned long startMillis = millis();
  while (millis() - startMillis < (durationSeconds * 1000)) {
    // Fill buffer with a harsh square wave (Warning: It will be loud)
    for (int i = 0; i < 64; i++) {
      sample[i] = (i % 20 < 10) ? 10000 : -10000; 
    }
    i2s_write(I2S_NUM_0, &sample, sizeof(sample), &bytes_written, portMAX_DELAY);
    
    // Crucial: yield to the watchdog timer and handle OTA mid-beep
    yield(); 
    ArduinoOTA.handle(); 
  }

  // Disable amp and clear buffer
  i2s_zero_dma_buffer(I2S_NUM_0);
  digitalWrite(PIN_AMP_SD, LOW);
  logToCloud("Playback finished. Amp disabled.");
}

void loop() {
  // Always handle OTA requests first
  ArduinoOTA.handle();

  if (Firebase.ready() && signupOK) {
    
    // Check if the frontend triggered the alarm
    if (Firebase.RTDB.getBool(&fbdo, "/alarm_state/trigger")) {
      bool shouldTrigger = fbdo.to<bool>();
      
      if (shouldTrigger) {
        logToCloud("Trigger command received from Web UI.");
        
        int duration = 3; // default
        if (Firebase.RTDB.getInt(&fbdo, "/alarm_state/duration")) {
          duration = fbdo.to<int>();
        }

        playTone(duration);
        
        // Reset trigger in DB
        Firebase.RTDB.setBool(&fbdo, "/alarm_state/trigger", false);
        logToCloud("System Idle. Waiting for commands.");
      }
    }
    
    // Polling delay (keeps Firebase happy, allows OTA to breathe)
    delay(500); 
  }
}