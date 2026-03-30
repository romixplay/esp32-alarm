const functions = require("firebase-functions");
const admin = require("firebase-admin");

admin.initializeApp();

exports.secureCommand = functions.https.onCall(async (request_or_data, context) => {
  try {
    const actualData = request_or_data.data || request_or_data;
    const { pin, action, payload } = actualData;

    const db = admin.database();
    const passcodeSnapshot = await db.ref("system/passcode").once("value");
    const SYSTEM_PIN = String(passcodeSnapshot.val()).trim();

    if (String(pin).trim() !== SYSTEM_PIN) {
      throw new Error(`ACCESS DENIED.`);
    }
    
    switch (action) {
      case "trigger_alarm":
        await db.ref("alarm_state").update({ trigger_time: Date.now(), duration: payload.duration, hold_trigger: false });
        break;
      case "hold_alarm":
        await db.ref("alarm_state").update({ hold_trigger: payload.active });
        break;
      case "update_settings":
        await db.ref("alarm_state").update({ 
            volume: payload.volume,
            siren_min: payload.siren_min,
            siren_max: payload.siren_max,
            siren_speed: payload.siren_speed,
            periodic_active: payload.periodic_active,
            periodic_sec: payload.periodic_sec,
            periodic_vol: payload.periodic_vol,
            periodic_freq: payload.periodic_freq,
            periodic_len: payload.periodic_len
        });
        break;
      case "sync_firmware":
        await db.ref("system").update({ ota_url: payload.url });
        break;
      default:
        throw new Error("Unknown command.");
    }

    return { success: true };
  } catch (error) {
    throw new functions.https.HttpsError("unknown", error.message);
  }
});