#!/bin/bash

# 1. Grab the current date and time
TIMESTAMP=$(date +"%b %d, %Y - %H:%M:%S")
echo "Deploying Version: $TIMESTAMP"

# 2. Automatically find the version span in index.html and inject the new timestamp
sed -i '' "s|<span id=\"fw-version\">.*</span>|<span id=\"fw-version\">$TIMESTAMP</span>|g" web-app/index.html

# 3. Standard Git Push
git add .
git commit -m "Auto-Deploy: $TIMESTAMP"
git push origin main

echo "Deployment Sent to GitHub!"