const functions = require("firebase-functions");
const admin = require("firebase-admin");

admin.initializeApp({
  databaseURL: "https://untitledcafe-bfd05-default-rtdb.europe-west1.firebasedatabase.app"
});

const SYSTEM_PIN = "1234";

exports.secureCommand = functions.https.onCall(async (data, context) => {
  try {
    const { pin, action, payload } = data;

    // Clean the input of any accidental spaces and force it to a string
    const receivedPin = String(pin).trim();

    if (receivedPin !== SYSTEM_PIN) {
      // THIS IS THE MAGIC LINE: It will now tell you EXACTLY what it received
      throw new Error(`ACCESS DENIED. Server received: '${receivedPin}'`);
    }

    const db = admin.database();
    
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