const statusEl = document.getElementById("status");
const statsEl = document.getElementById("stats");
const recEl = document.getElementById("rec");
const gpsEl = document.getElementById("gps");
const attEl = document.getElementById("att");
const baroEl = document.getElementById("baro");
const linkEl = document.getElementById("link");
const logsEl = document.getElementById("logs");
const pfdCanvas = document.getElementById("pfdCanvas");
const airLoggerTextEl = document.getElementById("airLoggerText");
const fusionAngularLightEl = document.getElementById("fusionAngularLight");
const fusionAccelLightEl = document.getElementById("fusionAccelLight");
const fusionMagLightEl = document.getElementById("fusionMagLight");
const eventLogEl = document.getElementById("eventLog");
const filesJsonEl = document.getElementById("filesJson");
const refreshLogStatusBtn = document.getElementById("refreshLogStatus");
const startLogBtn = document.getElementById("startLog");
const stopLogBtn = document.getElementById("stopLog");
const fusionApplyBtn = document.getElementById("fusionApplyBtn");
const gainSlider = document.getElementById("gainSlider");
const accelRejSlider = document.getElementById("accelRejSlider");
const magRejSlider = document.getElementById("magRejSlider");
const recoverySlider = document.getElementById("recoverySlider");
const gainValueEl = document.getElementById("gainValue");
const accelRejValueEl = document.getElementById("accelRejValue");
const magRejValueEl = document.getElementById("magRejValue");
const recoveryValueEl = document.getElementById("recoveryValue");

let ws = null;
let reconnectTimer = null;
let generation = 0;
let lastMessageMs = 0;
let latestSnapshot = null;
let latestFiles = null;
let latestStorage = null;
let pendingControl = null;
let reqId = 1;
let pfdLastW = 0;
let pfdLastH = 0;
let dbgMsgCount = 0;
let dbgApplyCount = 0;
let dbgRenderCount = 0;
let dbgMsgHz = 0;
let dbgApplyHz = 0;
let dbgRenderHz = 0;
let dbgLastMsgMs = 0;
let dbgLastApplyMs = 0;
let dbgLastRenderMs = 0;
let dbgLastMsgSeq = null;
let dbgLastMsgAgeMs = null;
let dbgLastDisplaySeq = null;
let dbgLastDisplayRoll = null;
let dbgLastDisplayPitch = null;
let dbgLastDisplayAlt = null;

const eventLines = [];
const previewSnapshot = {
  fresh: true,
  recording_active: false,
  recording_busy: false,
  replay_active: false,
  replay_paused: false,
  replay_file_open: false,
  roll_deg: 0,
  pitch_deg: 0,
  yaw_deg: 181,
  mag_heading_deg: 181,
  baro_alt_m: 8,
  baro_vsi_mps: 0,
  gSpeed_mms: 0,
  gps_fix_type: 3,
  gps_num_sv: 11,
  headMot_1e5deg: 24300000,
  lat_1e7: 372282000,
  lon_1e7: -1218882700,
  fusion_gain: 0.06,
  fusion_accel_rej: 20,
  fusion_mag_rej: 60,
  fusion_recovery_period: 1200,
  flags: 0
};

function appendEvent(text) {
  const stamp = new Date().toLocaleTimeString();
  eventLines.unshift(`[${stamp}] ${text}`);
  while (eventLines.length > 40) eventLines.pop();
  eventLogEl.textContent = eventLines.join("\n");
}

function nextReqId() {
  return reqId++;
}

function dash(v) {
  return (v === null || v === undefined || Number.isNaN(v)) ? "-" : String(v);
}

function fmt(v, n = 2) {
  if (v === null || v === undefined || Number.isNaN(Number(v))) return "-";
  return Number(v).toFixed(n);
}

function fmtMs(v) {
  if (v === null || v === undefined || Number.isNaN(Number(v))) return "-";
  return `${Math.round(Number(v))} ms`;
}

function fmtBytes(v) {
  if (v === null || v === undefined || Number.isNaN(Number(v))) return "-";
  const n = Number(v);
  if (n >= 1024 * 1024) return `${fmt(n / (1024 * 1024), 2)} MiB`;
  if (n >= 1024) return `${fmt(n / 1024, 1)} KiB`;
  return `${Math.round(n)} B`;
}

function normalizeSnapshot(snapshot) {
  if (!snapshot) return snapshot;
  const normalized = { ...snapshot };
  const airValid = !!normalized.air_valid;
  const airOnline = !!normalized.air_online;
  if (airValid) {
    normalized.roll_deg = normalized.air_roll_deg ?? normalized.roll_deg;
    normalized.pitch_deg = normalized.air_pitch_deg ?? normalized.pitch_deg;
    normalized.yaw_deg = normalized.air_yaw_deg ?? normalized.yaw_deg;
    normalized.mag_heading_deg = normalized.air_yaw_deg ?? normalized.mag_heading_deg;
    normalized.baro_alt_m = normalized.air_altitude_m ?? normalized.baro_alt_m;
    normalized.baro_vsi_mps = normalized.air_climb_mps ?? normalized.baro_vsi_mps;
  }
  if (airValid && airOnline) {
    normalized.fresh = true;
    normalized.age_ms = normalized.air_age_ms ?? normalized.age_ms;
  }
  return normalized;
}

