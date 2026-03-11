const statusEl = document.getElementById("status");
const statsEl = document.getElementById("stats");
const recEl = document.getElementById("rec");
const gpsEl = document.getElementById("gps");
const attEl = document.getElementById("att");
const fusionStatusEl = document.getElementById("fusionStatus");
const baroEl = document.getElementById("baro");
const linkEl = document.getElementById("link");

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
const PING_AVG_WINDOW = 30;
const CLIENT_EVENT_CAPACITY = 256;
const FUSION_SAMPLE_RATE_HZ = 400;
const FUSION_DEFAULTS = {
  gain: 0.06,
  accelerationRejection: 20.0,
  magneticRejection: 60.0,
  recoveryTriggerPeriod: 1200
};

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
    gain: Number(fusionDraft.gain || 0),
    accelerationRejection: Number(fusionDraft.accelerationRejection || 0),
    magneticRejection: Number(fusionDraft.magneticRejection || 0),
    recoveryTriggerPeriod: Math.round(Number(fusionDraft.recoveryTriggerPeriod || 0))
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

function dash(v) {
  return (v === null || v === undefined || Number.isNaN(v)) ? "-" : String(v);
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
  if (waitingForTelemetry()) {
    setStatus("Connected / waiting for AIR");
    return;
  }
  setStatus(telemetryLooksStale() ? "Connected / stale telemetry" : "Connected");
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
  fusionStatusEl.textContent =
`applied gain: -
applied accel reject: - deg
applied mag reject: - deg
applied recovery: - s / - smp`;
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
`ctrl_socket: -
state_socket: -
state_fps: -
state_age_ms: -
source_seq: -
source_t_us: -
esp_rx_ms: -
ws_loss: -
binary_parse_fail: -
ctrl_reconnects: -
state_reconnects: -
ctrl_last_close: -
state_last_close: -
ping: - ms
ping_avg: - ms`;
}

function renderStale() {
  renderGpsStale();
  renderAttStale();
  renderBaroStale();
  renderLinkStale();
  statsEl.textContent = `state_fps: - | seq: - | ctrl: - | state: - | ping: - ms | avg: - ms`;
}

function renderHeader() {
  const pingTxt = isPongFresh() ? String(pingMs ?? "-") : "-";
  const pingAvgTxt = isPongFresh() ? String(pingAvgMs ?? "-") : "-";
  const stateFpsTxt = isFresh(lastStateAt, STATE_STALE_MS) ? fmt(clientStateFps, 1) : "-";
  const seqTxt = lastSourceSeq > 0 ? String(lastSourceSeq) : "-";
  const ctrlTxt = isCtrlOpen() ? "open" : "closed";
  const stateTxt = isStateOpen() ? "open" : "closed";
  statsEl.textContent = `state_fps: ${stateFpsTxt} | seq: ${seqTxt} | ctrl: ${ctrlTxt} | state: ${stateTxt} | ping: ${pingTxt} ms | avg: ${pingAvgTxt} ms`;
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
  fusionStatusEl.textContent =
`applied gain: ${fmt(fusion.gain, 3)}
applied accel reject: ${fmt(fusion.accelerationRejection, 1)} deg
applied mag reject: ${fmt(fusion.magneticRejection, 1)} deg
applied recovery: ${fusionRecoveryText(fusion.recoveryTriggerPeriod)}`;
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
  const pingTxt = isPongFresh() ? String(pingMs ?? "-") : "-";
  const pingAvgTxt = isPongFresh() ? String(pingAvgMs ?? "-") : "-";
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
  linkEl.textContent =
`ctrl_socket: ${ctrlSocketTxt}
state_socket: ${stateSocketTxt}
state_fps: ${stateFpsTxt}
state_age_ms: ${stateAgeTxt}
source_seq: ${sourceSeqTxt}
source_t_us: ${sourceTusTxt}
esp_rx_ms: ${espRxMsTxt}
ws_loss: ${wsLossTxt}
binary_parse_fail: ${parseFailTxt}
ctrl_reconnects: ${ctrlReconnectTxt}
state_reconnects: ${stateReconnectTxt}
ctrl_last_close: ${ctrlLastCloseTxt}
state_last_close: ${stateLastCloseTxt}
ping: ${pingTxt} ms
ping_avg: ${pingAvgTxt} ms`;
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
  }
}

function renderNow() {
  updateStatus();
  renderHeader();
  if (activeTab === "link") {
    renderLinkPanel();
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
    if (m.cmd === "set_fusion" && !m.ok) {
      fusionDirty = false;
    }
    uiDirty = true;
    linkDirty = true;
    return;
  }
  if (m.type === "config") {
    document.getElementById("sourceHz").value = m.source_rate_hz ?? 50;
    document.getElementById("uiHz").value = m.ui_rate_hz ?? 20;
    recEl.textContent = "REC ON";
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
    socket.send(JSON.stringify({ type: "get_fusion", req_id: allocCtrlReqId() }));
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
  if (activeTab === "link") {
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
    source_rate_hz: parseInt(document.getElementById("sourceHz").value, 10),
    ui_rate_hz: Math.min(20, parseInt(document.getElementById("uiHz").value, 10))
  };
  await fetch("/api/config", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  try {
    const resp = await fetch("/api/config");
    if (resp.ok) {
      const cfg = await resp.json();
      document.getElementById("sourceHz").value = cfg.source_rate_hz ?? 50;
      document.getElementById("uiHz").value = cfg.ui_rate_hz ?? 20;
    }
  } catch (_err) {
  }
  recEl.textContent = "REC ON";
});

document.getElementById("resetAirNetwork").addEventListener("click", async () => {
  holdTelemetryFresh(10000);
  try {
    const resp = await fetch("/api/reset_air_network", { method: "POST", cache: "no-store" });
    if (!resp.ok) {
      throw new Error(`reset failed: ${resp.status}`);
    }
    setStatus("Connected / resetting AIR network");
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
recEl.textContent = "REC ON";
renderStale();
