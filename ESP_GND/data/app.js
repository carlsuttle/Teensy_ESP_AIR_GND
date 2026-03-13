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

let wsCtrl = null;
let wsState = null;
let pingSentAt = 0;
let pingMs = null;
let pingAvgMs = null;
let pingSamples = [];
let wsLoss = 0;
let wsSeen = 0;
let lastWsSeq = 0;
let lastStateAt = 0;
let lastPongAt = 0;
let lastCtrlRxAt = 0;
let binaryRxCount = 0;
let binaryParseFailCount = 0;
let clientStateFps = 0;
let binaryRxCountLast = 0;
let binaryRxFpsLastMs = 0;
let radioStateFps = null;
let lastRadioStatePackets = null;
let lastRadioStatusAt = 0;
let lastBinarySeq = 0;
let lastSourceSeq = 0;
let lastSourceTus = 0;
let lastEspRxMs = 0;
let lastStateGapTotal = 0;
let lastStateDropTotal = 0;
let dqiScore = null;
let dqiSmoothed = null;
let dqiDetail = null;
let uplinkDqiScore = null;
let uplinkDqiSmoothed = null;
let uplinkDqiDetail = null;
let lastUplinkPingTimeout = 0;
const STATE_STALE_MS = 1500;
const PONG_FRESH_MS = 3000;
const LOSS_WINDOW_MS = 10000;
const TARGET_STATE_FPS = 30;
let lossWinStart = 0;
let lossWinSeen = 0;
let lossWinLost = 0;
let activeTab = "gps";
let latestGps = null;
let latestAtt = null;
let latestBaro = null;
let fusionLast = null;
let fusionFlagsLast = {
  initialising: false,
  angularRecovery: false,
  accelerationRecovery: false,
  magneticRecovery: false
};
let fusionUiHoldUntilMs = 0;
let fusionDraft = null;
let fusionDirty = false;
let ctrlCloseCode = "-";
let ctrlCloseReason = "-";
let ctrlCloseClean = "-";
let ctrlClientErrors = 0;
let ctrlReconnects = 0;
let ctrlReconnectTimer = null;
let ctrlErrorRecoveryTimer = null;
let hasCtrlConnectedOnce = false;
let lastCtrlOpenAt = 0;
let stateCloseCode = "-";
let stateCloseReason = "-";
let stateCloseClean = "-";
let stateClientErrors = 0;
let stateReconnects = 0;
let stateReconnectTimer = null;
let stateErrorRecoveryTimer = null;
let hasStateConnectedOnce = false;
let lastStateSocketOpenAt = 0;
let suppressStaleUntilMs = 0;
let forceStateRecoveryRender = false;
let lastForcedStateResetAt = 0;
let uiDirty = true;
let linkDirty = true;
let lastLinkRenderAt = 0;
let pfdLastW = 0;
let pfdLastH = 0;
let courseSetDeg = 0;
let courseEditOpen = false;
let courseEditBuffer = "";
let pfdTapTargets = [];
let nextCtrlReqId = 1;
let clientEventLog = [];
let statusFetchInFlight = false;
let linkStatus = {
  transport: "ESP-NOW",
  radio_state_only: false,
  radio_lr_mode: true,
  air_link_fresh: false,
  has_link_meta: false,
  ap_clients: 0,
  air_radio_ready: false,
  air_peer_known: false,
  air_recorder_on: false,
  air_rssi_valid: false,
  air_rssi_dbm: null,
  air_scan_age_ms: null,
  air_link_age_ms: null,
  air_sender_mac: "none",
  air_target_mac: "none",
  radio_rtt_ms: null,
  radio_rtt_avg_ms: null,
  last_radio_pong_ms: 0,
  uplink_ping_sent: 0,
  uplink_ping_ok: 0,
  uplink_ping_timeout: 0,
  uplink_ping_miss_streak: 0,
  last_uplink_ack_ms: 0,
  has_log_status: false,
  air_log_active: false,
  air_log_requested: false,
  air_log_backend_ready: false,
  air_log_media_present: false,
  air_log_last_command: 0,
  air_log_session_id: 0,
  air_log_bytes_written: 0,
  air_log_free_bytes: null,
  air_log_last_change_ms: null,
  state_packets: 0,
  state_seq_gap: 0,
  state_seq_rewind: 0,
  link_rx: 0,
  ok: 0,
  len_err: 0,
  unknown_msg: 0,
  drop: 0
};

const RECONNECT_DELAY_MS = 1000;
const STATE_RECONNECT_DELAY_MS = 200;
const SOCKET_CONNECT_TIMEOUT_MS = 8000;
const SOCKET_ERROR_RECOVERY_MS = 3000;
const STATE_STARTUP_GRACE_MS = 4000;
const STATE_HARD_STALE_MS = 3000;
const STATE_HARD_STALE_COOLDOWN_MS = 5000;
const FILE_OP_STALE_GRACE_MS = 5000;
const RENDER_PERIOD_MS = 50;
const LINK_RENDER_PERIOD_MS = 250;
const LINK_STATUS_PERIOD_MS = 1000;
const PING_AVG_WINDOW = 30;
const CLIENT_EVENT_CAPACITY = 256;
const FUSION_SAMPLE_RATE_HZ = 400;
const FUSION_DEFAULTS = {
  gain: 0.06,
  accelerationRejection: 20.0,
  magneticRejection: 60.0,
  recoveryTriggerPeriod: 1200
};
const STATE_FLAG_FUSION_INITIALISING = 1 << 1;
const STATE_FLAG_FUSION_ANGULAR_RECOVERY = 1 << 2;
const STATE_FLAG_FUSION_ACCELERATION_RECOVERY = 1 << 3;
const STATE_FLAG_FUSION_MAGNETIC_RECOVERY = 1 << 4;

const WS_STATE_MAGIC = 0x57535445;
const WS_STATE_VERSION = 2;

function parseBinaryState(buf) {
  if (!(buf instanceof ArrayBuffer)) return null;
  const dv = new DataView(buf);
  if (dv.byteLength < 12) return null;
  const magic = dv.getUint32(0, true);
  const version = dv.getUint16(4, true);
  if (magic !== WS_STATE_MAGIC) return null;

  let headerLen = 12;
  let payloadLen = 0;
  let wsSeq = 0;
  let stateSeq = 0;
  let sourceTus = 0;
  let espRxMs = 0;
  let flags = 0;

  if (version === 1) {
    payloadLen = dv.getUint16(6, true);
    wsSeq = dv.getUint32(8, true);
  } else if (version === 2) {
    if (dv.byteLength < 28) return null;
    headerLen = dv.getUint16(6, true);
    payloadLen = dv.getUint16(8, true);
    flags = dv.getUint16(10, true);
    wsSeq = dv.getUint32(12, true);
    stateSeq = dv.getUint32(16, true);
    sourceTus = dv.getUint32(20, true);
    espRxMs = dv.getUint32(24, true);
    if (headerLen < 28) return null;
  } else {
    return null;
  }

  if (payloadLen !== dv.byteLength - headerLen) return null;

  let o = headerLen;
  const m = {
    type: "state",
    w: wsSeq,
    ss: stateSeq,
    stu: sourceTus,
    erm: espRxMs,
    fl: flags,
    a: {},
    g: {},
    b: {},
    f: {}
  };
  m.a.r = dv.getFloat32(o, true); o += 4;
  m.a.p = dv.getFloat32(o, true); o += 4;
  m.a.y = dv.getFloat32(o, true); o += 4;

  m.g.it = dv.getUint32(o, true); o += 4;
  m.g.fx = dv.getUint8(o); o += 1;
  m.g.sv = dv.getUint8(o); o += 1;
  m.g.la = dv.getInt32(o, true) * 1e-7; o += 4;
  m.g.lo = dv.getInt32(o, true) * 1e-7; o += 4;
  m.g.hm = dv.getInt32(o, true) / 1000.0; o += 4;
  m.g.gs = dv.getInt32(o, true) / 1000.0; o += 4;
  m.g.cr = dv.getInt32(o, true) / 100000.0; o += 4;
  m.g.ha = dv.getUint32(o, true) / 1000.0; o += 4;
  m.g.sa = dv.getUint32(o, true) / 1000.0; o += 4;

  m.g.pe = dv.getUint32(o, true); o += 4;
  m.mtok = dv.getUint32(o, true); o += 4;
  m.mdrop = dv.getUint32(o, true); o += 4;

  m.g.lgm = dv.getUint32(o, true); o += 4;
  m.a.lim = dv.getUint32(o, true); o += 4;
  m.b.lbm = dv.getUint32(o, true); o += 4;

  m.b.t = dv.getFloat32(o, true); o += 4;
  m.b.p = dv.getFloat32(o, true); o += 4;
  m.b.a = dv.getFloat32(o, true); o += 4;
  m.b.v = dv.getFloat32(o, true); o += 4;

  m.f.g = dv.getFloat32(o, true); o += 4;
  m.f.ar = dv.getFloat32(o, true); o += 4;
  m.f.mr = dv.getFloat32(o, true); o += 4;
  m.f.rp = dv.getUint16(o, true); o += 2;
  m.flags = dv.getUint16(o, true); o += 2;
  return m;
}

function near(a, b, eps) {
  return Math.abs(Number(a) - Number(b)) <= eps;
}

function sliderValue(id) {
  const el = document.getElementById(id);
  return el ? Number(el.value) : NaN;
}

function recoverySamplesToSeconds(samples) {
  return Number(samples || 0) / FUSION_SAMPLE_RATE_HZ;
}

function recoverySecondsToSamples(seconds) {
  return Math.max(1, Math.round(Number(seconds || 0) * FUSION_SAMPLE_RATE_HZ));
}