setInterval(() => {
  dbgMsgHz = dbgMsgCount;
  dbgApplyHz = dbgApplyCount;
  dbgRenderHz = dbgRenderCount;
  console.log(`WSRX hz=${dbgMsgHz} seq=${dash(dbgLastMsgSeq)} age_ms=${dash(dbgLastMsgAgeMs)}`);
  console.log(
    `UIAPPLY hz=${dbgApplyHz} render_hz=${dbgRenderHz} seq=${dash(dbgLastDisplaySeq)} ` +
    `roll=${fmt(dbgLastDisplayRoll, 2)} pitch=${fmt(dbgLastDisplayPitch, 2)} alt=${fmt(dbgLastDisplayAlt, 2)}`
  );
  dbgMsgCount = 0;
  dbgApplyCount = 0;
  dbgRenderCount = 0;
}, 1000);

function sourceMode(snapshot) {
  if (!snapshot) return "idle";
  if (snapshot.replay_active || snapshot.replay_paused || snapshot.replay_file_open) return "replay";
  if (snapshot.fresh) return "live";
  return "idle";
}

function gpsDateTimeText(snapshot) {
  if (!snapshot || !snapshot.gps_calendar_valid) return "-";
  const mm = String(snapshot.gps_month).padStart(2, "0");
  const dd = String(snapshot.gps_day).padStart(2, "0");
  const hh = String(snapshot.gps_hour).padStart(2, "0");
  const mi = String(snapshot.gps_min).padStart(2, "0");
  const ss = String(snapshot.gps_sec).padStart(2, "0");
  return `${snapshot.gps_year}-${mm}-${dd} ${hh}:${mi}:${ss} UTC`;
}

function liveUpdateHz(deltaMs) {
  if (!deltaMs || deltaMs <= 0) return null;
  return 1000 / deltaMs;
}

function linkState(snapshot) {
  if (!snapshot) return "waiting";
  if (snapshot.fresh) return "valid";
  const ageMs = Number(snapshot.age_ms ?? 0xFFFFFFFF);
  if (Number.isFinite(ageMs) && ageMs < 0xFFFFFFFF) return "stale";
  return "invalid";
}

function setFusionLight(el, on) {
  if (!el) return;
  el.classList.toggle("on", !!on);
  el.classList.toggle("off", !on);
}

function updateFusionUi(snapshot) {
  if (!snapshot) return;
  gainSlider.value = Number(snapshot.fusion_gain ?? 0.06).toFixed(2);
  accelRejSlider.value = String(Math.round(Number(snapshot.fusion_accel_rej ?? 20)));
  magRejSlider.value = String(Math.round(Number(snapshot.fusion_mag_rej ?? 60)));
  recoverySlider.value = (Number(snapshot.fusion_recovery_period ?? 1200) / 400).toFixed(1);
  gainValueEl.textContent = fmt(gainSlider.value, 2);
  accelRejValueEl.textContent = `${fmt(accelRejSlider.value, 0)} deg`;
  magRejValueEl.textContent = `${fmt(magRejSlider.value, 0)} deg`;
  recoveryValueEl.textContent = `${fmt(recoverySlider.value, 1)} s / ${Math.round(Number(snapshot.fusion_recovery_period ?? 1200))} smp`;
  const flags = Number(snapshot.flags || 0);
  setFusionLight(fusionAngularLightEl, (flags & (1 << 2)) !== 0);
  setFusionLight(fusionAccelLightEl, (flags & (1 << 3)) !== 0);
  setFusionLight(fusionMagLightEl, (flags & (1 << 4)) !== 0);
}

function setButtonsEnabled(enabled) {
  startLogBtn.disabled = !enabled;
  stopLogBtn.disabled = !enabled;
  refreshLogStatusBtn.disabled = !enabled;
  fusionApplyBtn.disabled = !enabled;
}

function configurePfdCanvas() {
  if (!pfdCanvas) return null;
  const dpr = Math.max(1, Math.min(window.devicePixelRatio || 1, 2));
  const wrap = pfdCanvas.parentElement;
  const wrapW = Math.round((wrap && wrap.clientWidth) ? wrap.clientWidth : (pfdCanvas.clientWidth || 390));
  const canvasTop = pfdCanvas.getBoundingClientRect().top;
  const availableH = Math.max(420, Math.floor(window.innerHeight - canvasTop - 12));
  const maxWFromHeight = Math.floor(availableH * (390 / 680));
  const cssW = Math.max(320, Math.min(wrapW, maxWFromHeight));
  const cssH = Math.round(cssW * (680 / 390));
  pfdCanvas.style.width = `${cssW}px`;
  pfdCanvas.style.height = `${cssH}px`;
  const pxW = Math.round(cssW * dpr);
  const pxH = Math.round(cssH * dpr);
  if (pfdCanvas.width !== pxW || pfdCanvas.height !== pxH) {
    pfdCanvas.width = pxW;
    pfdCanvas.height = pxH;
  }
  pfdLastW = cssW;
  pfdLastH = cssH;
  const ctx = pfdCanvas.getContext("2d");
  if (!ctx) return null;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w: cssW, h: cssH };
}

