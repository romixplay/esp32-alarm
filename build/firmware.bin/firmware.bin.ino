#include <Arduino.h>
#include <Update.h>

// --- SAFE BOOT VAULT ---
RTC_NOINIT_ATTR int crashCounter;
RTC_NOINIT_ATTR uint32_t rtcMagic;

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 1. Maintain the vault
  if (rtcMagic != 0x5A5A5A5A) { rtcMagic = 0x5A5A5A5A; crashCounter = 0; }
  crashCounter++;
  
  Serial.println("\n☠️ POISON PILL FIRMWARE LOADED ☠️");
  Serial.println("--- BOOT ATTEMPT: " + String(crashCounter) + " ---");

  // 2. The Rollback Trigger
  if (crashCounter >= 3) {
    Serial.println("CRITICAL: Death Loop limit reached!");
    if (Update.canRollBack()) {
        Serial.println("Rolling back to V1 Stable...");
        Update.rollBack();
        crashCounter = 0; // Reset before jumping back in time
        ESP.restart();    
    } else {
        Serial.println("Rollback failed! No safe partition found.");
        while(true) { delay(100); }
    }
  } 
  // 3. The Deliberate Crash
  else {
    Serial.println("Simulating fatal code error in 2 seconds...");
    delay(2000);
    
    // This physically crashes the CPU (Core Panic)
    abort(); 
  }
}

void loop() {}