function fusionRecoveryText(samples) {
  if (samples === null || samples === undefined || Number.isNaN(samples)) return "-";
  return `${fmt(recoverySamplesToSeconds(samples), 1)} s / ${dash(Math.round(Number(samples)))} smp`;
}

function setFusionUiValues(gain, acc, mag, rec) {
  document.getElementById("gainSlider").value = gain;
  document.getElementById("accelRejSlider").value = acc;
  document.getElementById("magRejSlider").value = mag;
  document.getElementById("recoverySlider").value = recoverySamplesToSeconds(rec).toFixed(1);
  document.getElementById("gainValue").textContent = fmt(gain, 2);
  document.getElementById("accelRejValue").textContent = `${fmt(acc, 0)} deg`;
  document.getElementById("magRejValue").textContent = `${fmt(mag, 0)} deg`;
  document.getElementById("recoveryValue").textContent = fusionRecoveryText(rec);
}

function renderFusionDraftValues() {
  if (!fusionDraft) return;
  document.getElementById("gainValue").textContent = fmt(fusionDraft.gain, 2);
  document.getElementById("accelRejValue").textContent = `${fmt(fusionDraft.accelerationRejection, 0)} deg`;
  document.getElementById("magRejValue").textContent = `${fmt(fusionDraft.magneticRejection, 0)} deg`;
  document.getElementById("recoveryValue").textContent = fusionRecoveryText(fusionDraft.recoveryTriggerPeriod);
}

function ensureFusionDraft() {
  if (!fusionDraft) {
    const current = fusionLast || FUSION_DEFAULTS;
    fusionDraft = {
      gain: Number(current.gain ?? FUSION_DEFAULTS.gain),
      accelerationRejection: Number(current.accelerationRejection ?? FUSION_DEFAULTS.accelerationRejection),
      magneticRejection: Number(current.magneticRejection ?? FUSION_DEFAULTS.magneticRejection),
      recoveryTriggerPeriod: Number(current.recoveryTriggerPeriod ?? FUSION_DEFAULTS.recoveryTriggerPeriod)
    };
  }
}

function requestFusionSnapshot() {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  wsCtrl.send(JSON.stringify({ type: "get_fusion", req_id: allocCtrlReqId() }));
}

function requestLogStatus() {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  wsCtrl.send(JSON.stringify({ type: "get_log_status", req_id: allocCtrlReqId() }));
}

function sendLogStart() {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  wsCtrl.send(JSON.stringify({ type: "start_log", req_id: allocCtrlReqId() }));
}

function sendLogStop() {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  wsCtrl.send(JSON.stringify({ type: "stop_log", req_id: allocCtrlReqId() }));
}

function sendFusion(partial = {}) {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  ensureFusionDraft();
  fusionDraft = {
    ...fusionDraft,
    ...partial
  };
  fusionDirty = true;
  fusionUiHoldUntilMs = Date.now() + 15000;
  const payload = {
    type: "set_fusion",
    req_id: allocCtrlReqId(),
    fusion: {
      gain: Number(fusionDraft.gain || 0),
      accelerationRejection: Number(fusionDraft.accelerationRejection || 0),
      magneticRejection: Number(fusionDraft.magneticRejection || 0),
      recoveryTriggerPeriod: Math.round(Number(fusionDraft.recoveryTriggerPeriod || 0))
    }
  };
  renderFusionDraftValues();
  wsCtrl.send(JSON.stringify(payload));
}

function setStatus(s) {
  statusEl.textContent = s;
}

function fmt(v, n = 3) {
  if (v === null || v === undefined || Number.isNaN(v)) return "-";
  return Number(v).toFixed(n);
}

function fmtMs(v) {
  if (v === null || v === undefined || v === "-" || Number.isNaN(v) || Number(v) <= 0) return "-";
  return `${Math.round(Number(v))} ms`;
}

function clamp(v, lo, hi) {
  return Math.min(Math.max(Number(v), lo), hi);
}

function fmtPct(v, digits = 0) {
  if (v === null || v === undefined || Number.isNaN(v)) return "-";
  return `${Number(v).toFixed(digits)}%`;
}

function dqiBand(score) {
  if (score === null || score === undefined || Number.isNaN(score)) return "unknown";
  if (score >= 90) return "excellent";
  if (score >= 75) return "good";
  if (score >= 55) return "fair";
  if (score >= 35) return "poor";
  return "critical";
}

function dqiBadgeClass(score) {
  const band = dqiBand(score);
  return `dqi-badge dqi-${band}`;
}

function updateDataQuality() {
  const staleMs = lastStateAt ? (Date.now() - lastStateAt) : Infinity;
  const stateFresh = isFresh(lastStateAt, STATE_STALE_MS);
  const radioFresh = !!linkStatus.air_link_fresh;
  const rateNow = stateFresh ? clientStateFps : 0;
  const rateRatio = clamp(rateNow / TARGET_STATE_FPS, 0, 1);
  const radioRateRatio = (radioStateFps === null || !radioFresh) ? 0 : clamp(radioStateFps / TARGET_STATE_FPS, 0, 1);
  const radioRtt = Number(linkStatus.radio_rtt_ms ?? 0);
  const radioRttPenalty = !radioFresh ? 0 : clamp((radioRtt - 80) / 220, 0, 1);
  const stalePenalty = !stateFresh ? 1 : clamp((staleMs - 300) / 1200, 0, 1);
  const gapTotal = Number(linkStatus.state_seq_gap ?? 0);
  const dropTotal = Number(linkStatus.drop ?? 0);
  const gapDelta = Math.max(0, gapTotal - lastStateGapTotal);
  const dropDelta = Math.max(0, dropTotal - lastStateDropTotal);
  const deliveredPackets = Math.max(0, Number(linkStatus.state_packets ?? 0) - Number(lastRadioStatePackets ?? 0));
  const radioLossRatio = clamp((gapDelta + dropDelta) / Math.max(1, deliveredPackets + gapDelta + dropDelta), 0, 1);
  const wsLossRatio = clamp(Number(lossWinLost) / Math.max(1, Number(lossWinSeen) + Number(lossWinLost)), 0, 1);

  let score = 100;
  const penaltyRadioFresh = !radioFresh ? 30 : 0;
  const penaltyStateFresh = !stateFresh ? 30 : 0;
  const penaltyRate = (1 - rateRatio) * 20;
  const penaltyRadioRate = (1 - Math.max(rateRatio, radioRateRatio)) * 10;
  const penaltyRadioLoss = radioLossRatio * 35;
  const penaltyWsLoss = wsLossRatio * 15;
  const penaltyRtt = radioRttPenalty * 8;
  const penaltyStale = stalePenalty * 12;
  score -= penaltyRadioFresh;
  score -= penaltyStateFresh;
  score -= penaltyRate;
  score -= penaltyRadioRate;
  score -= penaltyRadioLoss;
  score -= penaltyWsLoss;
  score -= penaltyRtt;
  score -= penaltyStale;
  score = Math.round(clamp(score, 0, 100));

  dqiScore = score;
  dqiSmoothed = dqiSmoothed === null ? score : Math.round((dqiSmoothed * 0.7) + (score * 0.3));
  dqiDetail = {
    band: dqiBand(dqiSmoothed),
    rateRatio,
    radioRateRatio,
    radioLossRatio,
    wsLossRatio,
    staleMs: Number.isFinite(staleMs) ? staleMs : null,
    gapDelta,
    dropDelta,
    penalties: {
      radioFresh: Math.round(penaltyRadioFresh * 10) / 10,
      stateFresh: Math.round(penaltyStateFresh * 10) / 10,
      rate: Math.round(penaltyRate * 10) / 10,
      radioRate: Math.round(penaltyRadioRate * 10) / 10,
      radioLoss: Math.round(penaltyRadioLoss * 10) / 10,
      wsLoss: Math.round(penaltyWsLoss * 10) / 10,
      rtt: Math.round(penaltyRtt * 10) / 10,
      stale: Math.round(penaltyStale * 10) / 10
    }
  };
  lastStateGapTotal = gapTotal;
  lastStateDropTotal = dropTotal;
}

function updateUplinkDataQuality() {
  const airLinkAgeMs = Number(linkStatus.air_link_age_ms ?? Infinity);
  const airLinkFresh = !!linkStatus.air_link_fresh;
  const missStreak = Number(linkStatus.uplink_ping_miss_streak ?? 0);
  const timeoutTotal = Number(linkStatus.uplink_ping_timeout ?? 0);
  const timeoutBaselineUnset = uplinkDqiSmoothed === null && lastUplinkPingTimeout === 0;
  const timeoutDelta = timeoutBaselineUnset ? 0 : Math.max(0, timeoutTotal - lastUplinkPingTimeout);
  const rttMs = Number(linkStatus.radio_rtt_avg_ms ?? linkStatus.radio_rtt_ms ?? 0);
  const freshRatio = airLinkFresh ? 1 : clamp(1 - ((airLinkAgeMs - 2500) / 4000), 0, 1);
  const agePenalty = Number.isFinite(airLinkAgeMs) ? clamp((airLinkAgeMs - 2000) / 3000, 0, 1) : 1;
  const rttPenalty = clamp((rttMs - 120) / 200, 0, 1);
  const timeoutPenalty = clamp(timeoutDelta / 2, 0, 1);
  const missPenaltyBase = clamp(missStreak / 8, 0, 1);
  const missPenalty = (1 - freshRatio) > 0.05 ? missPenaltyBase : (missPenaltyBase * 0.15);

  let score = 100;
  const penaltyFresh = (1 - freshRatio) * 55;
  const penaltyAge = agePenalty * 8;
  const penaltyMiss = missPenalty * 12;
  const penaltyTimeout = timeoutPenalty * 12;
  const penaltyRtt = rttPenalty * 4;
  score -= penaltyFresh;
  score -= penaltyAge;
  score -= penaltyMiss;
  score -= penaltyTimeout;
  score -= penaltyRtt;
  score = Math.round(clamp(score, 0, 100));

  uplinkDqiScore = score;
  uplinkDqiSmoothed = uplinkDqiSmoothed === null ? score : Math.round((uplinkDqiSmoothed * 0.7) + (score * 0.3));
  uplinkDqiDetail = {
    band: dqiBand(uplinkDqiSmoothed),
    freshRatio,
    airLinkAgeMs: Number.isFinite(airLinkAgeMs) ? airLinkAgeMs : null,
    missStreak,
    rttMs: rttMs > 0 ? rttMs : null,
    timeoutDelta,
    penalties: {
      fresh: Math.round(penaltyFresh * 10) / 10,
      age: Math.round(penaltyAge * 10) / 10,
      miss: Math.round(penaltyMiss * 10) / 10,
      timeout: Math.round(penaltyTimeout * 10) / 10,
      rtt: Math.round(penaltyRtt * 10) / 10
    }
  };
  lastUplinkPingTimeout = timeoutTotal;
}

