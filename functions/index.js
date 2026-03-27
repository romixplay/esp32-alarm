const functions = require("firebase-functions");
const admin = require("firebase-admin");

admin.initializeApp({
  databaseURL: "https://untitledcafe-bfd05-default-rtdb.europe-west1.firebasedatabase.app"
});

const USER_PIN = "1234";
const ADMIN_PIN = "9999";

exports.secureCommand = functions.https.onCall(async (data, context) => {
  try {
    const { pin, action, payload } = data;

    let role = null;
    if (pin === ADMIN_PIN) role = "ADMIN";
    else if (pin === USER_PIN) role = "USER";

    if (!role) {
      throw new Error("ACCESS DENIED: Invalid Passcode.");
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
        if (role !== "ADMIN") throw new Error("Admins only.");
        await db.ref("system").update({ ota_url: payload.url });
        break;
      default:
        throw new Error("Unknown command.");
    }

    return { success: true, message: `Command '${action}' executed successfully.` };

  } catch (error) {
    console.error("Backend Error:", error);
    // Passing "unknown" forces Firebase to send the actual text message back to your browser
    throw new functions.https.HttpsError("unknown", error.message || "Database update failed");
  }
});