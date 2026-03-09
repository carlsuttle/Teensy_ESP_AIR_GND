function fmtSigned(x, d = 3) {
  const n = Number(x);
  if (!Number.isFinite(n)) return "--";
  return `${n < 0 ? "-" : "+"}${Math.abs(n).toFixed(d)}`;
}

export function initAttUi(Telemetry, summaryRef) {
  const roll = document.getElementById("attRoll");
  const pitch = document.getElementById("attPitch");
  const yaw = document.getElementById("attYaw");
  const accelX = document.getElementById("attAccelX");
  const accelY = document.getElementById("attAccelY");
  const accelZ = document.getElementById("attAccelZ");
  const gyroX = document.getElementById("attGyroX");
  const gyroY = document.getElementById("attGyroY");
  const gyroZ = document.getElementById("attGyroZ");
  const magX = document.getElementById("attMagX");
  const magY = document.getElementById("attMagY");
  const magZ = document.getElementById("attMagZ");
  const fusionState = document.getElementById("fusionState");
  const fusionRates = document.getElementById("fusionRates");
  const ack = document.getElementById("fusionAck");
  const flagAccelRejected = document.getElementById("flagAccelRejected");
  const flagMagRejected = document.getElementById("flagMagRejected");
  const flagRecovering = document.getElementById("flagRecovering");

  const setGain = document.getElementById("setGain");
  const setAccelRej = document.getElementById("setAccelRej");
  const setMagRej = document.getElementById("setMagRej");
  const setRecTrig = document.getElementById("setRecTrig");
  const settingInputs = [setGain, setAccelRej, setMagRej, setRecTrig];
  let settingsDirty = false;
  let pendingAction = "";
  let pendingAckId = -1;

  function tsNow() {
    const d = new Date();
    const hh = String(d.getHours()).padStart(2, "0");
    const mm = String(d.getMinutes()).padStart(2, "0");
    const ss = String(d.getSeconds()).padStart(2, "0");
    return `${hh}:${mm}:${ss}`;
  }

  function anySettingFocused() {
    const active = document.activeElement;
    return settingInputs.includes(active);
  }

  settingInputs.forEach((el) => {
    el.addEventListener("input", () => {
      settingsDirty = true;
    });
  });

  function setBoolPill(el, isTrue) {
    if (!el) return;
    el.classList.toggle("bool-true", !!isTrue);
    el.classList.toggle("bool-false", !isTrue);
  }

  document.getElementById("btnFusionRefresh").addEventListener("click", () => {
    settingsDirty = false;
    const id = Telemetry.nextCmdId();
    pendingAction = "Refresh";
    pendingAckId = id;
    ack.textContent = `[${tsNow()}] Refreshing settings...`;
    const ok = Telemetry.sendCmd({ type: "cmd", id, cmd: "getFusionSettings" });
    if (!ok) ack.textContent = `[${tsNow()}] Refresh send failed`;
  });

  document.getElementById("btnFusionApply").addEventListener("click", () => {
    settingsDirty = false;
    const id = Telemetry.nextCmdId();
    pendingAction = "Apply";
    pendingAckId = id;
    ack.textContent = `[${tsNow()}] Applying settings...`;
    const settings = {
      gain: Number(setGain.value),
      accelerationRejection: Number(setAccelRej.value),
      magneticRejection: Number(setMagRej.value),
      recoveryTriggerPeriod: Number(setRecTrig.value)
    };
    const ok = Telemetry.sendCmd({ type: "cmd", id, cmd: "setFusionSettings", settings });
    if (!ok) ack.textContent = `[${tsNow()}] Apply send failed`;
  });

  document.getElementById("btnFusionReset").addEventListener("click", () => {
    const id = Telemetry.nextCmdId();
    pendingAction = "Reset";
    pendingAckId = id;
    ack.textContent = `[${tsNow()}] Resetting fusion...`;
    const ok = Telemetry.sendCmd({ type: "cmd", id, cmd: "resetFusion" });
    if (!ok) ack.textContent = `[${tsNow()}] Reset send failed`;
  });

  Telemetry.on("att", (m) => {
    roll.textContent = Number(m.roll_deg ?? 0).toFixed(2);
    pitch.textContent = Number(m.pitch_deg ?? 0).toFixed(2);
    yaw.textContent = Number(m.yaw_deg ?? 0).toFixed(2);
    const a = Array.isArray(m.accel_mps2) ? m.accel_mps2 : [NaN, NaN, NaN];
    const g = Array.isArray(m.gyro_dps) ? m.gyro_dps : [NaN, NaN, NaN];
    const mg = Array.isArray(m.mag_uT) ? m.mag_uT : [NaN, NaN, NaN];
    accelX.textContent = fmtSigned(a[0], 3);
    accelY.textContent = fmtSigned(a[1], 3);
    accelZ.textContent = fmtSigned(a[2], 3);
    gyroX.textContent = fmtSigned(g[0], 3);
    gyroY.textContent = fmtSigned(g[1], 3);
    gyroZ.textContent = fmtSigned(g[2], 3);
    magX.textContent = fmtSigned(mg[0], 3);
    magY.textContent = fmtSigned(mg[1], 3);
    magZ.textContent = fmtSigned(mg[2], 3);
    summaryRef.att = `r ${roll.textContent} p ${pitch.textContent} y ${yaw.textContent}`;
  });

  Telemetry.on("fusion_state", (m) => {
    if (!m.available) {
      fusionState.textContent = "available=false";
      setBoolPill(flagAccelRejected, false);
      setBoolPill(flagMagRejected, false);
      setBoolPill(flagRecovering, false);
      return;
    }
    const flags = m.flags || {};
    setBoolPill(flagAccelRejected, !!flags.accelRejected);
    setBoolPill(flagMagRejected, !!flags.magRejected);
    setBoolPill(flagRecovering, !!flags.recovering);
    const q = Array.isArray(m.quat) ? m.quat.map((x) => Number(x).toFixed(4)).join(", ") : "--";
    fusionState.textContent = `quat=[${q}] accelError=${m.accelError ?? "--"} magError=${m.magError ?? "--"}`;
  });

  Telemetry.on("fusion_settings", (m) => {
    const s = m.settings || {};
    const allowOverwrite = !settingsDirty && !anySettingFocused();
    if (allowOverwrite) {
      if (s.gain != null) setGain.value = s.gain;
      if (s.accelerationRejection != null) setAccelRej.value = s.accelerationRejection;
      if (s.magneticRejection != null) setMagRej.value = s.magneticRejection;
      if (s.recoveryTriggerPeriod != null) setRecTrig.value = s.recoveryTriggerPeriod;
    }
    fusionRates.textContent = `IMU ${m.imu_hz ?? "--"} Hz / ATT ${m.att_pub_hz ?? "--"} Hz`;
    if (pendingAction === "Refresh") {
      ack.textContent = `[${tsNow()}] Refresh complete`;
      pendingAction = "";
      pendingAckId = -1;
    } else if (pendingAction === "Apply") {
      ack.textContent = `[${tsNow()}] Apply complete`;
      pendingAction = "";
      pendingAckId = -1;
    }
  });

  Telemetry.on("ack", (m) => {
    if (m.id === pendingAckId) {
      if (m.ok) {
        if (pendingAction === "Reset") {
          ack.textContent = `[${tsNow()}] Reset accepted`;
          pendingAction = "";
          pendingAckId = -1;
        }
      } else {
        ack.textContent = `[${tsNow()}] ${pendingAction || "Command"} failed: ${m.error || "failed"}`;
        pendingAction = "";
        pendingAckId = -1;
      }
      return;
    }
    ack.textContent = m.ok ? `[${tsNow()}] ACK ${m.id}: ok` : `[${tsNow()}] ACK ${m.id}: ${m.error || "failed"}`;
  });
}
