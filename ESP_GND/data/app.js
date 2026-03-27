const statusEl = document.getElementById("status");
const statsEl = document.getElementById("stats");
const recEl = document.getElementById("rec");
const gpsEl = document.getElementById("gps");
const attEl = document.getElementById("att");
const baroEl = document.getElementById("baro");
const linkEl = document.getElementById("link");
const logsEl = document.getElementById("logs");
const replaySelectionSummaryEl = document.getElementById("replaySelectionSummary");
const replayTransportStateEl = document.getElementById("replayTransportState");
const replayTransportHintEl = document.getElementById("replayTransportHint");
const replayFileMetaEl = document.getElementById("replayFileMeta");
const replayFilesEl = document.getElementById("replayFiles");
const pfdCanvas = document.getElementById("pfdCanvas");
const appShellFrameEl = document.getElementById("appShellFrame");
const appShellEl = document.getElementById("appShell");
const airLoggerTextEl = document.getElementById("airLoggerText");
const fusionAngularLightEl = document.getElementById("fusionAngularLight");
const fusionMagLightEl = document.getElementById("fusionMagLight");
const rewindReplayBtn = document.getElementById("rewindReplay");
const startReplayBtn = document.getElementById("startReplay");
const pauseReplayBtn = document.getElementById("pauseReplay");
const stopReplayBtn = document.getElementById("stopReplay");
const fastForwardReplayBtn = document.getElementById("fastForwardReplay");
const refreshReplayFilesBtn = document.getElementById("refreshReplayFiles");
const deleteReplayFilesBtn = document.getElementById("deleteReplayFiles");
const exportReplayCsvBtn = document.getElementById("exportReplayCsv");
const replaySortKeyEl = document.getElementById("replaySortKey");
const replaySortDirEl = document.getElementById("replaySortDir");
const refreshStorageStatusBtn = document.getElementById("refreshStorageStatus");
const mountStorageBtn = document.getElementById("mountStorage");
const ejectStorageBtn = document.getElementById("ejectStorage");
const saveRecordPrefixBtn = document.getElementById("saveRecordPrefix");
const recordPrefixInputEl = document.getElementById("recordPrefixInput");
const storageStateTextEl = document.getElementById("storageStateText");
const storageSpaceTextEl = document.getElementById("storageSpaceText");
const storageNextFileTextEl = document.getElementById("storageNextFileText");

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
let lastStateFromWs = false;
let lastFallbackStateSeq = 0;
let lastFallbackStateAt = 0;
let lastStateGapTotal = null;
let lastStateDropTotal = null;
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
let activeTab = "pfd";
let latestGps = null;
let latestAtt = null;
let latestBaro = null;
let fusionLast = null;
let fusionFlagsLast = {
  initialising: false,
  angularRecovery: false,
  accelerationRecovery: false,
  magneticRecovery: false,
  accelerationError: false,
  accelerometerIgnored: false,
  magneticError: false,
  magnetometerIgnored: false
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
let ctrlSocketGeneration = 0;
let hasCtrlConnectedOnce = false;
let lastCtrlOpenAt = 0;
let stateCloseCode = "-";
let stateCloseReason = "-";
let stateCloseClean = "-";
let stateClientErrors = 0;
let stateReconnects = 0;
let stateReconnectTimer = null;
let stateErrorRecoveryTimer = null;
let stateSocketGeneration = 0;
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
let coursePressedKey = "";
let coursePressedUntilMs = 0;
let pfdTapTargets = [];
let nextCtrlReqId = 1;
let clientEventLog = [];
let statusFetchInFlight = false;
let replayFilesFetchTimer = null;
let replayFilesRefreshNonce = 0;
let pendingReplayFilesRefresh = false;
let selectedReplayFile = "";
let editingReplayFile = "";
let editingReplayDraft = "";
let pendingReplayEditFocus = false;
let replayFilesPanelDirty = true;
let lastReplayFileOp = null;
let controlImpactGraceUntilMs = 0;
let storageStatusFetchInFlight = false;
let lastStorageOp = null;
let replayFileSortKey = "date";
let replayFileSortDir = "desc";
let replayFilesState = {
  files: [],
  complete: false,
  refresh_inflight: false,
  revision: 0,
  total_files: 0,
  stored_files: 0,
  truncated: false,
  last_update_ms: 0
};
let storageState = {
  known: false,
  revision: 0,
  last_update_ms: 0,
  media_state: 0,
  mounted: false,
  backend_ready: false,
  media_present: false,
  busy: false,
  init_hz: 0,
  free_bytes: 0xFFFFFFFF,
  total_bytes: 0,
  file_count: 0,
  record_prefix: "air",
  next_record_name: ""
};
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
  air_log_busy: false,
  air_log_requested: false,
  air_log_backend_ready: false,
  air_log_media_present: false,
  air_log_last_command: 0,
  air_log_session_id: 0,
  air_log_bytes_written: 0,
  air_log_free_bytes: null,
  air_log_last_change_ms: null,
  storage_known: false,
  storage_revision: 0,
  storage_last_update_ms: 0,
  storage_media_state: 0,
  storage_mounted: false,
  storage_backend_ready: false,
  storage_media_present: false,
  storage_busy: false,
  storage_init_hz: 0,
  storage_free_bytes: null,
  storage_total_bytes: 0,
  storage_file_count: 0,
  storage_record_prefix: "air",
  storage_next_record_name: "",
  has_replay_status: false,
  air_replay_active: false,
  air_replay_file_open: false,
  air_replay_at_eof: false,
  air_replay_paused: false,
  air_replay_last_command: 0,
  air_replay_session_id: 0,
  air_replay_records_total: 0,
  air_replay_records_sent: 0,
  air_replay_last_error: 0,
  air_replay_last_change_ms: null,
  air_replay_current_file: "",
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
const FILE_OP_WAIT_TIMEOUT_MS = 5000;
const FILE_OP_POLL_MS = 300;
const DQI_STARTUP_SETTLE_MS = 2500;
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
const STATE_FLAG_FUSION_ACCELERATION_ERROR = 1 << 5;
const STATE_FLAG_FUSION_ACCELEROMETER_IGNORED = 1 << 6;
const STATE_FLAG_FUSION_MAGNETIC_ERROR = 1 << 7;
const STATE_FLAG_FUSION_MAGNETOMETER_IGNORED = 1 << 8;

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
  m.a.mh = dv.getFloat32(o, true); o += 4;

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
  document.getElementById("recoverySlider").value = recoverySamplesToSeconds(rec).toFixed(2);
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

function requestReplayStatus() {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  wsCtrl.send(JSON.stringify({ type: "get_replay_status", req_id: allocCtrlReqId() }));
}

function sendLogStart() {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  wsCtrl.send(JSON.stringify({ type: "start_log", req_id: allocCtrlReqId() }));
}

function sendLogStop() {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  wsCtrl.send(JSON.stringify({ type: "stop_log", req_id: allocCtrlReqId() }));
}

function sendReplayStart() {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  wsCtrl.send(JSON.stringify({ type: "start_replay", req_id: allocCtrlReqId() }));
}

function sendReplayStartFile(name = "") {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  const payload = { type: "start_replay", req_id: allocCtrlReqId() };
  if (name) payload.name = String(name);
  wsCtrl.send(JSON.stringify(payload));
}

function sendReplayStop() {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  wsCtrl.send(JSON.stringify({ type: "stop_replay", req_id: allocCtrlReqId() }));
}

function sendReplayPause() {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  wsCtrl.send(JSON.stringify({ type: "pause_replay", req_id: allocCtrlReqId() }));
}

function sendReplaySeek(deltaRecords) {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  wsCtrl.send(JSON.stringify({ type: "seek_replay", req_id: allocCtrlReqId(), delta_records: Number(deltaRecords || 0) }));
}

function replayFileDateKey(name = "") {
  const text = String(name || "");
  const match = text.match(/_(\d+)_(\d+)\.[A-Za-z0-9]+$/);
  return match ? Number(match[2] || 0) : 0;
}

function sortReplayFiles(files = []) {
  return [...files].sort((a, b) => {
    let delta = 0;
    if (replayFileSortKey === "size") {
      delta = Number(a.size || 0) - Number(b.size || 0);
    } else if (replayFileSortKey === "name") {
      delta = String(a.name || "").localeCompare(String(b.name || ""));
    } else {
      delta = replayFileDateKey(String(a.name || "")) - replayFileDateKey(String(b.name || ""));
    }
    if (delta === 0) {
      delta = String(a.name || "").localeCompare(String(b.name || ""));
    }
    return replayFileSortDir === "asc" ? delta : -delta;
  });
}

function preferredReplayFileName() {
  const selected = String(selectedReplayFile || "");
  if (selected) return selected;
  const files = sortReplayFiles(replayFilesState.files);
  return files.length ? String(files[0].name || "") : "";
}

function replayModeEngaged() {
  return !!linkStatus.air_replay_active ||
    !!linkStatus.air_replay_paused ||
    !!linkStatus.air_replay_file_open ||
    !!linkStatus.air_replay_at_eof ||
    !!linkStatus.air_replay_current_file;
}

function recorderControlState() {
  const ctrlReady = !!wsCtrl && wsCtrl.readyState === WebSocket.OPEN;
  const replayMode = replayModeEngaged();
  const logBusy = !!linkStatus.air_log_busy;
  const logBlocking = logBusy && !linkStatus.air_log_active;
  const logUnavailable = !!linkStatus.has_log_status &&
    (!linkStatus.air_log_backend_ready || !linkStatus.air_log_media_present) &&
    !linkStatus.air_log_active && !logBusy;
  return {
    ctrlReady,
    replayMode,
    logBusy,
    logBlocking,
    logUnavailable,
    disabled: !ctrlReady || replayMode || logBlocking || logUnavailable
  };
}

function syncRecorderControl() {
  if (!recEl) return;
  const state = recorderControlState();
  recEl.disabled = state.disabled;
  if (state.replayMode) {
    recEl.title = "Stop replay from the Logs page before starting a new recording";
  } else if (state.logBlocking) {
    recEl.title = "AIR recorder is still opening or closing the current file";
  } else if (state.logUnavailable) {
    recEl.title = "Recording is unavailable because the AIR SD card is missing or not ready";
  }
}

function nudgeReplayUiRefresh() {
  requestReplayStatus();
  fetchStatus();
  window.setTimeout(() => {
    requestReplayStatus();
    fetchStatus();
  }, 350);
  window.setTimeout(() => {
    requestReplayStatus();
    fetchStatus();
  }, 1000);
}

function driveReplayStopToLive() {
  setStatus("Con / return to live");
  sendReplayStop();
  nudgeReplayUiRefresh();
}

function applyReplayFilesPayload(payload = null) {
  if (!payload || !Array.isArray(payload.files)) return;
  const nextState = {
    files: sortReplayFiles(payload.files),
    complete: !!payload.complete,
    refresh_inflight: !!payload.refresh_inflight,
    revision: Number(payload.revision ?? replayFilesState.revision),
    total_files: Number(payload.total_files ?? payload.files.length),
    stored_files: Number(payload.stored_files ?? payload.files.length),
    truncated: !!payload.truncated,
    last_update_ms: Number(payload.last_update_ms ?? 0)
  };
  if (!nextState.complete && replayFilesState.complete && replayFilesState.files.length) {
    replayFilesState = {
      ...replayFilesState,
      refresh_inflight: nextState.refresh_inflight,
      total_files: nextState.total_files || replayFilesState.total_files,
      truncated: nextState.truncated,
      last_update_ms: nextState.last_update_ms,
      revision: nextState.revision
    };
  } else {
    replayFilesState = nextState;
  }
  if (selectedReplayFile && !replayFilesState.files.some((file) => file.name === selectedReplayFile)) {
    selectedReplayFile = "";
  }
  if (editingReplayFile && !replayFilesState.files.some((file) => file.name === editingReplayFile)) {
    editingReplayFile = "";
    editingReplayDraft = "";
    pendingReplayEditFocus = false;
  }
  replayFilesPanelDirty = true;
  uiDirty = true;
  linkDirty = true;
}

function storageStateText(code = storageState.media_state) {
  if (code === 0) return "No Card";
  if (code === 1) return "Unmounted";
  if (code === 2) return "Ready";
  if (code === 3) return "Error";
  return "Unknown";
}

function applyStorageStatusPayload(payload = null) {
  if (!payload) return;
  const nextState = {
    known: !!payload.known || !!payload.storage_known,
    revision: Number(payload.revision ?? payload.storage_revision ?? storageState.revision),
    last_update_ms: Number(payload.last_update_ms ?? payload.storage_last_update_ms ?? 0),
    media_state: Number(payload.media_state ?? payload.storage_media_state ?? 0),
    mounted: !!(payload.mounted ?? payload.storage_mounted),
    backend_ready: !!(payload.backend_ready ?? payload.storage_backend_ready),
    media_present: !!(payload.media_present ?? payload.storage_media_present),
    busy: !!(payload.busy ?? payload.storage_busy),
    init_hz: Number(payload.init_hz ?? payload.storage_init_hz ?? 0),
    free_bytes: Number(payload.free_bytes ?? payload.storage_free_bytes ?? 0xFFFFFFFF),
    total_bytes: Number(payload.total_bytes ?? payload.storage_total_bytes ?? 0),
    file_count: Number(payload.file_count ?? payload.storage_file_count ?? 0),
    record_prefix: String(payload.record_prefix ?? payload.storage_record_prefix ?? storageState.record_prefix ?? "air"),
    next_record_name: String(payload.next_record_name ?? payload.storage_next_record_name ?? "")
  };
  storageState = nextState;
  linkStatus.storage_known = nextState.known;
  linkStatus.storage_revision = nextState.revision;
  linkStatus.storage_last_update_ms = nextState.last_update_ms;
  linkStatus.storage_media_state = nextState.media_state;
  linkStatus.storage_mounted = nextState.mounted;
  linkStatus.storage_backend_ready = nextState.backend_ready;
  linkStatus.storage_media_present = nextState.media_present;
  linkStatus.storage_busy = nextState.busy;
  linkStatus.storage_init_hz = nextState.init_hz;
  linkStatus.storage_free_bytes = nextState.free_bytes;
  linkStatus.storage_total_bytes = nextState.total_bytes;
  linkStatus.storage_file_count = nextState.file_count;
  linkStatus.storage_record_prefix = nextState.record_prefix;
  linkStatus.storage_next_record_name = nextState.next_record_name;
  if (recordPrefixInputEl && document.activeElement !== recordPrefixInputEl) {
    recordPrefixInputEl.value = nextState.record_prefix;
  }
  uiDirty = true;
  linkDirty = true;
  replayFilesPanelDirty = true;
}

function replayFilePresent(name = "") {
  const target = String(name || "");
  return !!target && replayFilesState.files.some((file) => file.name === target);
}

function replaceReplayFileLocal(currentName = "", nextName = "") {
  const from = String(currentName || "");
  const to = String(nextName || "");
  if (!from || !to || from === to) return false;
  let changed = false;
  const nextFiles = replayFilesState.files.map((file) => {
    if (file.name !== from) return file;
    changed = true;
    return { ...file, name: to };
  });
  if (!changed) return false;
  replayFilesState = {
    ...replayFilesState,
    files: sortReplayFiles(nextFiles),
    last_update_ms: Date.now()
  };
  replayFilesPanelDirty = true;
  uiDirty = true;
  linkDirty = true;
  return true;
}

function removeReplayFileLocal(name = "") {
  const target = String(name || "");
  if (!target) return false;
  const nextFiles = replayFilesState.files.filter((file) => file.name !== target);
  if (nextFiles.length === replayFilesState.files.length) return false;
  replayFilesState = {
    ...replayFilesState,
    files: nextFiles,
    total_files: Math.max(0, Number(replayFilesState.total_files || 0) - 1),
    stored_files: Math.max(0, Number(replayFilesState.stored_files || 0) - 1),
    last_update_ms: Date.now()
  };
  replayFilesPanelDirty = true;
  uiDirty = true;
  linkDirty = true;
  return true;
}

function replayFileExtension(name = "") {
  const target = String(name || "");
  const dot = target.lastIndexOf(".");
  return dot > 0 ? target.slice(dot) : "";
}

function normalizeReplayRenameTarget(currentName = "", proposedName = "") {
  let next = String(proposedName ?? "").trim();
  if (!next) return "";
  if (next.indexOf("/") >= 0 || next.indexOf("\\") >= 0 || next.indexOf("..") >= 0) return next;
  if (!/\.[A-Za-z0-9]+$/.test(next)) {
    const ext = replayFileExtension(currentName);
    if (ext) next += ext;
  }
  return next;
}

function delayMs(ms) {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

function topPenaltySummary(detail) {
  if (!detail || !detail.penalties) return "-";
  const entries = Object.entries(detail.penalties)
    .map(([name, value]) => [name, Number(value || 0)])
    .filter(([, value]) => value > 0.05)
    .sort((a, b) => b[1] - a[1])
    .slice(0, 3)
    .map(([name, value]) => `${name}:${fmt(value, 1)}`);
  return entries.length ? entries.join(", ") : "none";
}

async function requestReplayFilesPayload({ refresh = true } = {}) {
  const url = refresh ? "/api/files?refresh=1" : "/api/files?refresh=0";
  const resp = await fetch(url, { cache: "no-store" });
  if (!resp.ok) throw new Error(`files failed: ${resp.status}`);
  return resp.json();
}

async function requestStorageStatusPayload({ refresh = true } = {}) {
  const url = refresh ? "/api/sd/status?refresh=1" : "/api/sd/status?refresh=0";
  const resp = await fetch(url, { cache: "no-store" });
  if (!resp.ok) throw new Error(`storage failed: ${resp.status}`);
  return resp.json();
}

function holdControlImpact(ms = 2500) {
  const until = Date.now() + Math.max(0, Number(ms || 0));
  if (until > controlImpactGraceUntilMs) controlImpactGraceUntilMs = until;
  holdTelemetryFresh(ms);
}

function logRefreshDeferred() {
  return !!linkStatus.air_log_active || !!linkStatus.air_log_busy;
}

function maybeFlushDeferredReplayFilesRefresh() {
  if (!pendingReplayFilesRefresh) return;
  if (logRefreshDeferred()) return;
  if (activeTab !== "logs") return;
  pendingReplayFilesRefresh = false;
  holdControlImpact(2000);
  window.setTimeout(() => {
    fetchReplayFiles({ refresh: true });
  }, 250);
}

function applyReplayFileOpResult(op, payload = null, target = "", nextTarget = "") {
  lastReplayFileOp = {
    at_ms: Date.now(),
    op: String(op || ""),
    target: String(target || ""),
    next_target: String(nextTarget || ""),
    ok: !!payload?.ok,
    tx_ok: !!payload?.tx_ok,
    ack_received: !!payload?.ack_received,
    ack_ok: !!payload?.ack_ok,
    ack_code: Number(payload?.ack_code ?? 0),
    ack_seq: Number(payload?.ack_seq ?? 0),
    files_synced: !!payload?.files_synced,
    files_revision: Number(payload?.files_revision ?? 0),
    files_complete: !!payload?.files_complete
  };
  uiDirty = true;
  linkDirty = true;
}

function replayFileOpSummary(result = null) {
  if (!result) return "-";
  return `${result.op || "-"} ok=${result.ok ? 1 : 0} tx=${result.tx_ok ? 1 : 0} ack=${result.ack_received ? 1 : 0}/${result.ack_ok ? 1 : 0} code=${result.ack_code} sync=${result.files_synced ? 1 : 0} rev=${result.files_revision}`;
}

function storageOpSummary(result = null) {
  if (!result) return "-";
  return `${result.op || "-"} ok=${result.ok ? 1 : 0} tx=${result.tx_ok ? 1 : 0} ack=${result.ack_received ? 1 : 0}/${result.ack_ok ? 1 : 0} code=${result.ack_code} sync=${result.storage_synced ? 1 : 0} rev=${result.storage_revision}`;
}

function queueReplayFilesRefresh(pollOnly = false, attempt = 0, nonce = replayFilesRefreshNonce) {
  clearTimeout(replayFilesFetchTimer);
  replayFilesFetchTimer = setTimeout(() => {
    fetchReplayFiles({ refresh: !pollOnly, attempt, nonce });
  }, attempt === 0 ? 0 : 350);
}

async function fetchReplayFiles({ refresh = true, attempt = 0, nonce = null } = {}) {
  const activeNonce = nonce ?? ++replayFilesRefreshNonce;
  if (nonce === null) replayFilesRefreshNonce = activeNonce;
  if (refresh && logRefreshDeferred()) {
    pendingReplayFilesRefresh = true;
    refresh = false;
  }
  try {
    const payload = await requestReplayFilesPayload({ refresh });
    if (activeNonce !== replayFilesRefreshNonce) return;
    applyReplayFilesPayload(payload);
    if ((payload.refresh_inflight || !payload.complete) && attempt < 10) {
      queueReplayFilesRefresh(true, attempt + 1, activeNonce);
    }
  } catch (_err) {
    if (attempt < 3) queueReplayFilesRefresh(false, attempt + 1, activeNonce);
  }
}

async function fetchStorageStatus({ refresh = true } = {}) {
  if (storageStatusFetchInFlight) return;
  storageStatusFetchInFlight = true;
  try {
    const payload = await requestStorageStatusPayload({ refresh });
    applyStorageStatusPayload(payload);
  } catch (_err) {
  } finally {
    storageStatusFetchInFlight = false;
  }
}

async function postStorageCommand(url, body = null, successLabel = "sd ok", refreshFiles = false) {
  let resp = null;
  try {
    resp = await fetch(url, {
      method: "POST",
      headers: body ? { "Content-Type": "application/json" } : undefined,
      body: body ? JSON.stringify(body) : undefined
    });
  } catch (_err) {
    setStatus(`Con / ${successLabel} tx failed`);
    return null;
  }
  let payload = null;
  try {
    payload = await resp.json();
  } catch (_err) {
  }
  if (!resp.ok || !payload) {
    setStatus(`Con / ${successLabel} failed`);
    return null;
  }
  lastStorageOp = {
    at_ms: Date.now(),
    op: String(payload.op || successLabel || ""),
    ok: !!payload.ok,
    tx_ok: !!payload.tx_ok,
    ack_received: !!payload.ack_received,
    ack_ok: !!payload.ack_ok,
    ack_code: Number(payload.ack_code ?? 0),
    storage_synced: !!payload.storage_synced,
    storage_revision: Number(payload.storage_revision ?? payload.revision ?? 0)
  };
  applyStorageStatusPayload(payload);
  if (payload.ok) {
    setStatus(`Con / ${successLabel}`);
  } else if (!payload.tx_ok) {
    setStatus(`Con / ${successLabel} tx failed`);
  } else if (!payload.ack_received) {
    setStatus(`Con / ${successLabel} ack timeout`);
  } else {
    setStatus(`Con / ${successLabel} fail c${payload.ack_code ?? 0}`);
  }
  if (refreshFiles) {
    fetchReplayFiles({ refresh: true });
  }
  uiDirty = true;
  linkDirty = true;
  return payload;
}

function selectReplayFile(name = "") {
  selectedReplayFile = String(name || "");
  replayFilesPanelDirty = true;
  uiDirty = true;
  linkDirty = true;
}

async function commitReplayRename(fromName, proposedName) {
  const current = String(fromName || "");
  if (!current) return;
  const next = normalizeReplayRenameTarget(current, proposedName);
  if (!next || next === current) return;
  const resp = await fetch("/api/rename", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ from: current, to: next })
  });
  let payload = null;
  try {
    payload = await resp.json();
  } catch (_err) {
  }
  if (!resp.ok || !payload) {
    setStatus("Con / replay rename failed");
    return;
  }
  applyReplayFileOpResult("rename", payload, current, next);
  editingReplayFile = "";
  editingReplayDraft = "";
  pendingReplayEditFocus = false;
  replayFilesPanelDirty = true;
  const renamed = !!payload.ok;
  if (renamed) {
    replaceReplayFileLocal(current, next);
    queueReplayFilesRefresh(true);
  }
  if (selectedReplayFile === current && renamed) {
    selectedReplayFile = next;
  }
  if (renamed && payload.files_synced) {
    setStatus("Con / replay rename ok");
  } else if (renamed) {
    setStatus("Con / replay rename ok / list pending");
  } else if (!payload.tx_ok) {
    setStatus("Con / replay rename tx failed");
  } else if (!payload.ack_received) {
    setStatus("Con / replay rename ack timeout");
  } else {
    setStatus(`Con / replay rename fail c${payload.ack_code ?? 0}`);
  }
  setTimeout(requestReplayStatus, 250);
}

