const functions = require("firebase-functions");
const admin = require("firebase-admin");

// Initialize the Admin SDK with God Mode privileges
admin.initializeApp({
  databaseURL: "https://untitledcafe-bfd05-default-rtdb.europe-west1.firebasedatabase.app"
});

// Hardcoded Passcodes (Secure because frontend users cannot read backend code)
const USER_PIN = "1234";
const ADMIN_PIN = "9999";

// This creates an API endpoint that your frontend can call
exports.secureCommand = functions.https.onCall(async (data, context) => {
  const { pin, action, payload } = data;

  // 1. VERIFY THE PIN
  let role = null;
  if (pin === ADMIN_PIN) role = "ADMIN";
  else if (pin === USER_PIN) role = "USER";

  if (!role) {
    throw new functions.https.HttpsError(
      "permission-denied",
      "ACCESS DENIED: Invalid Passcode."
    );
  }

  // 2. EXECUTE THE COMMAND
  const db = admin.database();
  
  try {
    switch (action) {
      case "trigger_alarm":
        await db.ref("alarm_state").update({ 
          trigger: true, 
          duration: payload.duration,
          hold_trigger: false 
        });
        break;

      case "hold_alarm":
        await db.ref("alarm_state").update({ 
          hold_trigger: payload.active 
        });
        break;

      case "update_volume":
        await db.ref("alarm_state").update({ 
          volume: payload.volume 
        });
        break;

      case "update_periodic":
        await db.ref("alarm_state").update({ 
          periodic_active: payload.active,
          periodic_mins: payload.mins
        });
        break;

      case "sync_firmware":
        if (role !== "ADMIN") {
          throw new functions.https.HttpsError("permission-denied", "Admins only.");
        }
        await db.ref("system").update({ 
          ota_url: payload.url 
        });
        break;

      default:
        throw new functions.https.HttpsError("invalid-argument", "Unknown command.");
    }

    // Return success to the frontend
    return { success: true, message: `Command '${action}' executed successfully.` };

  } catch (error) {
    console.error("Database error:", error);
    throw new functions.https.HttpsError("internal", "Failed to update database.");
  }
});