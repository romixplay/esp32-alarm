#!/bin/bash

# Grab the exact YYMMDDHHMMSS format
TIMESTAMP=$(date +"%y%m%d%H%M%S")
echo "Deploying Version: FW: $TIMESTAMP"

# Inject it into the HTML
sed -i '' "s|<span id=\"fw-version\">.*</span>|<span id=\"fw-version\">FW: $TIMESTAMP</span>|g" web-app/index.html

git add .
git commit -m "Auto-Deploy: FW: $TIMESTAMP"
git push origin main

echo "Deployment Sent to GitHub!"