async function renameReplayFile(fromName) {
  const current = String(fromName || "");
  if (!current) return;
  const proposed = window.prompt("Rename replay file", current);
  if (proposed === null) return;
  await commitReplayRename(current, proposed);
}

async function deleteReplayFile(name, confirmPrompt = true) {
  const target = String(name || "");
  if (!target) return;
  if (confirmPrompt && !window.confirm(`Delete ${target}?`)) return;
  const resp = await fetch("/api/delete", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ name: target })
  });
  let payload = null;
  try {
    payload = await resp.json();
  } catch (_err) {
  }
  if (!resp.ok || !payload) {
    setStatus("Con / replay delete failed");
    return;
  }
  applyReplayFileOpResult("delete", payload, target, "");
  replayFilesPanelDirty = true;
  const deleted = !!payload.ok;
  if (deleted) {
    removeReplayFileLocal(target);
    queueReplayFilesRefresh(true);
  }
  if (selectedReplayFile === target && deleted) selectedReplayFile = "";
  if (deleted && payload.files_synced) {
    setStatus("Con / replay delete ok");
  } else if (deleted) {
    setStatus("Con / replay delete ok / list pending");
  } else if (!payload.tx_ok) {
    setStatus("Con / replay delete tx failed");
  } else if (!payload.ack_received) {
    setStatus("Con / replay delete ack timeout");
  } else {
    setStatus(`Con / replay delete fail c${payload.ack_code ?? 0}`);
  }
  setTimeout(requestReplayStatus, 250);
}

