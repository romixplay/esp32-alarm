#!/bin/bash

# 1. Add and push the new firmware to GitHub
echo ">>> Pushing new firmware to GitHub..."
git add .
git commit -m "Auto-deploy new firmware build"
git push origin main

# Wait a couple of seconds to ensure GitHub's raw content servers register the change
sleep 3 