function drawRoundedRect(ctx, x, y, w, h, r) {
  const rr = Math.min(r, w / 2, h / 2);
  ctx.beginPath();
  ctx.moveTo(x + rr, y);
  ctx.arcTo(x + w, y, x + w, y + h, rr);
  ctx.arcTo(x + w, y + h, x, y + h, rr);
  ctx.arcTo(x, y + h, x, y, rr);
  ctx.arcTo(x, y, x + w, y, rr);
  ctx.closePath();
}

function pfdText(ctx, text, x, y, size, color, align = "center") {
  ctx.font = `600 ${size}px Arial`;
  ctx.textAlign = align;
  ctx.textBaseline = "middle";
  ctx.fillStyle = color;
  ctx.fillText(text, x, y);
}

function pfdPad(value, width) {
  const n = Math.max(0, Math.abs(Math.round(Number(value) || 0)));
  return String(n).padStart(width, "0");
}

function clamp(v, lo, hi) {
  return Math.min(Math.max(Number(v), lo), hi);
}

function renderPfdStale() {
  const view = configurePfdCanvas();
  if (!view) return;
  const { ctx, w, h } = view;
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = "#020817";
  ctx.fillRect(0, 0, w, h);
  drawRoundedRect(ctx, 10, 10, w - 20, h - 20, 18);
  ctx.strokeStyle = "#334155";
  ctx.lineWidth = 2;
  ctx.stroke();
  pfdText(ctx, "PFD", w / 2, h * 0.12, 30, "#e2e8f0");
  pfdText(ctx, "NO TELEMETRY", w / 2, h * 0.48, 26, "#f59e0b");
  pfdText(ctx, "Waiting for AIR", w / 2, h * 0.54, 18, "#94a3b8");
}

function pfdRenderSnapshot() {
  return latestSnapshot || previewSnapshot;
}