function dash(v) {
  return (v === null || v === undefined || Number.isNaN(v)) ? "-" : String(v);
}

function fmtRssi(valid, dbm) {
  if (!valid || dbm === null || dbm === undefined || Number.isNaN(dbm)) return "-";
  return `${Math.round(Number(dbm))} dBm`;
}

function fmtBytes(bytes) {
  if (bytes === null || bytes === undefined || Number.isNaN(bytes)) return "-";
  if (Number(bytes) === 0xFFFFFFFF) return "unknown";
  const value = Number(bytes);
  if (value >= 1024 * 1024) return `${fmt(value / (1024 * 1024), 2)} MiB`;
  if (value >= 1024) return `${fmt(value / 1024, 1)} KiB`;
  return `${Math.round(value)} B`;
}

function logCommandText(command) {
  if (command === 104) return "start";
  if (command === 105) return "stop";
  if (command === 106) return "status";
  return command ? String(command) : "-";
}

function setFusionLight(el, on) {
  if (!el) return;
  el.classList.toggle("on", !!on);
  el.classList.toggle("off", !on);
}

function renderFusionLights() {
  setFusionLight(fusionAngularLightEl, fusionFlagsLast.angularRecovery);
  setFusionLight(fusionAccelLightEl, fusionFlagsLast.accelerationRecovery);
  setFusionLight(fusionMagLightEl, fusionFlagsLast.magneticRecovery);
}

function applyLogStatus(logStatus = null) {
  if (!logStatus) return;
  linkStatus.has_log_status = true;
  linkStatus.air_log_active = !!logStatus.active;
  linkStatus.air_log_requested = !!logStatus.requested;
  linkStatus.air_log_backend_ready = !!logStatus.backend_ready;
  linkStatus.air_log_media_present = !!logStatus.media_present;
  linkStatus.air_log_last_command = Number(logStatus.last_command ?? 0);
  linkStatus.air_log_session_id = Number(logStatus.session_id ?? 0);
  linkStatus.air_log_bytes_written = Number(logStatus.bytes_written ?? 0);
  linkStatus.air_log_free_bytes = Number(logStatus.free_bytes ?? 0xFFFFFFFF);
  linkStatus.air_log_last_change_ms = Number(logStatus.last_change_ms ?? 0);
}

function setRecorderUi(enabled, known = true) {
  recEl.classList.remove("on", "off", "unknown");
  if (!known) {
    recEl.classList.add("unknown");
    recEl.textContent = "REC --";
    airLoggerTextEl.textContent = "Waiting for AIR recorder status...";
    return;
  }
  if (enabled) {
    recEl.classList.add("on");
    recEl.textContent = "REC ON";
    airLoggerTextEl.textContent = "AIR recorder is on";
    return;
  }
  recEl.classList.add("off");
  recEl.textContent = "REC OFF";
  airLoggerTextEl.textContent = "AIR recorder is off in firmware";
}

function allocCtrlReqId() {
  const id = nextCtrlReqId;
  nextCtrlReqId = (nextCtrlReqId % 2147483647) + 1;
  return id;
}

function resetPingStats() {
  pingMs = null;
  pingAvgMs = null;
  pingSamples = [];
}

function resetLocalCounters() {
  resetPingStats();
  wsLoss = 0;
  wsSeen = 0;
  lastWsSeq = 0;
  binaryRxCount = 0;
  binaryParseFailCount = 0;
  clientStateFps = 0;
  binaryRxCountLast = 0;
  binaryRxFpsLastMs = 0;
  lastBinarySeq = 0;
  lastSourceSeq = 0;
  lastSourceTus = 0;
  lastEspRxMs = 0;
  lastStateGapTotal = 0;
  lastStateDropTotal = 0;
  dqiScore = null;
  dqiSmoothed = null;
  dqiDetail = null;
  uplinkDqiScore = null;
  uplinkDqiSmoothed = null;
  uplinkDqiDetail = null;
  lastUplinkPingTimeout = 0;
  lossWinStart = 0;
  lossWinSeen = 0;
  lossWinLost = 0;
  ctrlCloseCode = "-";
  ctrlCloseReason = "-";
  ctrlCloseClean = "-";
  ctrlClientErrors = 0;
  ctrlReconnects = 0;
  stateCloseCode = "-";
  stateCloseReason = "-";
  stateCloseClean = "-";
  stateClientErrors = 0;
  stateReconnects = 0;
  clientEventLog = [];
  forceStateRecoveryRender = true;
  uiDirty = true;
  linkDirty = true;
}

function recordPingSample(ms) {
  if (ms === null || ms === undefined || Number.isNaN(ms)) return;
  pingSamples.push(Number(ms));
  if (pingSamples.length > PING_AVG_WINDOW) {
    pingSamples.shift();
  }
  const sum = pingSamples.reduce((acc, v) => acc + v, 0);
  pingAvgMs = pingSamples.length ? Math.round(sum / pingSamples.length) : null;
}

function recordClientEvent(socketKind, eventKind, code = "", reason = "", clean = "") {
  clientEventLog.push({
    ms: Date.now(),
    socket: socketKind,
    event: eventKind,
    code,
    reason,
    clean
  });
  if (clientEventLog.length > CLIENT_EVENT_CAPACITY) {
    clientEventLog.shift();
  }
}

function isCtrlOpen() {
  return !!wsCtrl && wsCtrl.readyState === WebSocket.OPEN;
}

function isStateOpen() {
  return !!wsState && wsState.readyState === WebSocket.OPEN;
}

function isFresh(ts, maxAgeMs) {
  return ts && ((Date.now() - ts) <= maxAgeMs);
}

function isPongFresh() {
  return isFresh(lastPongAt, PONG_FRESH_MS);
}

function waitingForTelemetry() {
  if (!isCtrlOpen() || !isStateOpen()) return false;
  return !lastStateAt;
}

function telemetryLooksStale() {
  if (Date.now() < suppressStaleUntilMs) return false;
  if (!isStateOpen()) return true;
  if (!lastStateAt) return false;
  return (Date.now() - lastStateAt) > STATE_STALE_MS;
}

function updateStatus() {
  if (!isCtrlOpen()) {
    setStatus("Disconnected");
    return;
  }
  if (linkStatus.has_link_meta && !linkStatus.air_link_fresh) {
    setStatus("Connected / AIR link waiting");
    return;
  }
  if (waitingForTelemetry()) {
    setStatus("Connected / waiting for AIR");
    return;
  }
  setStatus(telemetryLooksStale() ? "Con/Stale" : "Connected");
}

async function fetchStatus() {
  if (statusFetchInFlight) return;
  statusFetchInFlight = true;
  try {
    const resp = await fetch("/api/status", { cache: "no-store" });
    if (!resp.ok) throw new Error(`status ${resp.status}`);
    const data = await resp.json();
    linkStatus = {
      ...linkStatus,
      ...data
    };
    const now = Date.now();
    const statePackets = Number(linkStatus.state_packets ?? 0);
    if (lastRadioStatusAt > 0 && lastRadioStatePackets !== null && statePackets >= lastRadioStatePackets) {
      const dtMs = now - lastRadioStatusAt;
      if (dtMs > 0) {
        radioStateFps = (statePackets - lastRadioStatePackets) * 1000 / dtMs;
      }
    } else if (lastRadioStatePackets !== null && statePackets < lastRadioStatePackets) {
      radioStateFps = null;
    }
    updateDataQuality();
    updateUplinkDataQuality();
    lastRadioStatePackets = statePackets;
    lastRadioStatusAt = now;
    if (data.has_log_status) {
      applyLogStatus({
        active: data.air_log_active,
        requested: data.air_log_requested,
        backend_ready: data.air_log_backend_ready,
        media_present: data.air_log_media_present,
        last_command: data.air_log_last_command,
        session_id: data.air_log_session_id,
        bytes_written: data.air_log_bytes_written,
        free_bytes: data.air_log_free_bytes,
        last_change_ms: data.air_log_last_change_ms
      });
    }
    setRecorderUi(!!linkStatus.air_recorder_on, !!linkStatus.has_link_meta);
    uiDirty = true;
    linkDirty = true;
  } catch (_err) {
  } finally {
    statusFetchInFlight = false;
  }
}

function renderGpsStale() {
  gpsEl.textContent =
`fix: - sats: -
lat: - lon: -
speed: - m/s course: - deg
hAcc: - m sAcc: - m/s`;
}