function beginReplayFileEdit(name = "") {
  const target = String(name || "");
  if (!target) return;
  editingReplayFile = target;
  editingReplayDraft = target;
  pendingReplayEditFocus = true;
  replayFilesPanelDirty = true;
  uiDirty = true;
  linkDirty = true;
  renderReplayFilesPanel();
}

function cancelReplayFileEdit() {
  editingReplayFile = "";
  editingReplayDraft = "";
  pendingReplayEditFocus = false;
  replayFilesPanelDirty = true;
  uiDirty = true;
  linkDirty = true;
}

async function deleteSelectedReplayFile() {
  const target = String(selectedReplayFile || "");
  if (!target || !replayFilePresent(target)) return;
  await deleteReplayFile(target, true);
}

async function exportSelectedReplayCsv() {
  const target = String(selectedReplayFile || "");
  if (!target || !replayFilePresent(target)) return;
  await postStorageCommand("/api/csv", { name: target }, "csv export ok", false);
}

async function saveRecordPrefix() {
  const prefix = String(recordPrefixInputEl?.value || "").trim();
  await postStorageCommand("/api/logprefix", { prefix }, "prefix saved", false);
}

function renderReplayFilesPanel() {
  if (!replayFilesEl || !replayFileMetaEl) return;
  const files = sortReplayFiles(replayFilesState.files);
  const selectedName = selectedReplayFile;
  const currentName = String(linkStatus.air_replay_current_file || "");
  const mediaUnavailable = !!linkStatus.has_log_status &&
    (!linkStatus.air_log_backend_ready || !linkStatus.air_log_media_present);
  let meta = mediaUnavailable
    ? "AIR SD unavailable"
    : (replayFilesState.refresh_inflight ? "Refreshing..." : `${replayFilesState.total_files} files`);
  meta += ` | sort ${replayFileSortKey}/${replayFileSortDir}`;
  if (replayFilesState.truncated) meta += " | cache clipped";
  if (!mediaUnavailable && !replayFilesState.complete && !replayFilesState.refresh_inflight) meta += " | waiting for AIR";
  replayFileMetaEl.textContent = meta;

  replayFilesEl.innerHTML = "";
  if (mediaUnavailable || !files.length) {
    const empty = document.createElement("div");
    empty.className = "file-empty";
    empty.textContent = mediaUnavailable
      ? "Replay library unavailable while AIR SD card is missing. Reinsert the card, then refresh files or reload."
      : (replayFilesState.refresh_inflight ? "Waiting for AIR file list..." : "No replay files cached yet.");
    replayFilesEl.appendChild(empty);
    replayFilesPanelDirty = false;
    return;
  }

  files.forEach((file) => {
    const row = document.createElement("div");
    row.className = "file-row";
    if (file.name === selectedName) row.classList.add("active");
    if (file.name === currentName) row.classList.add("current");

    const main = document.createElement("div");
    main.className = "file-main";
    if (editingReplayFile === file.name) {
      const editEl = document.createElement("input");
      editEl.className = "file-edit";
      editEl.type = "text";
      editEl.value = editingReplayDraft || file.name;
      editEl.addEventListener("click", (ev) => ev.stopPropagation());
      editEl.addEventListener("input", () => {
        editingReplayDraft = editEl.value;
      });
      editEl.addEventListener("keydown", (ev) => {
        if (ev.key === "Enter") {
          ev.preventDefault();
          void commitReplayRename(file.name, editEl.value);
        } else if (ev.key === "Escape") {
          ev.preventDefault();
          cancelReplayFileEdit();
          renderReplayFilesPanel();
        }
      });
      editEl.addEventListener("blur", () => {
        if (editingReplayFile === file.name) {
          void commitReplayRename(file.name, editEl.value);
        }
      });
      main.appendChild(editEl);
      if (pendingReplayEditFocus) {
        requestAnimationFrame(() => {
          editEl.focus();
          editEl.select();
        });
        pendingReplayEditFocus = false;
      }
    } else {
      const nameEl = document.createElement("button");
      nameEl.className = "file-name";
      nameEl.textContent = file.name;
      main.appendChild(nameEl);
      nameEl.addEventListener("click", () => selectReplayFile(file.name));
      nameEl.addEventListener("dblclick", (ev) => {
        ev.preventDefault();
        beginReplayFileEdit(file.name);
      });
    }

    const sizeEl = document.createElement("div");
    sizeEl.className = "file-size";
    sizeEl.textContent = fmtBytes(file.size);
 
    row.appendChild(main);
    row.appendChild(sizeEl);

    replayFilesEl.appendChild(row);
  });
  replayFilesPanelDirty = false;
}