function renderPfdPanel(snapshot) {
  const view = configurePfdCanvas();
  if (!view || !snapshot) return;
  const { ctx, w, h } = view;
  const rollDeg = Number(snapshot.roll_deg ?? 0);
  const pitchDeg = Number(snapshot.pitch_deg ?? 0);
  const yawDeg = Number(snapshot.mag_heading_deg ?? snapshot.yaw_deg ?? 0);
  const headingDeg = ((yawDeg % 360) + 360) % 360;
  const altM = Number(snapshot.baro_alt_m ?? (Number(snapshot.hMSL_mm ?? 0) / 1000));
  const vsiMps = Number(snapshot.baro_vsi_mps ?? 0);
  const gsMps = Number(snapshot.gSpeed_mms ?? 0) / 1000;
  const fixType = Number(snapshot.gps_fix_type ?? 0);
  const sats = Number(snapshot.gps_num_sv ?? 0);
  const courseDeg = ((Number(snapshot.headMot_1e5deg ?? 0) / 100000) % 360 + 360) % 360;
  const horizonR = Math.max(120, Math.min(w * 0.34, h * 0.28));
  const horizonCx = w * 0.5;
  const horizonCy = horizonR + 46;
  const pitchPxPerDeg = horizonR / 40;
  const speedKmh = gsMps * 3.6;

  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = "#020817";
  ctx.fillRect(0, 0, w, h);
  ctx.save();
  ctx.beginPath();
  ctx.arc(horizonCx, horizonCy, horizonR, 0, Math.PI * 2);
  ctx.clip();
  ctx.translate(horizonCx, horizonCy + pitchDeg * pitchPxPerDeg);
  ctx.rotate((rollDeg * Math.PI) / 180);
  ctx.fillStyle = "#4da7e8";
  ctx.fillRect(-w, -h * 1.2, w * 2, h * 1.2);
  ctx.fillStyle = "#7b5a42";
  ctx.fillRect(-w, 0, w * 2, h * 1.2);
  ctx.strokeStyle = "#f8fafc";
  ctx.lineWidth = 3;
  ctx.beginPath();
  ctx.moveTo(-w, 0);
  ctx.lineTo(w, 0);
  ctx.stroke();
  for (let deg = -80; deg <= 80; deg += 5) {
    if (deg === 0) continue;
    const y = -deg * pitchPxPerDeg;
    const longMark = deg % 10 === 0;
    const markW = longMark ? 52 : 26;
    ctx.strokeStyle = "#f8fafc";
    ctx.lineWidth = longMark ? 3 : 2;
    ctx.beginPath();
    ctx.moveTo(-markW, y);
    ctx.lineTo(markW, y);
    ctx.stroke();
    if (longMark) {
      pfdText(ctx, String(Math.abs(deg)), -markW - 18, y, 14, "#f8fafc", "right");
      pfdText(ctx, String(Math.abs(deg)), markW + 18, y, 14, "#f8fafc", "left");
    }
  }
  ctx.restore();

  ctx.strokeStyle = "#cbd5e1";
  ctx.lineWidth = 3;
  ctx.beginPath();
  ctx.arc(horizonCx, horizonCy, horizonR, Math.PI * 1.08, Math.PI * 1.92);
  ctx.stroke();
  for (let bank = -60; bank <= 60; bank += 10) {
    const a = ((bank - 90) * Math.PI) / 180;
    const r1 = horizonR + 6;
    const r2 = horizonR + (bank % 30 === 0 ? 24 : 16);
    ctx.beginPath();
    ctx.moveTo(horizonCx + Math.cos(a) * r1, horizonCy + Math.sin(a) * r1);
    ctx.lineTo(horizonCx + Math.cos(a) * r2, horizonCy + Math.sin(a) * r2);
    ctx.stroke();
  }
  const bankPointerDeg = clamp(rollDeg, -60, 60);
  const bankPointerAngle = ((bankPointerDeg - 90) * Math.PI) / 180;
  const bankPointerCx = horizonCx + Math.cos(bankPointerAngle) * horizonR;
  const bankPointerCy = horizonCy + Math.sin(bankPointerAngle) * horizonR;
  ctx.fillStyle = "#facc15";
  ctx.save();
  ctx.translate(bankPointerCx, bankPointerCy);
  ctx.rotate(bankPointerAngle + (Math.PI * 0.5));
  ctx.beginPath();
  ctx.moveTo(0, -10);
  ctx.lineTo(-12, 14);
  ctx.lineTo(12, 14);
  ctx.closePath();
  ctx.fill();
  ctx.restore();

  ctx.strokeStyle = "#f59e0b";
  ctx.lineWidth = 4;
  ctx.beginPath();
  ctx.moveTo(horizonCx - 58, horizonCy + 4);
  ctx.lineTo(horizonCx - 20, horizonCy + 4);
  ctx.lineTo(horizonCx - 8, horizonCy + 14);
  ctx.lineTo(horizonCx + 8, horizonCy + 14);
  ctx.lineTo(horizonCx + 20, horizonCy + 4);
  ctx.lineTo(horizonCx + 58, horizonCy + 4);
  ctx.stroke();

  const tapeTop = horizonCy - horizonR;
  const tapeBottom = horizonCy + horizonR;
  const tapeH = tapeBottom - tapeTop;
  const tapeW = 32;
  const speedX = 44;
  const altX = w - tapeW - 44;
  const valueBoxInset = 24;
  const speedOuterLeft = speedX;
  const speedOuterRight = speedX + tapeW + valueBoxInset;
  const altOuterLeft = altX - valueBoxInset;
  const altOuterRight = altX + tapeW;
  const valueBoxH = 42;
  const valueBoxY = horizonCy - valueBoxH / 2;
  const valueTextY = valueBoxY + 21;
  const tapeCenterY = horizonCy;
  ctx.fillStyle = "#0b1220";
  drawRoundedRect(ctx, speedX, tapeTop, tapeW, tapeH, 10);
  ctx.fill();
  ctx.strokeStyle = "#334155";
  ctx.lineWidth = 2;
  ctx.stroke();
  drawRoundedRect(ctx, altX, tapeTop, tapeW, tapeH, 10);
  ctx.fill();
  ctx.stroke();

  const tapeScale = 2.6;
  const drawTape = (x, currentValue, colorAccent, outerLeft, outerRight) => {
    ctx.save();
    ctx.beginPath();
    ctx.rect(x, tapeTop, tapeW, tapeH);
    ctx.clip();
    for (let i = -6; i <= 6; i++) {
      const markValue = Math.round(currentValue / 10) * 10 + (i * 10);
      const y = tapeCenterY + ((currentValue - markValue) * tapeScale);
      pfdText(ctx, String(markValue), x + tapeW - 6, y, 14, "#cbd5e1", "right");
    }
    ctx.restore();
    ctx.fillStyle = "#020617";
    drawRoundedRect(ctx, outerLeft, valueBoxY, outerRight - outerLeft, valueBoxH, 10);
    ctx.fill();
    ctx.strokeStyle = colorAccent;
    ctx.lineWidth = 2;
    ctx.stroke();
    const valueTxt = pfdPad(currentValue, colorAccent === "#f59e0b" ? 3 : 2);
    const valueColor = currentValue < 0 ? "#ef4444" : "#f8fafc";
    pfdText(ctx, valueTxt, outerRight - 10, valueTextY, 24, valueColor, "right");
  };
  drawTape(speedX, speedKmh, "#38bdf8", speedOuterLeft, speedOuterRight);
  drawTape(altX, altM, "#f59e0b", altOuterLeft, altOuterRight);

  const vsiX = w - 18;
  const vsiTop = horizonCy - 112;
  const vsiBottom = horizonCy + 112;
  ctx.strokeStyle = "#64748b";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(vsiX, vsiTop);
  ctx.lineTo(vsiX, vsiBottom);
  ctx.stroke();
  pfdText(ctx, "1000", vsiX, vsiTop - 12, 12, "#f8fafc");
  pfdText(ctx, "-1000", vsiX, vsiBottom + 12, 12, "#f8fafc");
  for (const tick of [-4, -2, 0, 2, 4]) {
    const y = horizonCy - (tick * 24);
    ctx.beginPath();
    ctx.moveTo(vsiX - (tick === 0 ? 20 : 12), y);
    ctx.lineTo(vsiX, y);
    ctx.stroke();
  }
  const vsiY = horizonCy - clamp(vsiMps, -5, 5) * 24;
  const vsiBoxX = altX + tapeW;
  const vsiBoxY = vsiY - 12;
  ctx.fillStyle = "#020617";
  drawRoundedRect(ctx, vsiBoxX, vsiBoxY, 42, 24, 6);
  ctx.fill();
  ctx.strokeStyle = "#f8fafc";
  ctx.lineWidth = 1;
  ctx.stroke();
  pfdText(ctx, String(Math.abs(Math.round(vsiMps))), vsiBoxX + 36, vsiBoxY + 12, 14, "#f8fafc", "right");

  ctx.save();
  ctx.beginPath();
  ctx.rect(12, 0, w - 24, 22);
  ctx.clip();
  for (let i = -6; i <= 6; i++) {
    const mark = (Math.round(headingDeg / 10) * 10) + (i * 10);
    const wrapped = ((mark % 360) + 360) % 360;
    let delta = wrapped - headingDeg;
    if (delta > 180) delta -= 360;
    if (delta < -180) delta += 360;
    const x = horizonCx + (delta * 5.5);
    ctx.strokeStyle = "#94a3b8";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, 6);
    ctx.stroke();
    pfdText(ctx, String(wrapped).padStart(3, "0"), x, 14, 10, "#cbd5e1");
  }
  ctx.restore();
  ctx.fillStyle = "#020617";
  drawRoundedRect(ctx, 12, 8, 88, 24, 8);
  ctx.fill();
  ctx.strokeStyle = "#475569";
  ctx.lineWidth = 1.5;
  ctx.stroke();
  pfdText(ctx, `FIX ${fixType}/${sats}`, 20, 20, 13, "#cbd5e1", "left");
  drawRoundedRect(ctx, horizonCx - 34, 8, 68, 28, 10);
  ctx.fillStyle = "#020617";
  ctx.fill();
  ctx.strokeStyle = "#ffffff";
  ctx.lineWidth = 1.5;
  ctx.stroke();
  pfdText(ctx, pfdPad(headingDeg, 3), horizonCx, 22, 22, "#f8fafc");

  const hsiAreaX = 12;
  const hsiAreaY = tapeBottom + 18;
  const hsiAreaW = w - 24;
  const hsiAreaH = h - hsiAreaY - 12;
  const hsiCx = hsiAreaX + (hsiAreaW * 0.5);
  const hsiCy = hsiAreaY + (hsiAreaH * 0.5);
  const hsiR = Math.max(75, Math.min(hsiAreaW * 0.34, hsiAreaH * 0.42));
  ctx.strokeStyle = "#334155";
  ctx.lineWidth = 2;
  drawRoundedRect(ctx, hsiAreaX, hsiAreaY, hsiAreaW, hsiAreaH, 16);
  ctx.stroke();
  ctx.strokeStyle = "#475569";
  ctx.lineWidth = 4;
  ctx.beginPath();
  ctx.arc(hsiCx, hsiCy, hsiR, 0, Math.PI * 2);
  ctx.stroke();
  ctx.strokeStyle = "#f8fafc";
  ctx.lineWidth = 2;
  for (let deg = 0; deg < 360; deg += 45) {
    const a = ((deg - 90) * Math.PI) / 180;
    const tickInner = hsiR + 1;
    const tickOuter = hsiR + (deg % 90 === 0 ? 13 : 9);
    ctx.beginPath();
    ctx.moveTo(hsiCx + Math.cos(a) * tickInner, hsiCy + Math.sin(a) * tickInner);
    ctx.lineTo(hsiCx + Math.cos(a) * tickOuter, hsiCy + Math.sin(a) * tickOuter);
    ctx.stroke();
  }

  ctx.save();
  ctx.translate(hsiCx, hsiCy);
  ctx.rotate((-headingDeg * Math.PI) / 180);
  for (let deg = 0; deg < 360; deg += 10) {
    const a = ((deg - 90) * Math.PI) / 180;
    const r1 = hsiR - (deg % 30 === 0 ? 12 : 7);
    const r2 = hsiR;
    ctx.strokeStyle = "#94a3b8";
    ctx.lineWidth = deg % 30 === 0 ? 2 : 1;
    ctx.beginPath();
    ctx.moveTo(Math.cos(a) * r1, Math.sin(a) * r1);
    ctx.lineTo(Math.cos(a) * r2, Math.sin(a) * r2);
    ctx.stroke();
  }
  ctx.lineWidth = 6;
  ctx.strokeStyle = "#d946ef";
  const rel = ((courseDeg - headingDeg) * Math.PI) / 180;
  ctx.beginPath();
  ctx.moveTo(Math.sin(rel) * (hsiR - 58), Math.cos(rel) * (hsiR - 58));
  ctx.lineTo(Math.sin(rel) * (hsiR - 10), -Math.cos(rel) * (hsiR - 10));
  ctx.stroke();
  ctx.restore();

  const headingLabels = [[0, "N"], [90, "E"], [180, "S"], [270, "W"]];
  headingLabels.forEach(([deg, label]) => {
    const relDeg = deg - headingDeg;
    const a = ((relDeg - 90) * Math.PI) / 180;
    const tx = hsiCx + Math.cos(a) * (hsiR - 24);
    const ty = hsiCy + Math.sin(a) * (hsiR - 24);
    pfdText(ctx, label, tx, ty, 18, "#f8fafc");
  });
  drawRoundedRect(ctx, hsiAreaX + 10, hsiAreaY + 10, 82, 24, 8);
  ctx.fillStyle = "#08111d";
  ctx.fill();
  ctx.strokeStyle = "#334155";
  ctx.lineWidth = 1;
  ctx.stroke();
  pfdText(ctx, `CRS ${pfdPad(courseDeg, 3)}`, hsiAreaX + 20, hsiAreaY + 22, 13, "#f8fafc", "left");
  const lat = Number(snapshot.lat_1e7 ?? 0) / 1e7;
  const lon = Number(snapshot.lon_1e7 ?? 0) / 1e7;
  const hsiFooterY = hsiAreaY + hsiAreaH - 18;
  drawRoundedRect(ctx, hsiAreaX + 10, hsiFooterY - 12, 120, 24, 8);
  ctx.fillStyle = "#08111d";
  ctx.fill();
  ctx.strokeStyle = "#334155";
  ctx.stroke();
  pfdText(ctx, `LAT ${fmt(lat, 5)}`, hsiAreaX + 18, hsiFooterY, 13, "#f8fafc", "left");
  drawRoundedRect(ctx, w - hsiAreaX - 120 - 10, hsiFooterY - 12, 120, 24, 8);
  ctx.fillStyle = "#08111d";
  ctx.fill();
  ctx.strokeStyle = "#334155";
  ctx.stroke();
  pfdText(ctx, `LON ${fmt(lon, 5)}`, w - hsiAreaX - 18, hsiFooterY, 13, "#f8fafc", "right");
}

