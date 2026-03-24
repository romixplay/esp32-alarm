#!/bin/bash

# 1. Add and push the new firmware to GitHub
echo ">>> Pushing new firmware to GitHub..."
git add build/firmware.bin
git commit -m "Auto-deploy new firmware build"
git push origin main

# Wait a couple of seconds to ensure GitHub's raw content servers register the change
sleep 3 

# 2. Send the trigger to Firebase via REST API
# Note: The URL uses raw.githubusercontent.com to bypass the GitHub webpage and get the raw data
echo ">>> Signaling ESP32 via Firebase..."

curl -X PATCH -d '{
  "ota_url": "https://raw.githubusercontent.com/romixplay/esp32-alarm/main/build/firmware.bin"
}' "https://untitledcafe-bfd05-default-rtdb.europe-west1.firebasedatabase.app/system.json"

echo ""
echo ">>> Deployment trigger sent! ESP32 should be flashing now."