function renderStoragePanel() {
  if (!storageStateTextEl || !storageSpaceTextEl || !storageNextFileTextEl) return;
  const stateText = storageState.known
    ? `${storageStateText(storageState.media_state)}${storageState.busy ? " / busy" : ""}`
    : "Waiting for AIR SD status...";
  storageStateTextEl.textContent = stateText;
  storageSpaceTextEl.textContent = storageState.total_bytes
    ? `${fmtBytes(storageState.free_bytes)} free / ${fmtBytes(storageState.total_bytes)} total`
    : fmtBytes(storageState.free_bytes);
  storageNextFileTextEl.textContent = storageState.next_record_name || "-";
  storageNextFileTextEl.classList.toggle("empty", !storageState.next_record_name);

  const canMount = !storageState.busy && (!storageState.mounted || storageState.media_state !== 2);
  const canEject = !storageState.busy && storageState.mounted;
  if (mountStorageBtn) mountStorageBtn.disabled = !canMount;
  if (ejectStorageBtn) ejectStorageBtn.disabled = !canEject;
  if (saveRecordPrefixBtn) saveRecordPrefixBtn.disabled = storageState.busy;
  if (refreshStorageStatusBtn) refreshStorageStatusBtn.disabled = storageStatusFetchInFlight;
}

function syncReplayTransportUi() {
  if (!replaySelectionSummaryEl || !replayTransportStateEl || !replayTransportHintEl) return;
  const ctrlReady = !!wsCtrl && wsCtrl.readyState === WebSocket.OPEN;
  const replayKnown = !!linkStatus.has_replay_status;
  const replayActive = !!linkStatus.air_replay_active;
  const replayPaused = !!linkStatus.air_replay_paused;
  const atEof = !!linkStatus.air_replay_at_eof;
  const fileOpen = !!linkStatus.air_replay_file_open;
  const recordsSent = Number(linkStatus.air_replay_records_sent ?? 0);
  const recordsTotal = Number(linkStatus.air_replay_records_total ?? 0);
  const currentFile = String(linkStatus.air_replay_current_file || "");
  const selectedFile = String(selectedReplayFile || "");
  const recorderState = recorderControlState();

  replayTransportStateEl.classList.remove("idle", "ready", "active", "complete");
  if (replayActive) {
    replayTransportStateEl.classList.add("active");
    replayTransportStateEl.textContent = "PLAYING";
  } else if (replayPaused) {
    replayTransportStateEl.classList.add("ready");
    replayTransportStateEl.textContent = "PAUSED";
  } else if (replayKnown && atEof) {
    replayTransportStateEl.classList.add("complete");
    replayTransportStateEl.textContent = "DONE";
  } else if (replayKnown && currentFile) {
    replayTransportStateEl.classList.add("ready");
    replayTransportStateEl.textContent = "STOPPED";
  } else {
    replayTransportStateEl.classList.add("idle");
    replayTransportStateEl.textContent = "IDLE";
  }

  if (selectedFile) {
    replaySelectionSummaryEl.textContent = selectedFile;
    replaySelectionSummaryEl.classList.remove("empty");
  } else {
    replaySelectionSummaryEl.textContent = "No file selected";
    replaySelectionSummaryEl.classList.add("empty");
  }

  if (replayActive) {
    replayTransportHintEl.textContent = `Replay progress ${recordsSent}/${recordsTotal || "-"}. << and >> jump by 100 records.`;
  } else if (replayPaused) {
    replayTransportHintEl.textContent = `Paused at ${recordsSent}/${recordsTotal || "-"}. Use play to resume or stop to end.`;
  } else if (currentFile) {
    replayTransportHintEl.textContent = `Last replay source ${currentFile}.`;
  } else {
    replayTransportHintEl.textContent = "";
  }

  const replayMode = replayModeEngaged();
  if (startReplayBtn) startReplayBtn.disabled = !ctrlReady || replayActive;
  if (stopReplayBtn) stopReplayBtn.disabled = !ctrlReady || !replayMode;
  if (pauseReplayBtn) pauseReplayBtn.disabled = !ctrlReady || !replayActive;
  if (rewindReplayBtn) rewindReplayBtn.disabled = !ctrlReady || !replayMode;
  if (fastForwardReplayBtn) fastForwardReplayBtn.disabled = !ctrlReady || !replayMode;
  syncRecorderControl();
  if (refreshReplayFilesBtn) refreshReplayFilesBtn.disabled = logRefreshDeferred();
  if (deleteReplayFilesBtn) deleteReplayFilesBtn.disabled = logRefreshDeferred() || !selectedFile || !replayFilePresent(selectedFile);
  if (exportReplayCsvBtn) exportReplayCsvBtn.disabled = logRefreshDeferred() || !selectedFile || !replayFilePresent(selectedFile);
  if (replaySortKeyEl) replaySortKeyEl.value = replayFileSortKey;
  if (replaySortDirEl) replaySortDirEl.value = replayFileSortDir;
  if (!editingReplayFile || replayFilesPanelDirty) {
    renderReplayFilesPanel();
  }
  renderStoragePanel();
}

function toggleHeaderRecording() {
  if (!wsCtrl || wsCtrl.readyState !== WebSocket.OPEN) return;
  const replayMode = replayModeEngaged();
  if (replayMode) {
    setStatus("Con / stop replay in Logs first");
    return;
  }
  if (linkStatus.air_log_busy && !linkStatus.air_log_active) {
    setStatus("Con / recorder busy");
    return;
  }
  holdControlImpact(3000);
  if (linkStatus.air_log_active) {
    pendingReplayFilesRefresh = true;
    setStatus("Con / stop recording");
    sendLogStop();
    requestLogStatus();
    fetchStatus();
    window.setTimeout(() => {
      requestLogStatus();
      fetchStatus();
    }, 500);
    return;
  }
  setStatus("Con / starting new recording");
  sendLogStart();
  requestLogStatus();
  fetchStatus();
  window.setTimeout(() => {
    requestLogStatus();
    fetchStatus();
  }, 500);
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
  const startupGraceActive = Date.now() < suppressStaleUntilMs;
  const controlGraceActive = Date.now() < controlImpactGraceUntilMs;
  const startupSettleActive = startupGraceActive || controlGraceActive ||
    (lastStateSocketOpenAt > 0 && (Date.now() - lastStateSocketOpenAt) < DQI_STARTUP_SETTLE_MS);
  const staleMs = lastStateAt ? (Date.now() - lastStateAt) : Infinity;
  const stateFresh = isFresh(lastStateAt, STATE_STALE_MS);
  const radioFresh = !!linkStatus.air_link_fresh;
  const rateNow = stateFresh ? clientStateFps : 0;
  const haveClientRate = stateFresh && Number.isFinite(clientStateFps) && clientStateFps > 0;
  const rateRatio = (controlGraceActive || (startupSettleActive && !haveClientRate)) ? 1 : clamp(rateNow / TARGET_STATE_FPS, 0, 1);
  const radioRateRatio = (controlGraceActive || (startupSettleActive && (radioStateFps === null || radioStateFps <= 0 || !radioFresh)))
    ? 1
    : ((radioStateFps === null || !radioFresh) ? 0 : clamp(radioStateFps / TARGET_STATE_FPS, 0, 1));
  const radioRtt = Number(linkStatus.radio_rtt_ms ?? 0);
  const radioRttPenalty = !radioFresh ? 0 : clamp((radioRtt - 80) / 220, 0, 1);
  const stalePenalty = startupSettleActive ? 0 : (!stateFresh ? 1 : clamp((staleMs - 300) / 1200, 0, 1));
  const gapTotal = Number(linkStatus.state_seq_gap ?? 0);
  const dropTotal = Number(linkStatus.drop ?? 0);
  const gapDelta = 0;
  const dropDelta = 0;
  const deliveredPackets = lastRadioStatePackets !== null
    ? Math.max(0, Number(linkStatus.state_packets ?? 0) - Number(lastRadioStatePackets ?? 0))
    : 0;
  // `state_seq_gap` tracks skipped source states from the faster upstream mirror stream,
  // not just missing radio deliveries, so treating it as radio loss permanently depresses DQI.
  const radioLossRatio = 0;
  const wsLossRatio = controlGraceActive ? 0 : clamp(Number(lossWinLost) / Math.max(1, Number(lossWinSeen) + Number(lossWinLost)), 0, 1);

  let score = 100;
  const penaltyRadioFresh = !radioFresh ? 30 : 0;
  const penaltyStateFresh = startupSettleActive ? 0 : (!stateFresh ? 30 : 0);
  const penaltyRate = (1 - rateRatio) * 20;
  const penaltyRadioRate = (1 - Math.max(rateRatio, radioRateRatio)) * 10;
  const penaltyRadioLoss = radioLossRatio * 35;
  const penaltyWsLoss = startupSettleActive ? 0 : (wsLossRatio * 15);
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
  dqiSmoothed = (dqiSmoothed === null || startupSettleActive) ? score : Math.round((dqiSmoothed * 0.7) + (score * 0.3));
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
  if (command === 109) return "replay_start";
  if (command === 110) return "replay_stop";
  if (command === 111) return "replay_status";
  return command ? String(command) : "-";
}