function renderHeader(snapshot, deltaMs) {
  const now = Date.now();
  const msgAgeMs = dbgLastMsgMs ? (now - dbgLastMsgMs) : null;
  const updateHz = liveUpdateHz(deltaMs);
  const seq = snapshot ? dash(snapshot.seq) : "-";
  const ageMs = snapshot ? dash(snapshot.age_ms) : "-";
  const mode = sourceMode(snapshot);
  const link = linkState(snapshot);
  statsEl.innerHTML =
    `<span class="stats-line"><span>link: ${link}</span><span>update: ${updateHz === null ? "-" : fmt(updateHz, 1)} Hz</span><span>age: ${ageMs}</span><span>seq: ${seq}</span><span>mode: ${mode}</span></span>` +
    `<span class="stats-line"><span>ws:${dbgMsgHz}/s</span><span>apply:${dbgApplyHz}/s</span><span>render:${dbgRenderHz}/s</span><span>msg_age:${msgAgeMs === null ? "-" : Math.round(msgAgeMs)}</span></span>`;
}

function activeTabName() {
  const activeButton = document.querySelector(".tabs button.active");
  return activeButton ? activeButton.dataset.tab : "pfd";
}

function renderGpsPanel(snapshot) {
  if (!snapshot) {
    gpsEl.textContent = "fix: - sats: -\nlat: - lon: -\nspeed: - m/s course: - deg\nhAcc: - m sAcc: - m/s";
    return;
  }
  gpsEl.textContent =
`fix: ${dash(snapshot.gps_fix_type)} sats: ${dash(snapshot.gps_num_sv)}
lat: ${fmt(Number(snapshot.lat_1e7 || 0) / 1e7, 7)} lon: ${fmt(Number(snapshot.lon_1e7 || 0) / 1e7, 7)}
speed: ${fmt(Number(snapshot.gSpeed_mms || 0) / 1000, 2)} m/s course: ${fmt(Number(snapshot.headMot_1e5deg || 0) / 100000, 2)} deg
hAcc: ${fmt(Number(snapshot.hAcc_mm || 0) / 1000, 2)} m sAcc: ${fmt(Number(snapshot.sAcc_mms || 0) / 1000, 2)} m/s`;
}

