const statusEl = document.getElementById("status");
const statsEl = document.getElementById("stats");
const recEl = document.getElementById("rec");
const gpsEl = document.getElementById("gps");
const attEl = document.getElementById("att");
const baroEl = document.getElementById("baro");
const linkEl = document.getElementById("link");
const logsEl = document.getElementById("logs");
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
const STATE_STALE_MS = 1500;
const PONG_FRESH_MS = 3000;
const LOSS_WINDOW_MS = 10000;
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
let nextCtrlReqId = 1;
let clientEventLog = [];
let statusFetchInFlight = false;
let linkStatus = {
  transport: "ESP-NOW",
  radio_state_only: false,
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
  setStatus(telemetryLooksStale() ? "Connected / stale telemetry" : "Connected");
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
  renderGpsStale();
  renderAttStale();
  renderBaroStale();
  renderLinkStale();
  renderLogsStale();
  statsEl.textContent = `state_fps: - | seq: - | ctrl: - | state: - | ping: - | radio: - | avg: -`;
}

function renderHeader() {
  const pingTxt = isPongFresh() ? pingMs : null;
  const pingAvgTxt = isPongFresh() ? pingAvgMs : null;
  const radioRttTxt = linkStatus.air_link_fresh ? linkStatus.radio_rtt_ms : null;
  const stateFpsTxt = isFresh(lastStateAt, STATE_STALE_MS) ? fmt(clientStateFps, 1) : "-";
  const seqTxt = lastSourceSeq > 0 ? String(lastSourceSeq) : "-";
  const ctrlTxt = isCtrlOpen() ? "open" : "closed";
  const stateTxt = isStateOpen() ? "open" : "closed";
  statsEl.textContent =
    `state_fps: ${stateFpsTxt} | seq: ${seqTxt} | ctrl: ${ctrlTxt} | state: ${stateTxt} | ping: ${fmtMs(pingTxt)} | radio: ${fmtMs(radioRttTxt)} | avg: ${fmtMs(pingAvgTxt)}`;
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
  const sourceSeqTxt = lastSourceSeq > 0 ? String(lastSourceSeq) : "-";
  const sourceTusTxt = lastSourceTus > 0 ? String(lastSourceTus) : "-";
  const espRxMsTxt = lastEspRxMs > 0 ? String(lastEspRxMs) : "-";
  const wsLossTxt = String(wsLoss);
  const parseFailTxt = String(binaryParseFailCount);
  const ctrlReconnectTxt = String(ctrlReconnects);
  const stateReconnectTxt = String(stateReconnects);
  const ctrlLastCloseTxt = `${ctrlCloseCode}/${ctrlCloseClean}`;
  const stateLastCloseTxt = `${stateCloseCode}/${stateCloseClean}`;
  const airLinkTxt = linkStatus.air_link_fresh ? "up" : "waiting";
  const airRadioTxt = linkStatus.air_radio_ready ? "ready" : "starting";
  const airPeerTxt = linkStatus.air_peer_known ? "known" : "discovering";
  const radioModeTxt = linkStatus.radio_state_only ? "state-only stress" : "normal unified";
  const airRssiTxt = fmtRssi(linkStatus.air_rssi_valid, linkStatus.air_rssi_dbm);
  const airScanAgeTxt = dash(linkStatus.air_scan_age_ms);
  const airLinkAgeTxt = dash(linkStatus.air_link_age_ms);
  const airRecorderTxt = !linkStatus.has_link_meta ? "unknown" : (linkStatus.air_recorder_on ? "on" : "off");
  linkEl.textContent =
`transport: ${dash(linkStatus.transport)}
radio_mode: ${radioModeTxt}
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
ctrl_ws: ${ctrlSocketTxt}
state_ws: ${stateSocketTxt}
state_fps: ${stateFpsTxt}
state_age_ms: ${stateAgeTxt}
source_seq: ${sourceSeqTxt}
source_t_us: ${sourceTusTxt}
esp_rx_ms: ${espRxMsTxt}
link_rx: ${dash(linkStatus.link_rx)}
state_rx: ${dash(linkStatus.state_packets)}
state_gap: ${dash(linkStatus.state_seq_gap)}
state_rewind: ${dash(linkStatus.state_seq_rewind)}
frames_ok: ${dash(linkStatus.ok)}
len_err: ${dash(linkStatus.len_err)}
unknown_msg: ${dash(linkStatus.unknown_msg)}
drop: ${dash(linkStatus.drop)}
ws_loss: ${wsLossTxt}
binary_parse_fail: ${parseFailTxt}
ctrl_reconnects: ${ctrlReconnectTxt}
state_reconnects: ${stateReconnectTxt}
ctrl_last_close: ${ctrlLastCloseTxt}
state_last_close: ${stateLastCloseTxt}
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
  if (activeTab === "gps") {
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
    radio_state_only: false
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
    setStatus("Connected / resetting AIR link");
  } catch (_err) {
    setStatus("Connected / AIR reset failed");
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