function renderAttStale() {
  attEl.textContent =
`roll: - deg
pitch: - deg
yaw: - deg

fusion.gain: -
fusion.accelRej: - deg
fusion.magRej: - deg
fusion.recovery: - s / - smp`;
  fusionFlagsLast = {
    initialising: false,
    angularRecovery: false,
    accelerationRecovery: false,
    magneticRecovery: false
  };
  renderFusionLights();
}

function configurePfdCanvas() {
  if (!pfdCanvas) return null;
  const dpr = Math.max(1, Math.min(window.devicePixelRatio || 1, 2));
  const cssW = Math.max(320, Math.round(pfdCanvas.clientWidth || 390));
  const cssH = Math.round(cssW * (680 / 390));
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

function norm360(value) {
  return ((Number(value) % 360) + 360) % 360;
}

function updateCourseSetUi() {
}

function setCourseSetDeg(value) {
  courseSetDeg = norm360(value);
  try {
    localStorage.setItem("pfd_course_deg", String(courseSetDeg));
  } catch (_err) {
  }
  updateCourseSetUi();
  if (activeTab === "pfd") {
    uiDirty = true;
    renderNow();
  }
}

function setCourseEditOpen(open) {
  courseEditOpen = !!open;
  courseEditBuffer = courseEditOpen ? pfdPad(courseSetDeg, 3) : "";
  if (activeTab === "pfd") {
    uiDirty = true;
    renderNow();
  }
}

function appendCourseDigit(digit) {
  courseEditBuffer = `${courseEditBuffer}${digit}`.replace(/\D/g, "").slice(-3);
}

function commitCourseEdit() {
  if (!courseEditBuffer) return;
  setCourseSetDeg(Number(courseEditBuffer));
  setCourseEditOpen(false);
}

function setPfdTapTarget(id, x, y, w, h) {
  pfdTapTargets.push({ id, x, y, w, h });
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

function renderPfdPanel() {
  const view = configurePfdCanvas();
  if (!view) return;
  const { ctx, w, h } = view;
  const att = latestAtt || {};
  const gps = latestGps || {};
  const baro = latestBaro || {};
  const rollDeg = Number(att.r ?? 0);
  const pitchDeg = Number(att.p ?? 0);
  const yawDeg = Number(att.y ?? 0);
  const headingDeg = ((yawDeg % 360) + 360) % 360;
  const altM = Number(baro.a ?? gps.hm ?? 0);
  const vsiMps = Number(baro.v ?? 0);
  const gsMps = Number(gps.gs ?? 0);
  const fixType = Number(gps.fx ?? 0);
  const sats = Number(gps.sv ?? 0);
  const courseDeg = courseSetDeg;
  const horizonR = Math.min(w * 0.34, 165);
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
  const bankPointerR = horizonR;
  const bankPointerCx = horizonCx + Math.cos(bankPointerAngle) * bankPointerR;
  const bankPointerCy = horizonCy + Math.sin(bankPointerAngle) * bankPointerR;
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
  const vsiBoxW = 42;
  const vsiBoxH = 24;
  const vsiBoxX = altX + tapeW;
  const vsiBoxY = vsiY - (vsiBoxH * 0.5);
  ctx.fillStyle = "#020617";
  drawRoundedRect(ctx, vsiBoxX, vsiBoxY, vsiBoxW, vsiBoxH, 6);
  ctx.fill();
  ctx.strokeStyle = "#f8fafc";
  ctx.lineWidth = 1;
  ctx.stroke();
  pfdText(ctx, String(Math.abs(Math.round(vsiMps))), vsiBoxX + vsiBoxW - 6, vsiBoxY + 12, 14, "#f8fafc", "right");

  const hdgBoxY = 0;
  const hdgBoxW = 68;
  const hdgBoxH = 28;
  const hdgY = 10;
  const fixBoxX = 12;
  const fixBoxY = 8;
  const fixBoxW = 88;
  const fixBoxH = 24;
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
  drawRoundedRect(ctx, fixBoxX, fixBoxY, fixBoxW, fixBoxH, 8);
  ctx.fill();
  ctx.strokeStyle = "#475569";
  ctx.lineWidth = 1.5;
  ctx.stroke();
  pfdText(ctx, `FIX ${fixType}/${sats}`, fixBoxX + 8, fixBoxY + 12, 13, "#cbd5e1", "left");
  ctx.fillStyle = "#020617";
  drawRoundedRect(ctx, horizonCx - (hdgBoxW * 0.5), hdgBoxY, hdgBoxW, hdgBoxH, 10);
  ctx.fill();
  ctx.strokeStyle = "#f8fafc";
  ctx.lineWidth = 2;
  ctx.stroke();
  pfdText(ctx, pfdPad(headingDeg, 3), horizonCx, hdgBoxY + 14, 20, "#f8fafc");

  const hsiAreaX = 12;
  const hsiAreaY = tapeBottom + 18;
  const hsiAreaW = w - 24;
  const hsiAreaH = h - hsiAreaY - 12;
  const hsiCx = hsiAreaX + (hsiAreaW * 0.5);
  const hsiCy = hsiAreaY + (hsiAreaH * 0.5);
  const hsiR = Math.min(126, Math.max(75, Math.min(hsiAreaW, hsiAreaH) * 0.39));
  pfdTapTargets = [];
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
  ctx.rotate((courseDeg * Math.PI) / 180);
  ctx.strokeStyle = "#d946ef";
  ctx.lineWidth = 6;
  ctx.beginPath();
  ctx.moveTo(0, -hsiR + 12);
  ctx.lineTo(0, -14);
  ctx.moveTo(0, 14);
  ctx.lineTo(0, hsiR - 12);
  ctx.stroke();
  ctx.fillStyle = "#d946ef";
  ctx.beginPath();
  ctx.moveTo(0, -hsiR);
  ctx.lineTo(-9, -hsiR + 20);
  ctx.lineTo(9, -hsiR + 20);
  ctx.closePath();
  ctx.fill();
  ctx.restore();

  for (let deg = 0; deg < 360; deg += 30) {
    const relDeg = deg - headingDeg;
    const a = ((relDeg - 90) * Math.PI) / 180;
    const tx = hsiCx + Math.cos(a) * (hsiR - 24);
    const ty = hsiCy + Math.sin(a) * (hsiR - 24);
    if (deg === 0) {
      pfdText(ctx, "N", tx, ty, 18, "#f8fafc");
    } else if (deg === 90) {
      pfdText(ctx, "E", tx, ty, 18, "#f8fafc");
    } else if (deg === 180) {
      pfdText(ctx, "S", tx, ty, 18, "#f8fafc");
    } else if (deg === 270) {
      pfdText(ctx, "W", tx, ty, 18, "#f8fafc");
    } else {
      pfdText(ctx, String(deg).padStart(3, "0"), tx, ty, 15, "#cbd5e1");
    }
  }
  const crsBoxX = hsiAreaX + 10;
  const crsBoxY = hsiAreaY + 10;
  drawRoundedRect(ctx, crsBoxX, crsBoxY, 82, 24, 8);
  ctx.fillStyle = "#08111d";
  ctx.fill();
  ctx.strokeStyle = "#334155";
  ctx.lineWidth = 1;
  ctx.stroke();
  pfdText(ctx, `CRS ${(courseEditOpen ? courseEditBuffer : pfdPad(courseDeg, 3)).padStart(3, "0")}`, crsBoxX + 10, crsBoxY + 12, 13, "#f8fafc", "left");
  setPfdTapTarget("course_toggle", crsBoxX, crsBoxY, 82, 24);

  if (courseEditOpen) {
    const btnY = crsBoxY + 32;
    const btnW = 44;
    const btnH = 36;
    const btnGap = 8;
    const buttons = [
      { id: "course_1", x: crsBoxX, y: btnY, label: "1" },
      { id: "course_2", x: crsBoxX + btnW + btnGap, y: btnY, label: "2" },
      { id: "course_3", x: crsBoxX + (btnW + btnGap) * 2, y: btnY, label: "3" },
      { id: "course_4", x: crsBoxX, y: btnY + btnH + btnGap, label: "4" },
      { id: "course_5", x: crsBoxX + btnW + btnGap, y: btnY + btnH + btnGap, label: "5" },
      { id: "course_6", x: crsBoxX + (btnW + btnGap) * 2, y: btnY + btnH + btnGap, label: "6" },
      { id: "course_7", x: crsBoxX, y: btnY + (btnH + btnGap) * 2, label: "7" },
      { id: "course_8", x: crsBoxX + btnW + btnGap, y: btnY + (btnH + btnGap) * 2, label: "8" },
      { id: "course_9", x: crsBoxX + (btnW + btnGap) * 2, y: btnY + (btnH + btnGap) * 2, label: "9" },
      { id: "course_clr", x: crsBoxX, y: btnY + (btnH + btnGap) * 3, label: "C" },
      { id: "course_0", x: crsBoxX + btnW + btnGap, y: btnY + (btnH + btnGap) * 3, label: "0" },
      { id: "course_ent", x: crsBoxX + (btnW + btnGap) * 2, y: btnY + (btnH + btnGap) * 3, label: "OK" }
    ];
    buttons.forEach((btn) => {
      drawRoundedRect(ctx, btn.x, btn.y, btnW, btnH, 8);
      ctx.fillStyle = "#08111d";
      ctx.fill();
      ctx.strokeStyle = "#475569";
      ctx.stroke();
      pfdText(ctx, btn.label, btn.x + (btnW * 0.5), btn.y + 18, 16, "#f8fafc");
      setPfdTapTarget(btn.id, btn.x, btn.y, btnW, btnH);
    });
  }

  const hsiFooterY = hsiAreaY + hsiAreaH - 18;
  drawRoundedRect(ctx, hsiAreaX + 10, hsiFooterY - 12, 120, 24, 8);
  ctx.fillStyle = "#08111d";
  ctx.fill();
  ctx.strokeStyle = "#334155";
  ctx.stroke();
  pfdText(ctx, `LAT ${fmt(gps.la, 5)}`, hsiAreaX + 18, hsiFooterY, 13, "#f8fafc", "left");
  drawRoundedRect(ctx, w - hsiAreaX - 120 - 10, hsiFooterY - 12, 120, 24, 8);
  ctx.fillStyle = "#08111d";
  ctx.fill();
  ctx.strokeStyle = "#334155";
  ctx.stroke();
  pfdText(ctx, `LON ${fmt(gps.lo, 5)}`, w - hsiAreaX - 18, hsiFooterY, 13, "#f8fafc", "right");
}

function renderBaroStale() {
  baroEl.textContent =
`alt: - m
vsi: - m/s
press: - hPa
temp: - C`;
}

function renderLinkStale() {
linkEl.textContent =
`transport: ${dash(linkStatus.transport)}
air_link: -
air_radio: -
air_peer: -
air_sender_mac: ${dash(linkStatus.air_sender_mac)}
air_target_mac: ${dash(linkStatus.air_target_mac)}
air_rssi: -
air_scan_age_ms: -
air_link_age_ms: -
air_recorder: -
radio_rtt: -
radio_rtt_avg: -
radio_state_fps: -
ap_clients: ${dash(linkStatus.ap_clients)}
ctrl_ws: -
state_ws: -
state_fps: -
state_age_ms: -
source_seq: -
source_t_us: -
esp_rx_ms: -
link_rx: ${dash(linkStatus.link_rx)}
state_rx: ${dash(linkStatus.state_packets)}
state_gap: ${dash(linkStatus.state_seq_gap)}
state_rewind: ${dash(linkStatus.state_seq_rewind)}
frames_ok: ${dash(linkStatus.ok)}
len_err: ${dash(linkStatus.len_err)}
unknown_msg: ${dash(linkStatus.unknown_msg)}
drop: ${dash(linkStatus.drop)}
ws_loss: -
binary_parse_fail: -
ctrl_reconnects: -
state_reconnects: -
ctrl_last_close: -
state_last_close: -
ping: - ms
ping_avg: - ms`;
}

function renderLogsStale() {
  logsEl.textContent =
`status: -
requested: -
backend_ready: -
media_present: -
session_id: -
bytes_written: -
free_bytes: -
last_command: -
last_change_ms: -`;
}

function renderStale() {
  renderPfdStale();
  renderGpsStale();
  renderAttStale();
  renderBaroStale();
  renderLinkStale();
  renderLogsStale();
  statsEl.innerHTML =
    `<span class="stats-line"><span>fps: -</span><span>ping: -</span><span class="dqi-badge dqi-unknown">D:--</span><span class="dqi-badge dqi-unknown">U:--</span></span>`;
}

function renderHeader() {
  const pingAvgTxt = isPongFresh() ? pingAvgMs : null;
  const stateFpsTxt = isFresh(lastStateAt, STATE_STALE_MS) ? fmt(clientStateFps, 1) : "-";
  const dqiValueTxt = dqiSmoothed === null ? "--" : String(dqiSmoothed);
  const uplinkDqiValueTxt = uplinkDqiSmoothed === null ? "--" : String(uplinkDqiSmoothed);
  statsEl.innerHTML =
    `<span class="stats-line"><span>fps: ${stateFpsTxt}</span><span>ping: ${fmtMs(pingAvgTxt)}</span><span class="${dqiBadgeClass(dqiSmoothed)}">D:${dqiValueTxt}</span><span class="${dqiBadgeClass(uplinkDqiSmoothed)}">U:${uplinkDqiValueTxt}</span></span>`;
}

function renderGpsPanel() {
  const gps = latestGps || {};
  const fix = gps.fx ?? "-";
  const sats = gps.sv ?? "-";
  gpsEl.textContent =
`fix: ${dash(fix)} sats: ${dash(sats)}
lat: ${fmt(gps.la, 7)} lon: ${fmt(gps.lo, 7)}
speed: ${fmt(gps.gs, 2)} m/s course: ${fmt(gps.cr, 2)} deg
hAcc: ${fmt(gps.ha, 2)} m sAcc: ${fmt(gps.sa, 2)} m/s`;
}

function renderAttPanel() {
  const att = latestAtt || {};
  const fusion = fusionLast || {};
  attEl.textContent =
`roll: ${fmt(att.r, 2)} deg
pitch: ${fmt(att.p, 2)} deg
yaw: ${fmt(att.y, 2)} deg

fusion.gain: ${fmt(fusion.gain, 3)}
fusion.accelRej: ${fmt(fusion.accelerationRejection, 1)} deg
fusion.magRej: ${fmt(fusion.magneticRejection, 1)} deg
fusion.recovery: ${fusionRecoveryText(fusion.recoveryTriggerPeriod)}`;
  renderFusionLights();
}

function renderBaroPanel() {
  const baro = latestBaro || {};
  baroEl.textContent =
`alt: ${fmt(baro.a, 2)} m
vsi: ${fmt(baro.v, 2)} m/s
press: ${fmt(baro.p, 2)} hPa
temp: ${fmt(baro.t, 2)} C`;
}

function renderLinkPanel() {
  const pingTxt = isPongFresh() ? pingMs : null;
  const pingAvgTxt = isPongFresh() ? pingAvgMs : null;
  const radioRttTxt = linkStatus.air_link_fresh ? linkStatus.radio_rtt_ms : null;
  const radioRttAvgTxt = linkStatus.air_link_fresh ? linkStatus.radio_rtt_avg_ms : null;
  const radioStateFpsTxt = linkStatus.air_link_fresh && radioStateFps !== null ? fmt(radioStateFps, 1) : "-";
  const ctrlSocketTxt = isCtrlOpen() ? "open" : "closed";
  const stateSocketTxt = isStateOpen() ? "open" : "closed";
  const stateFpsTxt = isFresh(lastStateAt, STATE_STALE_MS) ? fmt(clientStateFps, 1) : "-";
  const stateAgeTxt = lastStateAt ? String(Date.now() - lastStateAt) : (waitingForTelemetry() ? "waiting" : "-");
  const airLinkTxt = linkStatus.air_link_fresh ? "up" : "waiting";
  const airRadioTxt = linkStatus.air_radio_ready ? "ready" : "starting";
  const airPeerTxt = linkStatus.air_peer_known ? "known" : "discovering";
  const radioModeTxt = linkStatus.radio_state_only ? "state-only stress" : "normal unified";
  const dqiScoreTxt = dqiSmoothed === null ? "-" : String(dqiSmoothed);
  const dqiBandTxt = dqiDetail ? dqiDetail.band : "unknown";
  const dqiRadioLossTxt = dqiDetail ? fmtPct(dqiDetail.radioLossRatio * 100, 1) : "-";
  const dqiWsLossTxt = dqiDetail ? fmtPct(dqiDetail.wsLossRatio * 100, 1) : "-";
  const dqiRateTxt = dqiDetail ? fmtPct(dqiDetail.rateRatio * 100, 0) : "-";
  const dqiAgeTxt = dqiDetail && dqiDetail.staleMs !== null ? String(Math.round(dqiDetail.staleMs)) : "-";
  const dqiPenaltyRadioFreshTxt = dqiDetail ? fmt(dqiDetail.penalties.radioFresh, 1) : "-";
  const dqiPenaltyStateFreshTxt = dqiDetail ? fmt(dqiDetail.penalties.stateFresh, 1) : "-";
  const dqiPenaltyRateTxt = dqiDetail ? fmt(dqiDetail.penalties.rate, 1) : "-";
  const dqiPenaltyRadioRateTxt = dqiDetail ? fmt(dqiDetail.penalties.radioRate, 1) : "-";
  const dqiPenaltyRadioLossTxt = dqiDetail ? fmt(dqiDetail.penalties.radioLoss, 1) : "-";
  const dqiPenaltyWsLossTxt = dqiDetail ? fmt(dqiDetail.penalties.wsLoss, 1) : "-";
  const dqiPenaltyRttTxt = dqiDetail ? fmt(dqiDetail.penalties.rtt, 1) : "-";
  const dqiPenaltyStaleTxt = dqiDetail ? fmt(dqiDetail.penalties.stale, 1) : "-";
  const uplinkDqiScoreTxt = uplinkDqiSmoothed === null ? "-" : String(uplinkDqiSmoothed);
  const uplinkDqiBandTxt = uplinkDqiDetail ? uplinkDqiDetail.band : "unknown";
  const uplinkFreshTxt = uplinkDqiDetail ? fmtPct(uplinkDqiDetail.freshRatio * 100, 0) : "-";
  const uplinkMissStreakTxt = uplinkDqiDetail ? String(uplinkDqiDetail.missStreak) : "-";
  const uplinkAirAgeTxt = uplinkDqiDetail && uplinkDqiDetail.airLinkAgeMs !== null ? String(Math.round(uplinkDqiDetail.airLinkAgeMs)) : "-";
  const uplinkRttTxt = uplinkDqiDetail && uplinkDqiDetail.rttMs !== null ? String(Math.round(uplinkDqiDetail.rttMs)) : "-";
  const uplinkPenaltyFreshTxt = uplinkDqiDetail ? fmt(uplinkDqiDetail.penalties.fresh, 1) : "-";
  const uplinkPenaltyAgeTxt = uplinkDqiDetail ? fmt(uplinkDqiDetail.penalties.age, 1) : "-";
  const uplinkPenaltyMissTxt = uplinkDqiDetail ? fmt(uplinkDqiDetail.penalties.miss, 1) : "-";
  const uplinkPenaltyTimeoutTxt = uplinkDqiDetail ? fmt(uplinkDqiDetail.penalties.timeout, 1) : "-";
  const uplinkPenaltyRttTxt = uplinkDqiDetail ? fmt(uplinkDqiDetail.penalties.rtt, 1) : "-";
  const airRssiTxt = fmtRssi(linkStatus.air_rssi_valid, linkStatus.air_rssi_dbm);
  const airScanAgeTxt = dash(linkStatus.air_scan_age_ms);
  const airLinkAgeTxt = dash(linkStatus.air_link_age_ms);
  const airRecorderTxt = !linkStatus.has_link_meta ? "unknown" : (linkStatus.air_recorder_on ? "on" : "off");
  linkEl.textContent =
`transport: ${dash(linkStatus.transport)}
radio_mode: ${radioModeTxt}
dqi: ${dqiScoreTxt} / 100
dqi_band: ${dqiBandTxt}
dqi_uplink: ${uplinkDqiScoreTxt} / 100
dqi_uplink_band: ${uplinkDqiBandTxt}
dqi_rate: ${dqiRateTxt}
dqi_radio_loss: ${dqiRadioLossTxt}
dqi_ws_loss: ${dqiWsLossTxt}
dqi_state_age_ms: ${dqiAgeTxt}
dqi_penalty_radio_fresh: ${dqiPenaltyRadioFreshTxt}
dqi_penalty_state_fresh: ${dqiPenaltyStateFreshTxt}
dqi_penalty_rate: ${dqiPenaltyRateTxt}
dqi_penalty_radio_rate: ${dqiPenaltyRadioRateTxt}
dqi_penalty_radio_loss: ${dqiPenaltyRadioLossTxt}
dqi_penalty_ws_loss: ${dqiPenaltyWsLossTxt}
dqi_penalty_rtt: ${dqiPenaltyRttTxt}
dqi_penalty_stale: ${dqiPenaltyStaleTxt}
uplink_miss_streak: ${uplinkMissStreakTxt}
uplink_fresh: ${uplinkFreshTxt}
uplink_air_age_ms: ${uplinkAirAgeTxt}
uplink_rtt_ms: ${uplinkRttTxt}
uplink_penalty_fresh: ${uplinkPenaltyFreshTxt}
uplink_penalty_age: ${uplinkPenaltyAgeTxt}
uplink_penalty_miss: ${uplinkPenaltyMissTxt}
uplink_penalty_timeout: ${uplinkPenaltyTimeoutTxt}
uplink_penalty_rtt: ${uplinkPenaltyRttTxt}
air_link: ${airLinkTxt}
air_radio: ${airRadioTxt}
air_peer: ${airPeerTxt}
air_sender_mac: ${dash(linkStatus.air_sender_mac)}
air_target_mac: ${dash(linkStatus.air_target_mac)}
air_rssi: ${airRssiTxt}
air_scan_age_ms: ${airScanAgeTxt}
air_link_age_ms: ${airLinkAgeTxt}
air_recorder: ${airRecorderTxt}
radio_rtt: ${fmtMs(radioRttTxt)}
radio_rtt_avg: ${fmtMs(radioRttAvgTxt)}
radio_state_fps: ${radioStateFpsTxt}
ap_clients: ${dash(linkStatus.ap_clients)}
web_fps: ${stateFpsTxt}
state_age_ms: ${stateAgeTxt}
link_rx: ${dash(linkStatus.link_rx)}
state_rx: ${dash(linkStatus.state_packets)}
state_gap: ${dash(linkStatus.state_seq_gap)}
state_rewind: ${dash(linkStatus.state_seq_rewind)}
frames_ok: ${dash(linkStatus.ok)}
len_err: ${dash(linkStatus.len_err)}
unknown_msg: ${dash(linkStatus.unknown_msg)}
drop: ${dash(linkStatus.drop)}
ping: ${fmtMs(pingTxt)}
ping_avg: ${fmtMs(pingAvgTxt)}`;
}

function renderLogsPanel() {
  const activeTxt = linkStatus.has_log_status ? (linkStatus.air_log_active ? "on" : "off") : "-";
  const requestedTxt = linkStatus.has_log_status ? (linkStatus.air_log_requested ? "on" : "off") : "-";
  const backendTxt = linkStatus.has_log_status ? (linkStatus.air_log_backend_ready ? "ready" : "not ready") : "-";
  const mediaTxt = linkStatus.has_log_status ? (linkStatus.air_log_media_present ? "present" : "missing") : "-";
  logsEl.textContent =
`status: ${activeTxt}
requested: ${requestedTxt}
backend_ready: ${backendTxt}
media_present: ${mediaTxt}
session_id: ${dash(linkStatus.air_log_session_id)}
bytes_written: ${fmtBytes(linkStatus.air_log_bytes_written)}
free_bytes: ${fmtBytes(linkStatus.air_log_free_bytes)}
last_command: ${logCommandText(linkStatus.air_log_last_command)}
last_change_ms: ${dash(linkStatus.air_log_last_change_ms)}`;
}

function renderActiveTab() {
  if (!isFresh(lastStateAt, STATE_STALE_MS)) {
    renderStale();
    return;
  }
  if (activeTab === "pfd") {
    renderPfdPanel();
  } else if (activeTab === "gps") {
    renderGpsPanel();
  } else if (activeTab === "att") {
    renderAttPanel();
  } else if (activeTab === "baro") {
    renderBaroPanel();
  } else if (activeTab === "link") {
    renderLinkPanel();
  } else if (activeTab === "logs") {
    renderLogsPanel();
  }
}

function renderNow() {
  updateStatus();
  renderHeader();
  if (activeTab === "link" || activeTab === "logs") {
    if (activeTab === "link") {
      renderLinkPanel();
    } else {
      linkStatus.has_log_status ? renderLogsPanel() : renderLogsStale();
    }
    lastLinkRenderAt = Date.now();
    linkDirty = false;
  }
  if (activeTab === "pfd") (latestAtt || latestGps || latestBaro) ? renderPfdPanel() : renderPfdStale();
  if (activeTab === "gps") latestGps ? renderGpsPanel() : renderGpsStale();
  if (activeTab === "att") latestAtt ? renderAttPanel() : renderAttStale();
  if (activeTab === "baro") latestBaro ? renderBaroPanel() : renderBaroStale();
  uiDirty = false;
}

function clearCtrlReconnectTimer() {
  if (!ctrlReconnectTimer) return;
  clearTimeout(ctrlReconnectTimer);
  ctrlReconnectTimer = null;
}

function clearCtrlErrorRecoveryTimer() {
  if (!ctrlErrorRecoveryTimer) return;
  clearTimeout(ctrlErrorRecoveryTimer);
  ctrlErrorRecoveryTimer = null;
}

function clearStateReconnectTimer() {
  if (!stateReconnectTimer) return;
  clearTimeout(stateReconnectTimer);
  stateReconnectTimer = null;
}

function clearStateErrorRecoveryTimer() {
  if (!stateErrorRecoveryTimer) return;
  clearTimeout(stateErrorRecoveryTimer);
  stateErrorRecoveryTimer = null;
}

function scheduleCtrlReconnect(delayMs = RECONNECT_DELAY_MS) {
  if (ctrlReconnectTimer) return;
  if (hasCtrlConnectedOnce) ctrlReconnects++;
  recordClientEvent("ctrl", "reconnect_scheduled");
  ctrlReconnectTimer = setTimeout(() => {
    ctrlReconnectTimer = null;
    connectCtrl();
  }, delayMs);
}

function scheduleStateReconnect(delayMs = STATE_RECONNECT_DELAY_MS) {
  if (stateReconnectTimer) return;
  if (hasStateConnectedOnce) stateReconnects++;
  recordClientEvent("state", "reconnect_scheduled");
  stateReconnectTimer = setTimeout(() => {
    stateReconnectTimer = null;
    connectState();
  }, delayMs);
}

function holdTelemetryFresh(ms = FILE_OP_STALE_GRACE_MS) {
  const until = Date.now() + ms;
  if (until > suppressStaleUntilMs) suppressStaleUntilMs = until;
}

function handleBinaryStateMessage(buf) {
  const wasStale = telemetryLooksStale();
  const m = parseBinaryState(buf);
  if (!m) {
    binaryParseFailCount++;
    uiDirty = true;
    return;
  }

  binaryRxCount++;
  lastStateAt = Date.now();
  const wsSeq = Number(m.w || 0);
  lastBinarySeq = wsSeq;
  lastSourceSeq = Number(m.ss || 0);
  lastSourceTus = Number(m.stu || 0);
  lastEspRxMs = Number(m.erm || 0);
  const nowMs = Date.now();
  if (!lossWinStart) lossWinStart = nowMs;
  if (nowMs - lossWinStart > LOSS_WINDOW_MS) {
    lossWinStart = nowMs;
    lossWinSeen = 0;
    lossWinLost = 0;
  }
  if (lastWsSeq && wsSeq > lastWsSeq + 1) {
    const gap = (wsSeq - lastWsSeq - 1);
    wsLoss += gap;
    lossWinLost += gap;
  }
  if (wsSeq > 0) {
    wsSeen++;
    lossWinSeen++;
    lastWsSeq = wsSeq;
  }

  latestGps = m.g || {};
  latestAtt = m.a || {};
  latestBaro = m.b || {};
  const fusion = m.f || {};
  fusionLast = {
    gain: Number(fusion.g ?? 0),
    accelerationRejection: Number(fusion.ar ?? 0),
    magneticRejection: Number(fusion.mr ?? 0),
    recoveryTriggerPeriod: Number(fusion.rp ?? 0)
  };
  const stateFlags = Number(m.flags ?? 0);
  fusionFlagsLast = {
    initialising: (stateFlags & STATE_FLAG_FUSION_INITIALISING) !== 0,
    angularRecovery: (stateFlags & STATE_FLAG_FUSION_ANGULAR_RECOVERY) !== 0,
    accelerationRecovery: (stateFlags & STATE_FLAG_FUSION_ACCELERATION_RECOVERY) !== 0,
    magneticRecovery: (stateFlags & STATE_FLAG_FUSION_MAGNETIC_RECOVERY) !== 0
  };
  const fusionMatchesDraft = fusionDraft &&
    near(fusionLast.gain, fusionDraft.gain, 0.005) &&
    near(fusionLast.accelerationRejection, fusionDraft.accelerationRejection, 0.15) &&
    near(fusionLast.magneticRejection, fusionDraft.magneticRejection, 0.15) &&
    near(fusionLast.recoveryTriggerPeriod, fusionDraft.recoveryTriggerPeriod, 0.5);

  if (fusionDirty && fusionMatchesDraft) {
    fusionDirty = false;
    fusionUiHoldUntilMs = 0;
  }
  if (fusionDirty) {
    renderFusionDraftValues();
  } else if (Date.now() >= fusionUiHoldUntilMs) {
    const gainVal = Number(fusionLast.gain ?? 0);
    const accVal = Number(fusionLast.accelerationRejection ?? 0);
    const magVal = Number(fusionLast.magneticRejection ?? 0);
    const recVal = Number(fusionLast.recoveryTriggerPeriod ?? 0);
    fusionDraft = {
      gain: gainVal,
      accelerationRejection: accVal,
      magneticRejection: magVal,
      recoveryTriggerPeriod: recVal
    };
    setFusionUiValues(gainVal, accVal, magVal, recVal);
  }
  uiDirty = true;
  linkDirty = true;
  updateStatus();
  if (forceStateRecoveryRender || wasStale) {
    forceStateRecoveryRender = false;
    renderNow();
  }
}

function handleCtrlMessage(text) {
  let m = null;
  try {
    m = JSON.parse(text);
  } catch (_err) {
    return;
  }
  lastCtrlRxAt = Date.now();
  if (m.type === "pong") {
    pingMs = Date.now() - pingSentAt;
    recordPingSample(pingMs);
    lastPongAt = Date.now();
    uiDirty = true;
    linkDirty = true;
    return;
  }
  if (m.type === "ack") {
    if (m.log_status) {
      applyLogStatus(m.log_status);
    }
    if (m.cmd === "get_fusion" && m.fusion) {
      fusionLast = {
        gain: Number(m.fusion.gain ?? FUSION_DEFAULTS.gain),
        accelerationRejection: Number(m.fusion.accelerationRejection ?? FUSION_DEFAULTS.accelerationRejection),
        magneticRejection: Number(m.fusion.magneticRejection ?? FUSION_DEFAULTS.magneticRejection),
        recoveryTriggerPeriod: Number(m.fusion.recoveryTriggerPeriod ?? FUSION_DEFAULTS.recoveryTriggerPeriod)
      };
      if (!fusionDirty && Date.now() >= fusionUiHoldUntilMs) {
        fusionDraft = { ...fusionLast };
        setFusionUiValues(
          fusionLast.gain,
          fusionLast.accelerationRejection,
          fusionLast.magneticRejection,
          fusionLast.recoveryTriggerPeriod
        );
      }
    }
    if (m.cmd === "set_fusion") {
      if (!m.ok) {
        fusionDirty = false;
      } else {
        setTimeout(requestFusionSnapshot, 150);
        setTimeout(requestFusionSnapshot, 600);
        setTimeout(requestFusionSnapshot, 1200);
      }
    }
    if (m.cmd === "start_log" || m.cmd === "stop_log" || m.cmd === "get_log_status") {
      setTimeout(requestLogStatus, 150);
      setTimeout(fetchStatus, 250);
    }
    uiDirty = true;
    linkDirty = true;
    return;
  }
  if (m.type === "config") {
    document.getElementById("sourceHz").value = m.source_rate_hz ?? m.capture_rate_hz ?? m.log_rate_hz ?? 50;
    if (Object.prototype.hasOwnProperty.call(m, "radio_lr_mode")) {
      linkStatus.radio_lr_mode = !!m.radio_lr_mode;
    }
    uiDirty = true;
    linkDirty = true;
    return;
  }
}

function connectCtrl() {
  if (wsCtrl && (wsCtrl.readyState === WebSocket.OPEN || wsCtrl.readyState === WebSocket.CONNECTING)) return;
  const proto = location.protocol === "https:" ? "wss" : "ws";
  clearCtrlReconnectTimer();
  clearCtrlErrorRecoveryTimer();
  setStatus("Connecting");
  const socket = new WebSocket(`${proto}://${location.host}/ws_ctrl`);
  wsCtrl = socket;
  const connectTimeout = setTimeout(() => {
    if (wsCtrl !== socket) return;
    if (socket.readyState === WebSocket.CONNECTING) {
      recordClientEvent("ctrl", "connect_timeout");
      try {
        socket.close();
      } catch (_err) {
      }
      if (wsCtrl === socket) wsCtrl = null;
      scheduleCtrlReconnect();
    }
  }, SOCKET_CONNECT_TIMEOUT_MS);

  socket.onopen = () => {
    if (wsCtrl !== socket) return;
    clearTimeout(connectTimeout);
    clearCtrlErrorRecoveryTimer();
    recordClientEvent("ctrl", "open");
    hasCtrlConnectedOnce = true;
    lastCtrlOpenAt = Date.now();
    lastCtrlRxAt = lastCtrlOpenAt;
    ctrlCloseCode = "-";
    ctrlCloseReason = "-";
    ctrlCloseClean = "-";
    resetPingStats();
    updateStatus();
    requestFusionSnapshot();
    requestLogStatus();
    connectState();
    uiDirty = true;
  };

  socket.onclose = (ev) => {
    clearTimeout(connectTimeout);
    clearCtrlErrorRecoveryTimer();
    if (wsCtrl !== socket) return;
    recordClientEvent("ctrl", "close", ev.code, ev.reason || "", ev.wasClean ? 1 : 0);
    wsCtrl = null;
    ctrlCloseCode = dash(ev.code);
    ctrlCloseReason = ev.reason || "-";
    ctrlCloseClean = ev.wasClean ? "1" : "0";
    uiDirty = true;
    renderNow();
    scheduleCtrlReconnect();
  };

  socket.onerror = () => {
    clearTimeout(connectTimeout);
    if (wsCtrl !== socket) return;
    recordClientEvent("ctrl", "error");
    ctrlClientErrors++;
    clearCtrlErrorRecoveryTimer();
    ctrlErrorRecoveryTimer = setTimeout(() => {
      if (wsCtrl !== socket) return;
      if (socket.readyState === WebSocket.OPEN) return;
      try {
        socket.close();
      } catch (_err) {
      }
      if (wsCtrl === socket) wsCtrl = null;
      scheduleCtrlReconnect();
    }, SOCKET_ERROR_RECOVERY_MS);
    uiDirty = true;
    if (activeTab === "link") renderNow();
  };

  socket.onmessage = (ev) => {
    if (wsCtrl !== socket || typeof ev.data !== "string") return;
    handleCtrlMessage(ev.data);
  };
}

function connectState() {
  if (wsState && (wsState.readyState === WebSocket.OPEN || wsState.readyState === WebSocket.CONNECTING)) return;
  const proto = location.protocol === "https:" ? "wss" : "ws";
  clearStateReconnectTimer();
  clearStateErrorRecoveryTimer();
  const socket = new WebSocket(`${proto}://${location.host}/ws_state`);
  wsState = socket;
  socket.binaryType = "arraybuffer";
  const connectTimeout = setTimeout(() => {
    if (wsState !== socket) return;
    if (socket.readyState === WebSocket.CONNECTING) {
      recordClientEvent("state", "connect_timeout");
      try {
        socket.close();
      } catch (_err) {
      }
      if (wsState === socket) wsState = null;
      scheduleStateReconnect();
    }
  }, SOCKET_CONNECT_TIMEOUT_MS);

  socket.onopen = () => {
    if (wsState !== socket) return;
    clearTimeout(connectTimeout);
    clearStateErrorRecoveryTimer();
    recordClientEvent("state", "open");
    hasStateConnectedOnce = true;
    lastStateSocketOpenAt = Date.now();
    lastStateAt = 0;
    forceStateRecoveryRender = true;
    lastForcedStateResetAt = 0;
    holdTelemetryFresh(STATE_STARTUP_GRACE_MS);
    stateCloseCode = "-";
    stateCloseReason = "-";
    stateCloseClean = "-";
    wsLoss = 0;
    wsSeen = 0;
    lastWsSeq = 0;
    lossWinStart = Date.now();
    lossWinSeen = 0;
    lossWinLost = 0;
    updateStatus();
    uiDirty = true;
  };

  socket.onclose = (ev) => {
    clearTimeout(connectTimeout);
    clearStateErrorRecoveryTimer();
    if (wsState !== socket) return;
    recordClientEvent("state", "close", ev.code, ev.reason || "", ev.wasClean ? 1 : 0);
    wsState = null;
    stateCloseCode = dash(ev.code);
    stateCloseReason = ev.reason || "-";
    stateCloseClean = ev.wasClean ? "1" : "0";
    forceStateRecoveryRender = true;
    uiDirty = true;
    renderNow();
    scheduleStateReconnect();
  };

  socket.onerror = () => {
    clearTimeout(connectTimeout);
    if (wsState !== socket) return;
    recordClientEvent("state", "error");
    stateClientErrors++;
    clearStateErrorRecoveryTimer();
    stateErrorRecoveryTimer = setTimeout(() => {
      if (wsState !== socket) return;
      if (socket.readyState === WebSocket.OPEN) return;
      try {
        socket.close();
      } catch (_err) {
      }
      if (wsState === socket) wsState = null;
      scheduleStateReconnect();
    }, SOCKET_ERROR_RECOVERY_MS);
    uiDirty = true;
    if (activeTab === "link") renderNow();
  };

  socket.onmessage = (ev) => {
    if (wsState !== socket || typeof ev.data === "string") return;
    handleBinaryStateMessage(ev.data);
  };
}

setInterval(() => {
  const now = Date.now();
  if (!binaryRxFpsLastMs) {
    binaryRxFpsLastMs = now;
    binaryRxCountLast = binaryRxCount;
  } else {
    const dt = now - binaryRxFpsLastMs;
    if (dt >= 1000) {
      const d = binaryRxCount - binaryRxCountLast;
      clientStateFps = dt > 0 ? (1000 * d / dt) : 0;
      binaryRxCountLast = binaryRxCount;
      binaryRxFpsLastMs = now;
      updateDataQuality();
      updateUplinkDataQuality();
      uiDirty = true;
      linkDirty = true;
    }
  }
  if (!isCtrlOpen()) return;
  pingSentAt = Date.now();
  wsCtrl.send(JSON.stringify({ type: "ping", req_id: allocCtrlReqId(), t_ms: pingSentAt }));
}, 1000);

setInterval(() => {
  if (isCtrlOpen() && isStateOpen() && lastStateAt) {
    const now = Date.now();
    const hardStale = (now - lastStateAt) > STATE_HARD_STALE_MS;
    const canForceReset = (now - lastForcedStateResetAt) > STATE_HARD_STALE_COOLDOWN_MS;
    if (hardStale && canForceReset) {
      lastForcedStateResetAt = now;
      recordClientEvent("state", "stale_reset");
      try {
        wsState.close();
      } catch (_err) {
      }
    }
  }
  const stale = !isFresh(lastStateAt, STATE_STALE_MS);
  if (activeTab === "link" || activeTab === "logs") {
    const now = Date.now();
    if ((linkDirty || stale) && (now - lastLinkRenderAt >= LINK_RENDER_PERIOD_MS)) {
      renderNow();
    }
    return;
  }
  if (uiDirty || stale) {
    renderNow();
  }
}, RENDER_PERIOD_MS);

setInterval(() => {
  fetchStatus();
}, LINK_STATUS_PERIOD_MS);

document.querySelectorAll(".tabs button").forEach((b) => {
  b.addEventListener("click", () => {
    document.querySelectorAll(".tabs button").forEach((x) => x.classList.remove("active"));
    document.querySelectorAll(".tab").forEach((x) => x.classList.remove("active"));
    b.classList.add("active");
    document.getElementById(`tab-${b.dataset.tab}`).classList.add("active");
    activeTab = b.dataset.tab;
    uiDirty = true;
    renderNow();
  });
});

window.addEventListener("resize", () => {
  uiDirty = true;
  renderNow();
});

try {
  const savedCourse = localStorage.getItem("pfd_course_deg");
  if (savedCourse !== null) {
    courseSetDeg = norm360(Number(savedCourse));
  }
} catch (_err) {
}
updateCourseSetUi();

if (pfdCanvas) {
  pfdCanvas.addEventListener("click", (ev) => {
    const rect = pfdCanvas.getBoundingClientRect();
    const cssW = pfdLastW || rect.width || 390;
    const cssH = pfdLastH || rect.height || 680;
    const x = ((ev.clientX - rect.left) / rect.width) * cssW;
    const y = ((ev.clientY - rect.top) / rect.height) * cssH;
    const hit = pfdTapTargets.find((t) => x >= t.x && x <= (t.x + t.w) && y >= t.y && y <= (t.y + t.h));
    if (!hit) {
      if (courseEditOpen) setCourseEditOpen(false);
      return;
    }
    if (hit.id === "course_toggle") {
      setCourseEditOpen(!courseEditOpen);
      return;
    }
    if (hit.id.startsWith("course_")) {
      const key = hit.id.slice(7);
      if (/^[0-9]$/.test(key)) appendCourseDigit(key);
      if (key === "clr") courseEditBuffer = "";
      if (key === "ent") commitCourseEdit();
      uiDirty = true;
      renderNow();
    }
  });
}

document.getElementById("gainSlider").addEventListener("input", () => {
  ensureFusionDraft();
  fusionDirty = true;
  fusionUiHoldUntilMs = Date.now() + 15000;
  fusionDraft.gain = sliderValue("gainSlider");
  renderFusionDraftValues();
});
document.getElementById("accelRejSlider").addEventListener("input", () => {
  ensureFusionDraft();
  fusionDirty = true;
  fusionUiHoldUntilMs = Date.now() + 15000;
  fusionDraft.accelerationRejection = sliderValue("accelRejSlider");
  renderFusionDraftValues();
});
document.getElementById("magRejSlider").addEventListener("input", () => {
  ensureFusionDraft();
  fusionDirty = true;
  fusionUiHoldUntilMs = Date.now() + 15000;
  fusionDraft.magneticRejection = sliderValue("magRejSlider");
  renderFusionDraftValues();
});
document.getElementById("recoverySlider").addEventListener("input", () => {
  ensureFusionDraft();
  fusionDirty = true;
  fusionUiHoldUntilMs = Date.now() + 15000;
  fusionDraft.recoveryTriggerPeriod = recoverySecondsToSamples(sliderValue("recoverySlider"));
  renderFusionDraftValues();
});

document.getElementById("gainSlider").addEventListener("change", () => {
  sendFusion({ gain: sliderValue("gainSlider") });
});
document.getElementById("accelRejSlider").addEventListener("change", () => {
  sendFusion({ accelerationRejection: sliderValue("accelRejSlider") });
});
document.getElementById("magRejSlider").addEventListener("change", () => {
  sendFusion({ magneticRejection: sliderValue("magRejSlider") });
});
document.getElementById("recoverySlider").addEventListener("change", () => {
  sendFusion({ recoveryTriggerPeriod: recoverySecondsToSamples(sliderValue("recoverySlider")) });
});

document.getElementById("applyRate").addEventListener("click", async () => {
  const body = {
    source_rate_hz: Math.min(400, parseInt(document.getElementById("sourceHz").value, 10)),
    radio_state_only: false,
    radio_lr_mode: !!linkStatus.radio_lr_mode
  };
  await fetch("/api/config", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  try {
    const resp = await fetch("/api/config");
    if (resp.ok) {
      const cfg = await resp.json();
      document.getElementById("sourceHz").value = cfg.source_rate_hz ?? cfg.capture_rate_hz ?? cfg.log_rate_hz ?? 50;
    }
  } catch (_err) {
  }
});

document.getElementById("refreshLogStatus").addEventListener("click", () => {
  requestLogStatus();
  fetchStatus();
});

document.getElementById("startLog").addEventListener("click", () => {
  sendLogStart();
});

document.getElementById("stopLog").addEventListener("click", () => {
  sendLogStop();
});

document.getElementById("resetAirNetwork").addEventListener("click", async () => {
  holdTelemetryFresh(10000);
  try {
    const resp = await fetch("/api/reset_air_network", { method: "POST", cache: "no-store" });
    if (!resp.ok) {
      throw new Error(`reset failed: ${resp.status}`);
    }
    setStatus("Con / resetting AIR link");
  } catch (_err) {
    setStatus("Con / AIR reset failed");
  }
});

async function downloadUrlToFile(url, filename) {
  holdTelemetryFresh();
  const resp = await fetch(url);
  if (!resp.ok) {
    throw new Error(`download failed: ${resp.status}`);
  }
  const blob = await resp.blob();
  const blobUrl = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = blobUrl;
  a.download = filename;
  a.rel = "noopener";
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(blobUrl), 1000);
}
document.getElementById("downloadDiag").addEventListener("click", async () => {
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  await downloadUrlToFile("/api/diag", `diag-${stamp}.csv`);
});
document.getElementById("downloadWsEvents").addEventListener("click", async () => {
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  await downloadUrlToFile("/api/ws_events", `ws-events-${stamp}.csv`);
});
document.getElementById("downloadClientEvents").addEventListener("click", () => {
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  const lines = ["ms,socket,event,code,reason,clean"];
  clientEventLog.forEach((e) => {
    const reason = String(e.reason ?? "").replace(/"/g, "\"\"");
    lines.push(`${e.ms},${e.socket},${e.event},${e.code},"${reason}",${e.clean}`);
  });
  const blob = new Blob([lines.join("\n") + "\n"], { type: "text/csv" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = `client-events-${stamp}.csv`;
  a.rel = "noopener";
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
});
document.getElementById("resetCounters").addEventListener("click", async () => {
  try {
    await fetch("/api/reset_counters", { method: "POST", cache: "no-store" });
  } catch (_err) {
  }
  resetLocalCounters();
  renderNow();
});

setFusionUiValues(
  FUSION_DEFAULTS.gain,
  FUSION_DEFAULTS.accelerationRejection,
  FUSION_DEFAULTS.magneticRejection,
  FUSION_DEFAULTS.recoveryTriggerPeriod
);
connectCtrl();
connectState();
setRecorderUi(false, false);
fetchStatus();
renderStale();