function renderAttPanel(snapshot) {
  if (!snapshot) {
    attEl.textContent = "roll: - deg\npitch: - deg\nyaw: - deg\n\nfusion.gain: -\nfusion.accelRej: - deg\nfusion.magRej: - deg\nfusion.recovery: - s / - smp";
    return;
  }
  attEl.textContent =
`roll: ${fmt(snapshot.roll_deg, 2)} deg
pitch: ${fmt(snapshot.pitch_deg, 2)} deg
yaw: ${fmt(snapshot.yaw_deg, 2)} deg
air_online: ${snapshot.air_online ? "yes" : "no"}

fusion.gain: ${fmt(snapshot.fusion_gain, 3)}
fusion.accelRej: ${fmt(snapshot.fusion_accel_rej, 1)} deg
fusion.magRej: ${fmt(snapshot.fusion_mag_rej, 1)} deg
fusion.recovery: ${(Number(snapshot.fusion_recovery_period || 0) / 400).toFixed(1)} s / ${Math.round(Number(snapshot.fusion_recovery_period || 0))} smp`;
}

function renderBaroPanel(snapshot) {
  if (!snapshot) {
    baroEl.textContent = "alt: - m\nvsi: - m/s\npress: - hPa\ntemp: - C";
    return;
  }
  baroEl.textContent =
`alt: ${fmt(snapshot.baro_alt_m, 2)} m
vsi: ${fmt(snapshot.baro_vsi_mps, 2)} m/s
press: ${fmt(snapshot.baro_press_hpa, 2)} hPa
temp: ${fmt(snapshot.baro_temp_c, 2)} C`;
}