function setFusionLight(el, on) {
  if (!el) return;
  el.classList.toggle("on", !!on);
  el.classList.toggle("off", !on);
}

function setFusionPriorityLight(el, orangeOn, redOn) {
  if (!el) return;
  const on = !!(orangeOn || redOn);
  setFusionLight(el, on);
  el.classList.toggle("orange", !!on && !redOn);
  el.classList.toggle("red", !!redOn);
}

function renderFusionLights() {
  setFusionPriorityLight(fusionAngularLightEl, fusionFlagsLast.accelerationError, fusionFlagsLast.accelerometerIgnored);
  setFusionPriorityLight(fusionMagLightEl, fusionFlagsLast.magneticError, fusionFlagsLast.magnetometerIgnored);
}

function applyLogStatus(logStatus = null) {
  if (!logStatus) return;
  const wasActive = !!linkStatus.air_log_active;
  const wasBusy = !!linkStatus.air_log_busy;
  linkStatus.has_log_status = true;
  linkStatus.air_log_active = !!logStatus.active;
  linkStatus.air_log_busy = !!logStatus.busy;
  linkStatus.air_log_requested = !!logStatus.requested;
  linkStatus.air_log_backend_ready = !!logStatus.backend_ready;
  linkStatus.air_log_media_present = !!logStatus.media_present;
  linkStatus.air_log_last_command = Number(logStatus.last_command ?? 0);
  linkStatus.air_log_session_id = Number(logStatus.session_id ?? 0);
  linkStatus.air_log_bytes_written = Number(logStatus.bytes_written ?? 0);
  linkStatus.air_log_free_bytes = Number(logStatus.free_bytes ?? 0xFFFFFFFF);
  linkStatus.air_log_last_change_ms = Number(logStatus.last_change_ms ?? 0);
  if (!linkStatus.air_log_backend_ready || !linkStatus.air_log_media_present) {
    replayFilesState = {
      files: [],
      complete: true,
      refresh_inflight: false,
      truncated: false,
      total_files: 0,
      stored_files: 0,
      revision: replayFilesState.revision,
      last_update_ms: Date.now()
    };
    selectedReplayFile = "";
    replayFilesPanelDirty = true;
  }
  setRecorderUi(
    linkStatus.has_log_status ? !!linkStatus.air_log_active : !!linkStatus.air_recorder_on,
    !!linkStatus.has_log_status || !!linkStatus.has_link_meta
  );
  syncRecorderControl();
  if ((wasActive || wasBusy) && !linkStatus.air_log_active && !linkStatus.air_log_busy) {
    maybeFlushDeferredReplayFilesRefresh();
  }
}

function applyReplayStatus(replayStatus = null) {
  if (!replayStatus) return;
  linkStatus.has_replay_status = true;
  linkStatus.air_replay_active = !!replayStatus.active;
  linkStatus.air_replay_file_open = !!replayStatus.file_open;
  linkStatus.air_replay_at_eof = !!replayStatus.at_eof;
  linkStatus.air_replay_paused = !!replayStatus.paused;
  linkStatus.air_replay_last_command = Number(replayStatus.last_command ?? 0);
  linkStatus.air_replay_session_id = Number(replayStatus.session_id ?? 0);
  linkStatus.air_replay_records_total = Number(replayStatus.records_total ?? 0);
  linkStatus.air_replay_records_sent = Number(replayStatus.records_sent ?? 0);
  linkStatus.air_replay_last_error = Number(replayStatus.last_error ?? 0);
  linkStatus.air_replay_last_change_ms = Number(replayStatus.last_change_ms ?? 0);
  linkStatus.air_replay_current_file = String(replayStatus.current_file || "");
}

function applyPolledState(state = null, seq = 0, tUs = 0, espRxMs = 0) {
  if (!state) return;
  const now = Date.now();
  if (seq !== 0 && seq !== lastFallbackStateSeq) {
    if (lastFallbackStateAt > 0) {
      const dtMs = now - lastFallbackStateAt;
      if (dtMs > 0) clientStateFps = 1000 / dtMs;
    }
    lastFallbackStateSeq = seq;
    lastFallbackStateAt = now;
  }

  lastStateAt = now;
  lastStateFromWs = false;
  lastSourceSeq = Number(seq ?? 0);
  lastSourceTus = Number(tUs ?? 0);
  lastEspRxMs = Number(espRxMs ?? 0);

  latestAtt = {
    r: Number(state.roll_deg ?? 0),
    p: Number(state.pitch_deg ?? 0),
    y: Number(state.yaw_deg ?? 0),
    mh: Number(state.mag_heading_deg ?? 0)
  };
  latestGps = {
    it: Number(state.iTOW_ms ?? 0),
    fx: Number(state.fixType ?? 0),
    sv: Number(state.numSV ?? 0),
    la: Number(state.lat_1e7 ?? 0) * 1e-7,
    lo: Number(state.lon_1e7 ?? 0) * 1e-7,
    hm: Number(state.hMSL_mm ?? 0) / 1000.0,
    gs: Number(state.gSpeed_mms ?? 0) / 1000.0,
    cr: Number(state.headMot_1e5deg ?? 0) / 100000.0,
    ha: Number(state.hAcc_mm ?? 0) / 1000.0,
    sa: Number(state.sAcc_mms ?? 0) / 1000.0,
    pe: Number(state.gps_parse_errors ?? 0),
    lgm: Number(state.last_gps_ms ?? 0)
  };
  latestBaro = {
    t: Number(state.baro_temp_c ?? 0),
    p: Number(state.baro_press_hpa ?? 0),
    a: Number(state.baro_alt_m ?? 0),
    v: Number(state.baro_vsi_mps ?? 0),
    lbm: Number(state.last_baro_ms ?? 0)
  };
  fusionLast = {
    gain: Number(state.fusion_gain ?? 0),
    accelerationRejection: Number(state.fusion_accel_rej ?? 0),
    magneticRejection: Number(state.fusion_mag_rej ?? 0),
    recoveryTriggerPeriod: Number(state.fusion_recovery_period ?? 0)
  };
  const stateFlags = Number(state.flags ?? 0);
  fusionFlagsLast = {
    initialising: (stateFlags & STATE_FLAG_FUSION_INITIALISING) !== 0,
    angularRecovery: (stateFlags & STATE_FLAG_FUSION_ANGULAR_RECOVERY) !== 0,
    accelerationRecovery: (stateFlags & STATE_FLAG_FUSION_ACCELERATION_RECOVERY) !== 0,
    magneticRecovery: (stateFlags & STATE_FLAG_FUSION_MAGNETIC_RECOVERY) !== 0,
    accelerationError: (stateFlags & STATE_FLAG_FUSION_ACCELERATION_ERROR) !== 0,
    accelerometerIgnored: (stateFlags & STATE_FLAG_FUSION_ACCELEROMETER_IGNORED) !== 0,
    magneticError: (stateFlags & STATE_FLAG_FUSION_MAGNETIC_ERROR) !== 0,
    magnetometerIgnored: (stateFlags & STATE_FLAG_FUSION_MAGNETOMETER_IGNORED) !== 0
  };
}

