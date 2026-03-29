const functions = require("firebase-functions");
const admin = require("firebase-admin");

admin.initializeApp({
  databaseURL: "https://untitledcafe-bfd05-default-rtdb.europe-west1.firebasedatabase.app"
});

exports.secureCommand = functions.https.onCall(async (request_or_data, context) => {
  try {
    const actualData = request_or_data.data || request_or_data;
    const { pin, action, payload } = actualData;

    // 1. Initialize DB
    const db = admin.database();

    // 2. FETCH THE DYNAMIC PASSCODE FROM FIREBASE
    const passcodeSnapshot = await db.ref("system/passcode").once("value");
    const SYSTEM_PIN = String(passcodeSnapshot.val()).trim();

    // 3. Verify the user's input against the database secret
    const receivedPin = String(pin).trim();
    
    if (receivedPin !== SYSTEM_PIN) {
      throw new Error(`ACCESS DENIED. Server received: '${receivedPin}'`);
    }
    
    // 4. Execute commands if authorized
    switch (action) {
      case "trigger_alarm":
        await db.ref("alarm_state").update({ trigger: true, duration: payload.duration, hold_trigger: false });
        break;
      case "hold_alarm":
        await db.ref("alarm_state").update({ hold_trigger: payload.active });
        break;
      case "update_volume":
        await db.ref("alarm_state").update({ volume: payload.volume });
        break;
      case "update_periodic":
        await db.ref("alarm_state").update({ periodic_active: payload.active, periodic_mins: payload.mins });
        break;
      case "sync_firmware":
        await db.ref("system").update({ ota_url: payload.url });
        break;
      default:
        throw new Error("Unknown command.");
    }

    return { success: true, message: `Command '${action}' executed successfully.` };

  } catch (error) {
    console.error("Backend Error:", error);
    throw new functions.https.HttpsError("unknown", error.message || "Database update failed");
  }
});