function renderLinkPanel(snapshot) {
  if (!snapshot) {
    linkEl.textContent = "link: waiting\nupdate_hz: -\nage_ms: -\nseq: -\nsource_mode: -\nactivity: -";
    return;
  }
  const updateHz = lastMessageMs ? liveUpdateHz(Date.now() - lastMessageMs) : null;
  const mode = sourceMode(snapshot);
  const activity = snapshot.replay_active || snapshot.replay_paused || snapshot.replay_file_open
    ? "replay"
    : (snapshot.recording_active ? "recording" : "live");
  linkEl.textContent =
`link: ${linkState(snapshot)}
update_hz: ${updateHz === null ? "-" : fmt(updateHz, 1)}
age_ms: ${dash(snapshot.age_ms)}
seq: ${dash(snapshot.seq)}
source_mode: ${mode}
activity: ${activity}
recording_active: ${snapshot.recording_active ? "on" : "off"}
replay_active: ${snapshot.replay_active ? "on" : "off"}
gps_datetime: ${gpsDateTimeText(snapshot)}`;
}

function renderLogsPanel(snapshot) {
  const activeTxt = snapshot ? (snapshot.recording_active ? "on" : "off") : "-";
  const busyTxt = snapshot ? (snapshot.recording_busy ? "on" : "off") : "-";
  logsEl.textContent =
`status: ${activeTxt}
busy: ${busyTxt}
session_id: ${snapshot ? dash(snapshot.recording_session_id) : "-"}
bytes_written: ${snapshot ? fmtBytes(snapshot.recording_bytes_written) : "-"}
pending_control: ${dash(pendingControl)}
storage_known: ${latestStorage ? (latestStorage.known ? "yes" : "no") : "-"}
storage_ready: ${latestStorage ? (latestStorage.backend_ready ? "yes" : "no") : "-"}
storage_busy: ${latestStorage ? (latestStorage.busy ? "yes" : "no") : "-"}
file_count: ${latestStorage ? dash(latestStorage.file_count) : "-"}
record_prefix: ${latestStorage ? dash(latestStorage.record_prefix) : "-"}
selected_file: ${latestFiles && latestFiles.files && latestFiles.files[0] ? latestFiles.files[0].name : "-"}
gps_time: ${snapshot ? gpsDateTimeText(snapshot) : "-"}`;
  airLoggerTextEl.textContent = snapshot ? (snapshot.recording_active ? "AIR recorder is on" : "AIR recorder is off in firmware") : "Checking AIR recorder...";
}

function updateStatus(snapshot) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    statusEl.textContent = "Disconnected";
    return;
  }
  if (!snapshot) {
    statusEl.textContent = "Connected / waiting for AIR";
    return;
  }
  statusEl.textContent = snapshot.fresh ? "Connected" : "Con/Stale";
}

function renderAll(snapshot, deltaMs) {
  dbgRenderCount += 1;
  dbgLastRenderMs = Date.now();
  if (snapshot) {
    dbgLastDisplaySeq = snapshot.seq ?? null;
    dbgLastDisplayRoll = snapshot.roll_deg ?? null;
    dbgLastDisplayPitch = snapshot.pitch_deg ?? null;
    dbgLastDisplayAlt = snapshot.baro_alt_m ?? null;
  }
  updateStatus(snapshot);
  renderHeader(snapshot, deltaMs);
  if (!snapshot) {
    recEl.className = "rec unknown";
    recEl.textContent = "REC --";
  } else if (snapshot.recording_active) {
    recEl.className = "rec on";
    recEl.textContent = "REC ON";
  } else {
    recEl.className = "rec off";
    recEl.textContent = "REC OFF";
  }
  switch (activeTabName()) {
    case "position":
      renderGpsPanel(snapshot);
      renderBaroPanel(snapshot);
      break;
    case "att":
      renderAttPanel(snapshot);
      updateFusionUi(snapshot);
      break;
    case "link":
      renderLinkPanel(snapshot);
      break;
    case "logs":
      renderLogsPanel(snapshot);
      break;
    case "pfd":
    default:
      renderPfdPanel(pfdRenderSnapshot());
      break;
  }
}

function sendControl(payload) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  pendingControl = `${payload.category}/${payload.action} req=${payload.req_id}`;
  appendEvent(`tx ${pendingControl}`);
  ws.send(JSON.stringify(payload));
  renderLogsPanel(latestSnapshot);
}

function wsEndpointUrl() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  return `${proto}//${location.host}/ws`;
}