function setRecorderUi(enabled, known = true) {
  recEl.classList.remove("ready", "recording", "unknown");
  if (!known) {
    recEl.classList.add("unknown");
    recEl.innerHTML = "&#9210;";
    recEl.title = "Recorder status is not available yet";
    airLoggerTextEl.textContent = "Waiting for AIR recorder status...";
    syncRecorderControl();
    return;
  }
  if (enabled) {
    recEl.classList.add("recording");
    recEl.innerHTML = "&#9209;";
    recEl.title = "Stop recording";
    airLoggerTextEl.textContent = "AIR recorder is on";
    syncRecorderControl();
    return;
  }
  if (linkStatus.has_log_status && (!linkStatus.air_log_backend_ready || !linkStatus.air_log_media_present)) {
    recEl.classList.add("unknown");
    recEl.innerHTML = "&#9210;";
    recEl.title = "Recording is unavailable because the AIR SD card is missing or not ready";
    airLoggerTextEl.textContent = "AIR recorder unavailable";
    syncRecorderControl();
    return;
  }
  if (linkStatus.air_log_busy) {
    recEl.classList.add("ready");
    recEl.innerHTML = "&#9209;";
    recEl.title = "AIR recorder is still opening or closing the current file";
    airLoggerTextEl.textContent = "AIR recorder is finalizing the current file";
    syncRecorderControl();
    return;
  }
  if (replayModeEngaged()) {
    recEl.classList.add("ready");
    recEl.innerHTML = "&#9210;";
    recEl.title = "Stop replay from the Logs page before starting a new recording";
    airLoggerTextEl.textContent = "AIR replay is active";
    syncRecorderControl();
    return;
  }
  recEl.classList.add("ready");
  recEl.innerHTML = "&#9210;";
  recEl.title = "Start a new recording";
  airLoggerTextEl.textContent = "AIR live telemetry";
  syncRecorderControl();
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

function resetUplinkTelemetryStats() {
  uplinkDqiScore = null;
  uplinkDqiSmoothed = null;
  uplinkDqiDetail = null;
  lastUplinkPingTimeout = 0;
}

function resetStateTelemetryStats() {
  wsLoss = 0;
  wsSeen = 0;
  lastWsSeq = 0;
  lastStateAt = 0;
  binaryRxCount = 0;
  binaryParseFailCount = 0;
  clientStateFps = 0;
  binaryRxCountLast = 0;
  binaryRxFpsLastMs = 0;
  radioStateFps = null;
  lastRadioStatePackets = null;
  lastRadioStatusAt = 0;
  lastBinarySeq = 0;
  lastSourceSeq = 0;
  lastSourceTus = 0;
  lastEspRxMs = 0;
  lastStateFromWs = false;
  lastFallbackStateSeq = 0;
  lastFallbackStateAt = 0;
  lastStateGapTotal = null;
  lastStateDropTotal = null;
  dqiScore = null;
  dqiSmoothed = null;
  dqiDetail = null;
  lossWinStart = 0;
  lossWinSeen = 0;
  lossWinLost = 0;
  forceStateRecoveryRender = true;
  uiDirty = true;
  linkDirty = true;
}

function resetLocalCounters() {
  resetPingStats();
  resetStateTelemetryStats();
  resetUplinkTelemetryStats();
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

function restartStateSocket(reason = "state_restart") {
  holdTelemetryFresh(STATE_STARTUP_GRACE_MS);
  resetStateTelemetryStats();
  recordClientEvent("state", reason);
  if (wsState && (wsState.readyState === WebSocket.OPEN || wsState.readyState === WebSocket.CONNECTING)) {
    try {
      wsState.close();
      return;
    } catch (_err) {
    }
  }
  connectState();
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
    setStatus("Connected / AIR link stale");
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
    const wasAirFresh = !!linkStatus.air_link_fresh;
    const wasReplayActive = !!linkStatus.air_replay_active;
    const priorStatePackets = Number(linkStatus.state_packets ?? 0);
    linkStatus = {
      ...linkStatus,
      ...data
    };
    const now = Date.now();
    const statePackets = Number(linkStatus.state_packets ?? 0);
    if ((!wasAirFresh && !!linkStatus.air_link_fresh) ||
        (priorStatePackets > 0 && statePackets < priorStatePackets) ||
        (wasReplayActive && !linkStatus.air_replay_active)) {
      resetUplinkTelemetryStats();
    }
    if (wasReplayActive !== !!linkStatus.air_replay_active) {
      restartStateSocket(wasReplayActive ? "replay_stop_reset" : "replay_start_reset");
    }
    if (lastRadioStatusAt > 0 && lastRadioStatePackets !== null && statePackets >= lastRadioStatePackets) {
      const dtMs = now - lastRadioStatusAt;
      if (dtMs > 0) {
        radioStateFps = (statePackets - lastRadioStatePackets) * 1000 / dtMs;
      }
    } else if (lastRadioStatePackets !== null && statePackets < lastRadioStatePackets) {
      radioStateFps = null;
    }
    const wsFresh = lastStateFromWs && isFresh(lastStateAt, STATE_STALE_MS);
    if (data.has_state && data.state && !wsFresh) {
      applyPolledState(data.state, Number(data.seq ?? 0), Number(data.t_us ?? 0), Number(data.last_rx_ms ?? 0));
    }
    updateDataQuality();
    updateUplinkDataQuality();
    lastRadioStatePackets = statePackets;
    lastRadioStatusAt = now;
    if (data.has_log_status) {
      applyLogStatus({
        active: data.air_log_active,
        busy: data.air_log_busy,
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
    if (data.has_replay_status) {
      applyReplayStatus({
        active: data.air_replay_active,
        file_open: data.air_replay_file_open,
        at_eof: data.air_replay_at_eof,
        paused: data.air_replay_paused,
        last_command: data.air_replay_last_command,
        session_id: data.air_replay_session_id,
        records_total: data.air_replay_records_total,
        records_sent: data.air_replay_records_sent,
        last_error: data.air_replay_last_error,
        last_change_ms: data.air_replay_last_change_ms,
        current_file: data.air_replay_current_file
      });
    }
    applyStorageStatusPayload(data);
    setRecorderUi(
      linkStatus.has_log_status ? !!linkStatus.air_log_active : !!linkStatus.air_recorder_on,
      !!linkStatus.has_log_status || !!linkStatus.has_link_meta
    );
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
mag.heading: - deg

fusion.gain: -
fusion.accelRej: - deg
fusion.magRej: - deg
fusion.recovery: - s / - smp`;
  fusionFlagsLast = {
    initialising: false,
    angularRecovery: false,
    accelerationRecovery: false,
    magneticRecovery: false,
    accelerationError: false,
    accelerometerIgnored: false,
    magneticError: false,
    magnetometerIgnored: false
  };
  renderFusionLights();
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

function updateUiScale() {
  if (!appShellEl || !appShellFrameEl) return;
  const baseW = 430;
  appShellEl.style.transform = "scale(1)";
  const naturalH = appShellEl.scrollHeight;
  const viewportW = window.innerWidth;
  const viewportH = window.innerHeight;
  const scaleW = viewportW / baseW;
  const scaleH = viewportH / Math.max(1, naturalH);
  const scale = Math.max(0.55, Math.min(scaleW, scaleH));
  appShellEl.style.transform = `scale(${scale})`;
  appShellFrameEl.style.height = `${Math.ceil(naturalH * scale)}px`;
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

function drawRecoveryLed(ctx, cx, cy, radius, color) {
  ctx.save();
  ctx.fillStyle = color;
  ctx.shadowColor = color;
  ctx.shadowBlur = radius * 3;
  ctx.beginPath();
  ctx.arc(cx, cy, radius, 0, Math.PI * 2);
  ctx.fill();
  ctx.shadowBlur = 0;
  ctx.strokeStyle = "#fed7aa";
  ctx.lineWidth = 1;
  ctx.stroke();
  ctx.restore();
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

function markCourseKeyPressed(key) {
  coursePressedKey = String(key || "");
  coursePressedUntilMs = Date.now() + 220;
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
  // Flight data is now aircraft-standard body axes. For the fixed aircraft
  // symbol, the horizon rotates opposite the aircraft bank.
  const displayRollDeg = -rollDeg;
  const headingDeg = ((yawDeg % 360) + 360) % 360;
  const altM = Number(baro.a ?? gps.hm ?? 0);
  const vsiMps = Number(baro.v ?? 0);
  const gsMps = Number(gps.gs ?? 0);
  const fixType = Number(gps.fx ?? 0);
  const sats = Number(gps.sv ?? 0);
  const courseDeg = courseSetDeg;
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
  ctx.rotate((displayRollDeg * Math.PI) / 180);

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
  if (fusionFlagsLast.accelerometerIgnored || fusionFlagsLast.accelerationError) {
    drawRecoveryLed(
      ctx,
      w - 18,
      16,
      5,
      fusionFlagsLast.accelerometerIgnored ? "#ef4444" : "#eab308"
    );
  }

  const hsiAreaX = 12;
  const hsiAreaY = tapeBottom + 18;
  const hsiAreaW = w - 24;
  const hsiAreaH = h - hsiAreaY - 12;
  const hsiCx = hsiAreaX + (hsiAreaW * 0.5);
  const hsiCy = hsiAreaY + (hsiAreaH * 0.5);
  const hsiR = Math.max(75, Math.min(hsiAreaW * 0.34, hsiAreaH * 0.42));
  pfdTapTargets = [];
  ctx.strokeStyle = "#334155";
  ctx.lineWidth = 2;
  drawRoundedRect(ctx, hsiAreaX, hsiAreaY, hsiAreaW, hsiAreaH, 16);
  ctx.stroke();
  if (fusionFlagsLast.magnetometerIgnored || fusionFlagsLast.magneticError) {
    drawRecoveryLed(
      ctx,
      hsiAreaX + hsiAreaW - 18,
      hsiAreaY + 16,
      5,
      fusionFlagsLast.magnetometerIgnored ? "#ef4444" : "#eab308"
    );
  }
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
      const pressed = coursePressedKey === btn.label && Date.now() < coursePressedUntilMs;
      drawRoundedRect(ctx, btn.x, btn.y, btnW, btnH, 8);
      ctx.fillStyle = pressed ? "#1d4ed8" : "#08111d";
      ctx.fill();
      ctx.strokeStyle = pressed ? "#93c5fd" : "#475569";
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
  syncReplayTransportUi();
}

function renderStale() {
  renderPfdStale();
  renderGpsStale();
  renderAttStale();
  renderBaroStale();
  renderLinkStale();
  renderLogsStale();
  statsEl.innerHTML =
    `<span class="stats-line"><span>fps: -</span><span class="dqi-badge dqi-unknown">DQI:--</span></span>`;
}

function renderHeader() {
  const stateFpsTxt = isFresh(lastStateAt, STATE_STALE_MS) ? fmt(clientStateFps, 1) : "-";
  const linkDqi = uplinkDqiSmoothed;
  const uiDqi = dqiSmoothed;
  const combinedDqi = (linkDqi === null) ? uiDqi : ((uiDqi === null) ? linkDqi : Math.min(linkDqi, uiDqi));
  const combinedDqiTxt = combinedDqi === null ? "--" : String(combinedDqi);
  const dqiBadge = `<span class="${dqiBadgeClass(combinedDqi)}">DQI:${combinedDqiTxt}</span>`;
  statsEl.innerHTML =
    `<span class="stats-line"><span>fps: ${stateFpsTxt}</span>${dqiBadge}</span>`;
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

function renderPositionPanel() {
  latestGps ? renderGpsPanel() : renderGpsStale();
  latestBaro ? renderBaroPanel() : renderBaroStale();
}

function renderAttPanel() {
  const att = latestAtt || {};
  const fusion = fusionLast || {};
  attEl.textContent =
`roll: ${fmt(att.r, 2)} deg
pitch: ${fmt(att.p, 2)} deg
yaw: ${fmt(att.y, 2)} deg
mag.heading: ${fmt(att.mh, 2)} deg

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
  const airLinkTxt = linkStatus.air_link_fresh ? "up" : "stale";
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
  const dqiTopPenaltyTxt = topPenaltySummary(dqiDetail);
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
  const uplinkTopPenaltyTxt = topPenaltySummary(uplinkDqiDetail);
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
dqi_top_penalties: ${dqiTopPenaltyTxt}
uplink_miss_streak: ${uplinkMissStreakTxt}
uplink_fresh: ${uplinkFreshTxt}
uplink_air_age_ms: ${uplinkAirAgeTxt}
uplink_rtt_ms: ${uplinkRttTxt}
uplink_penalty_fresh: ${uplinkPenaltyFreshTxt}
uplink_penalty_age: ${uplinkPenaltyAgeTxt}
uplink_penalty_miss: ${uplinkPenaltyMissTxt}
uplink_penalty_timeout: ${uplinkPenaltyTimeoutTxt}
uplink_penalty_rtt: ${uplinkPenaltyRttTxt}
uplink_top_penalties: ${uplinkTopPenaltyTxt}
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
  const recorderNoticeTxt = linkStatus.has_log_status
    ? ((!linkStatus.air_log_backend_ready || !linkStatus.air_log_media_present)
      ? "recording unavailable - AIR SD card missing or not ready; reinsert card then refresh files or reload"
      : (linkStatus.air_log_busy
        ? "recorder busy - AIR is opening or closing a file"
        : "none"))
    : "-";
  const replayActiveTxt = linkStatus.has_replay_status ? (linkStatus.air_replay_active ? "on" : "off") : "-";
  const replayPausedTxt = linkStatus.has_replay_status ? (linkStatus.air_replay_paused ? "yes" : "no") : "-";
  const replayFileTxt = linkStatus.has_replay_status ? (linkStatus.air_replay_file_open ? "open" : "closed") : "-";
  const replayEofTxt = linkStatus.has_replay_status ? (linkStatus.air_replay_at_eof ? "yes" : "no") : "-";
  const fileOpTxt = replayFileOpSummary(lastReplayFileOp);
  const fileOpAgeTxt = lastReplayFileOp ? String(Date.now() - lastReplayFileOp.at_ms) : "-";
  const storageOpTxt = storageOpSummary(lastStorageOp);
  const storageOpAgeTxt = lastStorageOp ? String(Date.now() - lastStorageOp.at_ms) : "-";
  const uiDqiReasonTxt = topPenaltySummary(dqiDetail);
  logsEl.textContent =
`notice: ${recorderNoticeTxt}
status: ${activeTxt}
requested: ${requestedTxt}
backend_ready: ${backendTxt}
media_present: ${mediaTxt}
session_id: ${dash(linkStatus.air_log_session_id)}
bytes_written: ${fmtBytes(linkStatus.air_log_bytes_written)}
free_bytes: ${fmtBytes(linkStatus.air_log_free_bytes)}
last_command: ${logCommandText(linkStatus.air_log_last_command)}
last_change_ms: ${dash(linkStatus.air_log_last_change_ms)}

replay_active: ${replayActiveTxt}
replay_paused: ${replayPausedTxt}
replay_file: ${replayFileTxt}
replay_eof: ${replayEofTxt}
replay_current_file: ${dash(linkStatus.air_replay_current_file)}
replay_session_id: ${dash(linkStatus.air_replay_session_id)}
replay_records_sent: ${dash(linkStatus.air_replay_records_sent)}
replay_records_total: ${dash(linkStatus.air_replay_records_total)}
replay_last_command: ${logCommandText(linkStatus.air_replay_last_command)}
replay_last_error: ${dash(linkStatus.air_replay_last_error)}
replay_last_change_ms: ${dash(linkStatus.air_replay_last_change_ms)}

storage_known: ${storageState.known ? "yes" : "no"}
storage_state: ${storageStateText(storageState.media_state)}
storage_mounted: ${storageState.mounted ? "yes" : "no"}
storage_busy: ${storageState.busy ? "yes" : "no"}
storage_prefix: ${dash(storageState.record_prefix)}
storage_next_record: ${dash(storageState.next_record_name)}
storage_free_bytes: ${fmtBytes(storageState.free_bytes)}
storage_total_bytes: ${fmtBytes(storageState.total_bytes)}
storage_file_count: ${dash(storageState.file_count)}

last_file_op: ${fileOpTxt}
last_file_op_age_ms: ${fileOpAgeTxt}
last_storage_op: ${storageOpTxt}
last_storage_op_age_ms: ${storageOpAgeTxt}
ui_dqi: ${dash(dqiSmoothed)}
ui_dqi_reason: ${uiDqiReasonTxt}`;
  syncReplayTransportUi();
}

function renderActiveTab() {
  if (!isFresh(lastStateAt, STATE_STALE_MS)) {
    renderStale();
    return;
  }
  if (activeTab === "pfd") {
    renderPfdPanel();
  } else if (activeTab === "position") {
    renderPositionPanel();
  } else if (activeTab === "att") {
    renderAttPanel();
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
  if (activeTab === "position") renderPositionPanel();
  if (activeTab === "att") latestAtt ? renderAttPanel() : renderAttStale();
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

function scheduleCtrlReconnect(expectedGeneration, delayMs = RECONNECT_DELAY_MS) {
  if (ctrlReconnectTimer) return;
  if (hasCtrlConnectedOnce) ctrlReconnects++;
  recordClientEvent("ctrl", "reconnect_scheduled");
  ctrlReconnectTimer = setTimeout(() => {
    ctrlReconnectTimer = null;
    if (expectedGeneration !== ctrlSocketGeneration) return;
    connectCtrl();
  }, delayMs);
}

function scheduleStateReconnect(expectedGeneration, delayMs = STATE_RECONNECT_DELAY_MS) {
  if (stateReconnectTimer) return;
  if (hasStateConnectedOnce) stateReconnects++;
  recordClientEvent("state", "reconnect_scheduled");
  stateReconnectTimer = setTimeout(() => {
    stateReconnectTimer = null;
    if (expectedGeneration !== stateSocketGeneration) return;
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
  const nowMs = Date.now();
  const prevStateAt = lastStateAt;
  lastStateAt = nowMs;
  lastStateFromWs = true;
  const wsSeq = Number(m.w || 0);
  lastBinarySeq = wsSeq;
  lastSourceSeq = Number(m.ss || 0);
  lastSourceTus = Number(m.stu || 0);
  lastEspRxMs = Number(m.erm || 0);
  if (!lossWinStart) lossWinStart = nowMs;
  if (nowMs - lossWinStart > LOSS_WINDOW_MS) {
    lossWinStart = nowMs;
    lossWinSeen = 0;
    lossWinLost = 0;
  }
  const staleGap = prevStateAt > 0 && (nowMs - prevStateAt) > STATE_HARD_STALE_MS;
  if (lastWsSeq && wsSeq > 0) {
    if (wsSeq <= lastWsSeq) {
      lossWinStart = nowMs;
      lossWinSeen = 0;
      lossWinLost = 0;
    } else if (wsSeq > lastWsSeq + 1) {
      const gap = (wsSeq - lastWsSeq - 1);
      const treatAsReset = staleGap || wasStale || (nowMs - lastStateSocketOpenAt) < 2000 ||
        nowMs < controlImpactGraceUntilMs || gap > (TARGET_STATE_FPS * 3);
      if (!treatAsReset) {
        wsLoss += gap;
        lossWinLost += gap;
      } else {
        lossWinStart = nowMs;
        lossWinSeen = 0;
        lossWinLost = 0;
      }
    }
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
    magneticRecovery: (stateFlags & STATE_FLAG_FUSION_MAGNETIC_RECOVERY) !== 0,
    accelerationError: (stateFlags & STATE_FLAG_FUSION_ACCELERATION_ERROR) !== 0,
    accelerometerIgnored: (stateFlags & STATE_FLAG_FUSION_ACCELEROMETER_IGNORED) !== 0,
    magneticError: (stateFlags & STATE_FLAG_FUSION_MAGNETIC_ERROR) !== 0,
    magnetometerIgnored: (stateFlags & STATE_FLAG_FUSION_MAGNETOMETER_IGNORED) !== 0
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
    if (m.replay_status) {
      applyReplayStatus(m.replay_status);
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
    if (m.cmd === "start_replay" || m.cmd === "pause_replay" || m.cmd === "stop_replay" ||
        m.cmd === "seek_replay" || m.cmd === "get_replay_status") {
      setTimeout(requestReplayStatus, 150);
      setTimeout(fetchStatus, 250);
      if (m.cmd === "start_replay" || m.cmd === "stop_replay") {
        setTimeout(() => { fetchReplayFiles({ refresh: true }); }, 350);
      }
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
  ctrlSocketGeneration++;
  const generation = ctrlSocketGeneration;
  setStatus("Connecting");
  const socket = new WebSocket(`${proto}://${location.host}/ws_ctrl`);
  wsCtrl = socket;
  const connectTimeout = setTimeout(() => {
    if (wsCtrl !== socket || generation !== ctrlSocketGeneration) return;
    if (socket.readyState === WebSocket.CONNECTING) {
      recordClientEvent("ctrl", "connect_timeout");
      try {
        socket.close();
      } catch (_err) {
      }
      if (wsCtrl === socket) wsCtrl = null;
      scheduleCtrlReconnect(generation);
    }
  }, SOCKET_CONNECT_TIMEOUT_MS);

  socket.onopen = () => {
    if (wsCtrl !== socket || generation !== ctrlSocketGeneration) return;
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
    requestReplayStatus();
    connectState();
    uiDirty = true;
  };

  socket.onclose = (ev) => {
    clearTimeout(connectTimeout);
    clearCtrlErrorRecoveryTimer();
    if (wsCtrl === socket) wsCtrl = null;
    if (generation !== ctrlSocketGeneration) return;
    recordClientEvent("ctrl", "close", ev.code, ev.reason || "", ev.wasClean ? 1 : 0);
    ctrlCloseCode = dash(ev.code);
    ctrlCloseReason = ev.reason || "-";
    ctrlCloseClean = ev.wasClean ? "1" : "0";
    uiDirty = true;
    renderNow();
    scheduleCtrlReconnect(generation);
  };

  socket.onerror = () => {
    clearTimeout(connectTimeout);
    if (wsCtrl !== socket || generation !== ctrlSocketGeneration) return;
    recordClientEvent("ctrl", "error");
    ctrlClientErrors++;
    clearCtrlErrorRecoveryTimer();
    ctrlErrorRecoveryTimer = setTimeout(() => {
      if (wsCtrl !== socket || generation !== ctrlSocketGeneration) return;
      if (socket.readyState === WebSocket.OPEN) return;
      try {
        socket.close();
      } catch (_err) {
      }
      if (wsCtrl === socket) wsCtrl = null;
      scheduleCtrlReconnect(generation);
    }, SOCKET_ERROR_RECOVERY_MS);
    uiDirty = true;
    if (activeTab === "link") renderNow();
  };

  socket.onmessage = (ev) => {
    if (wsCtrl !== socket || generation !== ctrlSocketGeneration || typeof ev.data !== "string") return;
    handleCtrlMessage(ev.data);
  };
}

function connectState() {
  if (wsState && (wsState.readyState === WebSocket.OPEN || wsState.readyState === WebSocket.CONNECTING)) return;
  const proto = location.protocol === "https:" ? "wss" : "ws";
  clearStateReconnectTimer();
  clearStateErrorRecoveryTimer();
  stateSocketGeneration++;
  const generation = stateSocketGeneration;
  const socket = new WebSocket(`${proto}://${location.host}/ws_state`);
  wsState = socket;
  socket.binaryType = "arraybuffer";
  const connectTimeout = setTimeout(() => {
    if (wsState !== socket || generation !== stateSocketGeneration) return;
    if (socket.readyState === WebSocket.CONNECTING) {
      recordClientEvent("state", "connect_timeout");
      try {
        socket.close();
      } catch (_err) {
      }
      if (wsState === socket) wsState = null;
      scheduleStateReconnect(generation);
    }
  }, SOCKET_CONNECT_TIMEOUT_MS);

  socket.onopen = () => {
    if (wsState !== socket || generation !== stateSocketGeneration) return;
    clearTimeout(connectTimeout);
    clearStateErrorRecoveryTimer();
    recordClientEvent("state", "open");
    hasStateConnectedOnce = true;
    lastStateSocketOpenAt = Date.now();
    resetStateTelemetryStats();
    lastForcedStateResetAt = 0;
    holdTelemetryFresh(STATE_STARTUP_GRACE_MS);
    stateCloseCode = "-";
    stateCloseReason = "-";
    stateCloseClean = "-";
    lossWinStart = Date.now();
    updateStatus();
    uiDirty = true;
  };

  socket.onclose = (ev) => {
    clearTimeout(connectTimeout);
    clearStateErrorRecoveryTimer();
    if (wsState === socket) wsState = null;
    if (generation !== stateSocketGeneration) return;
    recordClientEvent("state", "close", ev.code, ev.reason || "", ev.wasClean ? 1 : 0);
    stateCloseCode = dash(ev.code);
    stateCloseReason = ev.reason || "-";
    stateCloseClean = ev.wasClean ? "1" : "0";
    forceStateRecoveryRender = true;
    uiDirty = true;
    renderNow();
    scheduleStateReconnect(generation);
  };

  socket.onerror = () => {
    clearTimeout(connectTimeout);
    if (wsState !== socket || generation !== stateSocketGeneration) return;
    recordClientEvent("state", "error");
    stateClientErrors++;
    clearStateErrorRecoveryTimer();
    stateErrorRecoveryTimer = setTimeout(() => {
      if (wsState !== socket || generation !== stateSocketGeneration) return;
      if (socket.readyState === WebSocket.OPEN) return;
      try {
        socket.close();
      } catch (_err) {
      }
      if (wsState === socket) wsState = null;
      scheduleStateReconnect(generation);
    }, SOCKET_ERROR_RECOVERY_MS);
    uiDirty = true;
    if (activeTab === "link") renderNow();
  };

  socket.onmessage = (ev) => {
    if (wsState !== socket || generation !== stateSocketGeneration || typeof ev.data === "string") return;
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
  if (document.hidden) return;
  fetchStatus();
}, LINK_STATUS_PERIOD_MS);

setInterval(() => {
  if (document.hidden) return;
  if (activeTab !== "link" && activeTab !== "logs") return;
  fetchStorageStatus({ refresh: false });
}, 15000);

document.querySelectorAll(".tabs button").forEach((b) => {
  b.addEventListener("click", () => {
    document.querySelectorAll(".tabs button").forEach((x) => x.classList.remove("active"));
    document.querySelectorAll(".tab").forEach((x) => x.classList.remove("active"));
    b.classList.add("active");
    document.getElementById(`tab-${b.dataset.tab}`).classList.add("active");
    activeTab = b.dataset.tab;
    uiDirty = true;
    if (activeTab === "logs") maybeFlushDeferredReplayFilesRefresh();
    renderNow();
  });
});

window.addEventListener("resize", () => {
  updateUiScale();
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
      if (/^[0-9]$/.test(key)) {
        appendCourseDigit(key);
        markCourseKeyPressed(key);
      }
      if (key === "clr") {
        courseEditBuffer = "";
        markCourseKeyPressed("C");
      }
      if (key === "ent") {
        commitCourseEdit();
        markCourseKeyPressed("OK");
      }
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

startReplayBtn.addEventListener("click", () => {
  sendReplayStartFile(preferredReplayFileName());
  nudgeReplayUiRefresh();
});

stopReplayBtn.addEventListener("click", () => {
  driveReplayStopToLive();
});

pauseReplayBtn.addEventListener("click", () => {
  sendReplayPause();
  nudgeReplayUiRefresh();
});

rewindReplayBtn.addEventListener("click", () => {
  sendReplaySeek(-100);
});

fastForwardReplayBtn.addEventListener("click", () => {
  sendReplaySeek(100);
});

recEl.addEventListener("click", () => {
  toggleHeaderRecording();
});

refreshReplayFilesBtn.addEventListener("click", () => {
  holdControlImpact(2000);
  fetchReplayFiles({ refresh: true });
});

deleteReplayFilesBtn.addEventListener("click", () => {
  void deleteSelectedReplayFile();
});

if (exportReplayCsvBtn) {
  exportReplayCsvBtn.addEventListener("click", () => {
    void exportSelectedReplayCsv();
  });
}

if (replaySortKeyEl) {
  replaySortKeyEl.addEventListener("change", () => {
    replayFileSortKey = String(replaySortKeyEl.value || "date");
    replayFilesPanelDirty = true;
    uiDirty = true;
    linkDirty = true;
    renderNow();
  });
}

if (replaySortDirEl) {
  replaySortDirEl.addEventListener("change", () => {
    replayFileSortDir = String(replaySortDirEl.value || "desc");
    replayFilesPanelDirty = true;
    uiDirty = true;
    linkDirty = true;
    renderNow();
  });
}

if (refreshStorageStatusBtn) {
  refreshStorageStatusBtn.addEventListener("click", () => {
    void fetchStorageStatus({ refresh: true });
  });
}

if (mountStorageBtn) {
  mountStorageBtn.addEventListener("click", () => {
    void postStorageCommand("/api/sd/mount", null, "sd mount ok", true);
  });
}

if (ejectStorageBtn) {
  ejectStorageBtn.addEventListener("click", () => {
    void postStorageCommand("/api/sd/eject", null, "sd eject ok", true);
  });
}

if (saveRecordPrefixBtn) {
  saveRecordPrefixBtn.addEventListener("click", () => {
    void saveRecordPrefix();
  });
}

if (recordPrefixInputEl) {
  recordPrefixInputEl.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") {
      ev.preventDefault();
      void saveRecordPrefix();
    }
  });
}

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
if (recordPrefixInputEl) {
  recordPrefixInputEl.value = storageState.record_prefix;
}
updateUiScale();
connectCtrl();
connectState();
setRecorderUi(false, false);
fetchStatus();
fetchStorageStatus({ refresh: false });
renderStale();