function connect(force) {
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }
  generation += 1;
  const localGen = generation;
  if (ws) {
    try { ws.close(); } catch (_) {}
    ws = null;
  }
  statusEl.textContent = force ? "Reconnecting" : "Connecting";
  const socketUrl = wsEndpointUrl();
  const socket = new WebSocket(socketUrl);
  ws = socket;
  socket.onopen = () => {
    if (localGen !== generation || ws !== socket) return;
    appendEvent("socket open");
    setButtonsEnabled(true);
    renderAll(latestSnapshot, null);
  };
  socket.onmessage = (event) => {
    if (localGen !== generation || ws !== socket) return;
    const msg = JSON.parse(event.data);
    if (msg.type === "hello") {
      appendEvent(`hello snapshot_hz=${dash(msg.snapshot_hz)}`);
      return;
    }
    if (msg.type === "snapshot") {
      const now = Date.now();
      const deltaMs = lastMessageMs ? (now - lastMessageMs) : null;
      lastMessageMs = now;
      dbgMsgCount += 1;
      dbgLastMsgMs = now;
      latestSnapshot = normalizeSnapshot(msg);
      dbgLastMsgSeq = latestSnapshot?.seq ?? null;
      dbgLastMsgAgeMs = latestSnapshot?.age_ms ?? null;
      dbgApplyCount += 1;
      dbgLastApplyMs = now;
      renderAll(latestSnapshot, deltaMs);
      return;
    }
    if (msg.type === "files") {
      latestFiles = msg;
      renderLogsPanel(latestSnapshot);
      filesJsonEl.textContent = JSON.stringify(msg.files || [], null, 2);
      return;
    }
    if (msg.type === "storage") {
      latestStorage = msg;
      renderLogsPanel(latestSnapshot);
      return;
    }
    if (msg.type === "ack") {
      pendingControl = null;
      appendEvent(`ack req=${dash(msg.req_id)} ${dash(msg.op)} ok=${msg.ok ? 1 : 0} code=${dash(msg.code)} ${dash(msg.detail)}`);
      renderLogsPanel(latestSnapshot);
    }
  };
  socket.onclose = () => {
    if (localGen !== generation || ws !== socket) return;
    ws = null;
    appendEvent("socket closed");
    setButtonsEnabled(false);
    renderAll(latestSnapshot, null);
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      if (localGen === generation) connect(false);
    }, 1000);
  };
  socket.onerror = () => {
    if (localGen !== generation || ws !== socket) return;
    appendEvent("socket error");
    console.error(`WebSocket connect failed: ${socketUrl}`);
  };
}

document.querySelectorAll(".tabs button").forEach((button) => {
  button.addEventListener("click", () => {
    document.querySelectorAll(".tabs button").forEach((el) => el.classList.toggle("active", el === button));
    document.querySelectorAll(".tab").forEach((tab) => tab.classList.toggle("active", tab.id === `tab-${button.dataset.tab}`));
    if (latestSnapshot) {
      if (button.dataset.tab === "pfd") renderPfdPanel(pfdRenderSnapshot());
    } else if (button.dataset.tab === "pfd") {
      renderPfdPanel(pfdRenderSnapshot());
    }
  });
});

gainSlider.addEventListener("input", () => { gainValueEl.textContent = fmt(gainSlider.value, 2); });
accelRejSlider.addEventListener("input", () => { accelRejValueEl.textContent = `${fmt(accelRejSlider.value, 0)} deg`; });
magRejSlider.addEventListener("input", () => { magRejValueEl.textContent = `${fmt(magRejSlider.value, 0)} deg`; });
recoverySlider.addEventListener("input", () => { recoveryValueEl.textContent = `${fmt(recoverySlider.value, 1)} s / ${Math.round(Number(recoverySlider.value) * 400)} smp`; });

startLogBtn.addEventListener("click", () => {
  sendControl({ type: "control", req_id: nextReqId(), category: "recording", action: "start" });
});

stopLogBtn.addEventListener("click", () => {
  sendControl({ type: "control", req_id: nextReqId(), category: "recording", action: "stop" });
});

refreshLogStatusBtn.addEventListener("click", () => {
  sendControl({ type: "control", req_id: nextReqId(), category: "file", action: "refresh" });
});

fusionApplyBtn.addEventListener("click", () => {
  sendControl({
    type: "control",
    req_id: nextReqId(),
    category: "fusion",
    action: "set",
    gain: Number(gainSlider.value),
    accelerationRejection: Number(accelRejSlider.value),
    magneticRejection: Number(magRejSlider.value),
    recoveryTriggerPeriod: Math.round(Number(recoverySlider.value) * 400)
  });
});

window.addEventListener("resize", () => {
  renderPfdPanel(pfdRenderSnapshot());
});

document.getElementById("applyRate").disabled = true;
document.getElementById("resetAirNetwork").disabled = true;
document.getElementById("downloadDiag").disabled = true;
document.getElementById("downloadWsEvents").disabled = true;
document.getElementById("downloadClientEvents").disabled = true;
document.getElementById("resetCounters").disabled = true;

setButtonsEnabled(false);
renderPfdPanel(pfdRenderSnapshot());
renderGpsPanel(null);
renderAttPanel(null);
renderBaroPanel(null);
renderLinkPanel(null);
renderLogsPanel(null);
connect(false);
