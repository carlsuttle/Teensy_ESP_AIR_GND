const socketBadgeEl = document.getElementById('socketBadge');
const freshBadgeEl = document.getElementById('freshBadge');
const sourceBadgeEl = document.getElementById('sourceBadge');
const schemaBadgeEl = document.getElementById('schemaBadge');
const fixBadgeEl = document.getElementById('fixBadge');
const ageMetricEl = document.getElementById('ageMetric');
const hzMetricEl = document.getElementById('hzMetric');
const rttMetricEl = document.getElementById('rttMetric');
const recordMetricEl = document.getElementById('recordMetric');
const performanceFieldsEl = document.getElementById('performanceFields');
const pfdSummaryEl = document.getElementById('pfdSummary');
const modeSummaryEl = document.getElementById('modeSummary');
const hsiSummaryEl = document.getElementById('hsiSummary');
const statusFieldsEl = document.getElementById('statusFields');
const gpsFieldsEl = document.getElementById('gpsFields');
const stateFieldsEl = document.getElementById('stateFields');
const sensorFieldsEl = document.getElementById('sensorFields');
const fileFieldsEl = document.getElementById('fileFields');
const filesJsonEl = document.getElementById('filesJson');
const eventLogEl = document.getElementById('eventLog');
const rawJsonEl = document.getElementById('rawJson');
const pendingMetricEl = document.getElementById('pendingMetric');
const selectedFileMetricEl = document.getElementById('selectedFileMetric');
const reconnectBtn = document.getElementById('reconnectBtn');
const disconnectBtn = document.getElementById('disconnectBtn');
const clearBtn = document.getElementById('clearBtn');
const recordStartBtn = document.getElementById('recordStartBtn');
const recordStopBtn = document.getElementById('recordStopBtn');
const filesRefreshBtn = document.getElementById('filesRefreshBtn');
const fileDeleteBtn = document.getElementById('fileDeleteBtn');
const fileExportBtn = document.getElementById('fileExportBtn');
const fileSelect = document.getElementById('fileSelect');
const fusionGainInput = document.getElementById('fusionGainInput');
const fusionAccelInput = document.getElementById('fusionAccelInput');
const fusionMagInput = document.getElementById('fusionMagInput');
const fusionRecoveryInput = document.getElementById('fusionRecoveryInput');
const fusionApplyBtn = document.getElementById('fusionApplyBtn');
const pfdCanvas = document.getElementById('pfdCanvas');
const hsiCanvas = document.getElementById('hsiCanvas');
const pfdFreshEl = document.getElementById('pfdFresh');
const pfdSourceEl = document.getElementById('pfdSource');
const hsiFreshEl = document.getElementById('hsiFresh');
const hsiSourceEl = document.getElementById('hsiSource');

let ws = null;
let reconnectTimer = null;
let reconnectGeneration = 0;
let manualDisconnect = false;
let lastMessageMs = 0;
let latestSnapshot = null;
let latestFiles = null;
let latestStorage = null;
let reqId = 1;
let selectedFileName = '';
let fusionInitialized = false;
let pendingControl = null;
let activeTab = 'pfd';

const controlStatus = {
  recording: { tx: '-', ack: '-', detail: '-', reqId: '-' },
  file: { tx: '-', ack: '-', detail: '-', reqId: '-' },
  fusion: { tx: '-', ack: '-', detail: '-', reqId: '-' }
};

const eventLines = [];

function appendEvent(text) {
  const stamp = new Date().toLocaleTimeString();
  eventLines.unshift(`[${stamp}] ${text}`);
  while (eventLines.length > 40) eventLines.pop();
  eventLogEl.textContent = eventLines.join('\n');
}

function fmtNumber(value, digits = 3) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) return '-';
  return Number(value).toFixed(digits);
}

function fmtInt(value) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) return '-';
  return String(Math.round(Number(value)));
}

function fmtText(value) {
  if (value === null || value === undefined || value === '') return '-';
  return String(value);
}

function boolText(value) {
  return value ? 'true' : 'false';
}

function fieldHtml(key, value) {
  return `<div class="field"><div class="field-key">${key}</div><div class="field-value">${value}</div></div>`;
}

function renderFields(target, entries) {
  target.innerHTML = entries.map(([key, value]) => fieldHtml(key, value)).join('');
}

function setPill(el, text, mode = '') {
  el.textContent = text;
  el.className = `pill${mode ? ` ${mode}` : ''}`;
}

function gpsDateTimeText(snapshot) {
  if (!snapshot || !snapshot.gps_calendar_valid) return 'Unavailable';
  const yyyy = fmtInt(snapshot.gps_year);
  const mm = String(snapshot.gps_month).padStart(2, '0');
  const dd = String(snapshot.gps_day).padStart(2, '0');
  const hh = String(snapshot.gps_hour).padStart(2, '0');
  const mi = String(snapshot.gps_min).padStart(2, '0');
  const ss = String(snapshot.gps_sec).padStart(2, '0');
  return `${yyyy}-${mm}-${dd} ${hh}:${mi}:${ss} UTC`;
}

function sourceMode(snapshot) {
  if (!snapshot) return 'idle';
  if (snapshot.replay_active || snapshot.replay_paused || snapshot.replay_file_open) return 'replay';
  if (snapshot.has_state && snapshot.fresh) return 'live';
  return 'idle';
}

function selectedFileDisplay() {
  return selectedFileName || '-';
}

function setControlsEnabled(enabled) {
  const hasSocket = enabled && ws && ws.readyState === WebSocket.OPEN;
  recordStartBtn.disabled = !hasSocket;
  recordStopBtn.disabled = !hasSocket;
  filesRefreshBtn.disabled = !hasSocket;
  fileDeleteBtn.disabled = !hasSocket || !selectedFileName;
  fileExportBtn.disabled = !hasSocket || !selectedFileName;
  fusionApplyBtn.disabled = !hasSocket;
}

function updateControlStatus(category, txText, ackText, detailText, reqIdValue) {
  const bucket = controlStatus[category];
  if (!bucket) return;
  if (txText !== undefined) bucket.tx = txText;
  if (ackText !== undefined) bucket.ack = ackText;
  if (detailText !== undefined) bucket.detail = detailText;
  if (reqIdValue !== undefined) bucket.reqId = reqIdValue;
}

function recordPending(category, action, reqIdValue) {
  pendingControl = `${category}/${action} req=${reqIdValue}`;
  updateControlStatus(category, `tx ${action}`, 'pending', 'waiting for backend', reqIdValue);
  pendingMetricEl.textContent = pendingControl;
  renderFilesState();
  if (latestSnapshot) renderSnapshot(latestSnapshot, null);
}

function applyAck(msg) {
  const op = String(msg.op || '');
  let category = '';
  if (op.includes('record')) category = 'recording';
  else if (op.includes('file')) category = 'file';
  else if (op.includes('fusion')) category = 'fusion';
  if (!category) return;
  pendingControl = null;
  updateControlStatus(
    category,
    undefined,
    msg.ok ? 'ok' : 'rejected',
    `${fmtText(msg.detail)} (code ${fmtInt(msg.code)})`,
    fmtInt(msg.req_id)
  );
  pendingMetricEl.textContent = '-';
}

function nextReqId() {
  return reqId++;
}

function sendControl(message) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    appendEvent('control blocked: websocket not open');
    return;
  }
  ws.send(JSON.stringify(message));
  appendEvent(`tx ${message.category}/${message.action} req=${message.req_id}`);
}

function drawPfd(snapshot) {
  const ctx = pfdCanvas.getContext('2d');
  const w = pfdCanvas.width;
  const h = pfdCanvas.height;
  ctx.clearRect(0, 0, w, h);

  const roll = Number(snapshot?.roll_deg || 0);
  const pitch = Number(snapshot?.pitch_deg || 0);
  const altitude = Number(snapshot?.baro_alt_m || 0);
  const speed = Number(snapshot?.gSpeed_mms || 0) / 1000;
  const vsi = Number(snapshot?.baro_vsi_mps || 0);
  const fresh = !!snapshot?.fresh;

  ctx.save();
  ctx.translate(w / 2, h / 2);
  ctx.rotate(-roll * Math.PI / 180);
  const pitchScale = 5;
  const horizonY = pitch * pitchScale;

  ctx.fillStyle = 'rgba(77, 140, 211, 0.95)';
  ctx.fillRect(-w, -h * 2 + horizonY, w * 2, h * 2);
  ctx.fillStyle = 'rgba(120, 80, 50, 0.98)';
  ctx.fillRect(-w, horizonY, w * 2, h * 2);

  ctx.strokeStyle = 'rgba(255,255,255,0.25)';
  ctx.lineWidth = 2;
  for (let deg = -40; deg <= 40; deg += 5) {
    const y = horizonY + deg * pitchScale;
    const len = deg % 10 === 0 ? 150 : 80;
    ctx.beginPath();
    ctx.moveTo(-len / 2, y);
    ctx.lineTo(len / 2, y);
    ctx.stroke();
    if (deg % 10 === 0 && deg !== 0) {
      ctx.fillStyle = 'rgba(255,255,255,0.78)';
      ctx.font = '22px Segoe UI';
      ctx.fillText(String(Math.abs(deg)), -len / 2 - 44, y + 7);
      ctx.fillText(String(Math.abs(deg)), len / 2 + 14, y + 7);
    }
  }
  ctx.restore();

  ctx.strokeStyle = 'rgba(255,255,255,0.9)';
  ctx.lineWidth = 4;
  ctx.beginPath();
  ctx.moveTo(w / 2 - 90, h / 2);
  ctx.lineTo(w / 2 - 24, h / 2);
  ctx.lineTo(w / 2, h / 2 + 18);
  ctx.lineTo(w / 2 + 24, h / 2);
  ctx.lineTo(w / 2 + 90, h / 2);
  ctx.stroke();

  ctx.beginPath();
  ctx.arc(w / 2, h / 2, 165, Math.PI * 0.78, Math.PI * 0.22, false);
  ctx.strokeStyle = 'rgba(255,255,255,0.55)';
  ctx.lineWidth = 3;
  ctx.stroke();

  ctx.save();
  ctx.translate(w / 2, 112);
  for (let deg = -60; deg <= 60; deg += 10) {
    ctx.save();
    ctx.rotate((deg * Math.PI) / 180);
    ctx.strokeStyle = 'rgba(255,255,255,0.72)';
    ctx.lineWidth = deg % 30 === 0 ? 4 : 2;
    ctx.beginPath();
    ctx.moveTo(0, 145);
    ctx.lineTo(0, 170);
    ctx.stroke();
    ctx.restore();
  }
  ctx.rotate((roll * Math.PI) / 180);
  ctx.fillStyle = '#ffd166';
  ctx.beginPath();
  ctx.moveTo(0, 182);
  ctx.lineTo(-14, 156);
  ctx.lineTo(14, 156);
  ctx.closePath();
  ctx.fill();
  ctx.restore();

  ctx.fillStyle = 'rgba(6, 12, 18, 0.78)';
  ctx.strokeStyle = 'rgba(255,255,255,0.12)';
  ctx.lineWidth = 2;
  ctx.fillRect(24, 220, 154, 116);
  ctx.strokeRect(24, 220, 154, 116);
  ctx.fillRect(w - 178, 220, 154, 116);
  ctx.strokeRect(w - 178, 220, 154, 116);
  ctx.fillRect(w - 178, 354, 154, 92);
  ctx.strokeRect(w - 178, 354, 154, 92);

  ctx.fillStyle = '#8da4b7';
  ctx.font = '18px Segoe UI';
  ctx.fillText('SPD', 42, 248);
  ctx.fillText('ALT', w - 158, 248);
  ctx.fillText('VSI', w - 158, 382);

  ctx.fillStyle = '#edf5fb';
  ctx.font = 'bold 48px Segoe UI';
  ctx.fillText(fmtNumber(speed, 1), 42, 302);
  ctx.fillText(fmtNumber(altitude, 0), w - 158, 302);
  ctx.fillText(fmtNumber(vsi, 1), w - 158, 430);

  if (!fresh) {
    ctx.fillStyle = 'rgba(3, 6, 9, 0.55)';
    ctx.fillRect(0, 0, w, h);
    ctx.fillStyle = '#ffd166';
    ctx.font = 'bold 42px Segoe UI';
    ctx.textAlign = 'center';
    ctx.fillText('STALE', w / 2, h / 2 - 10);
    ctx.font = '22px Segoe UI';
    ctx.fillText(`Age ${fmtInt(snapshot?.age_ms)} ms`, w / 2, h / 2 + 28);
    ctx.textAlign = 'left';
  }
}

function drawHsi(snapshot) {
  const ctx = hsiCanvas.getContext('2d');
  const w = hsiCanvas.width;
  const h = hsiCanvas.height;
  ctx.clearRect(0, 0, w, h);

  const heading = Number(snapshot?.mag_heading_deg || snapshot?.yaw_deg || 0);
  const track = Number(snapshot?.headMot_1e5deg || 0) / 100000;
  const speed = Number(snapshot?.gSpeed_mms || 0) / 1000;
  const fix = Number(snapshot?.gps_fix_type || 0);
  const fresh = !!snapshot?.fresh;
  const cx = w / 2;
  const cy = h / 2;
  const radius = Math.min(w, h) * 0.34;

  ctx.fillStyle = '#071019';
  ctx.fillRect(0, 0, w, h);
  ctx.fillStyle = 'rgba(10, 22, 34, 0.95)';
  ctx.beginPath();
  ctx.arc(cx, cy, radius + 36, 0, Math.PI * 2);
  ctx.fill();

  ctx.strokeStyle = 'rgba(255,255,255,0.12)';
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.arc(cx, cy, radius + 12, 0, Math.PI * 2);
  ctx.stroke();

  ctx.beginPath();
  ctx.arc(cx, cy, radius - 82, 0, Math.PI * 2);
  ctx.stroke();

  ctx.save();
  ctx.translate(cx, cy);
  ctx.rotate(-heading * Math.PI / 180);
  for (let deg = 0; deg < 360; deg += 5) {
    ctx.save();
    ctx.rotate((deg * Math.PI) / 180);
    const longTick = deg % 30 === 0;
    ctx.strokeStyle = 'rgba(255,255,255,0.7)';
    ctx.lineWidth = longTick ? 4 : 2;
    ctx.beginPath();
    ctx.moveTo(0, -radius);
    ctx.lineTo(0, -(radius - (longTick ? 24 : 12)));
    ctx.stroke();
    if (longTick) {
      ctx.fillStyle = '#edf5fb';
      ctx.font = '22px Segoe UI';
      ctx.textAlign = 'center';
      const labels = { 0: 'N', 90: 'E', 180: 'S', 270: 'W' };
      const label = labels[deg] || String(deg / 10).padStart(2, '0');
      ctx.fillText(label, 0, -(radius - 42));
    }
    ctx.restore();
  }
  ctx.restore();

  ctx.strokeStyle = '#49b4e8';
  ctx.lineWidth = 4;
  ctx.beginPath();
  ctx.moveTo(cx, cy - radius + 54);
  ctx.lineTo(cx, cy + radius - 60);
  ctx.stroke();
  ctx.fillStyle = '#49b4e8';
  ctx.beginPath();
  ctx.moveTo(cx, cy - radius + 36);
  ctx.lineTo(cx - 12, cy - radius + 64);
  ctx.lineTo(cx + 12, cy - radius + 64);
  ctx.closePath();
  ctx.fill();

  ctx.save();
  ctx.translate(cx, cy);
  ctx.rotate((track - heading) * Math.PI / 180);
  ctx.fillStyle = '#ffd166';
  ctx.beginPath();
  ctx.moveTo(0, -(radius - 96));
  ctx.lineTo(-18, -(radius - 62));
  ctx.lineTo(0, -(radius - 22));
  ctx.lineTo(18, -(radius - 62));
  ctx.closePath();
  ctx.fill();
  ctx.restore();

  ctx.fillStyle = 'rgba(6,12,18,0.78)';
  ctx.fillRect(cx - 58, 38, 116, 48);
  ctx.strokeStyle = 'rgba(255,255,255,0.16)';
  ctx.strokeRect(cx - 58, 38, 116, 48);
  ctx.fillStyle = '#edf5fb';
  ctx.font = 'bold 34px Segoe UI';
  ctx.textAlign = 'center';
  ctx.fillText(String(Math.round(((heading % 360) + 360) % 360)).padStart(3, '0'), cx, 73);

  ctx.textAlign = 'left';
  ctx.fillStyle = '#8da4b7';
  ctx.font = '18px Segoe UI';
  ctx.fillText('TRACK', 36, h - 122);
  ctx.fillText('GS', 36, h - 78);
  ctx.fillText('FIX', w - 140, h - 122);
  ctx.fillText('MODE', w - 140, h - 78);
  ctx.fillStyle = '#edf5fb';
  ctx.font = 'bold 30px Segoe UI';
  ctx.fillText(fmtNumber(track, 1), 36, h - 92);
  ctx.fillText(fmtNumber(speed, 1), 36, h - 48);
  ctx.fillText(String(fix), w - 140, h - 92);
  ctx.fillText(sourceMode(snapshot).toUpperCase(), w - 140, h - 48);

  if (!fresh) {
    ctx.fillStyle = 'rgba(3, 6, 9, 0.55)';
    ctx.fillRect(0, 0, w, h);
    ctx.fillStyle = '#ffd166';
    ctx.font = 'bold 42px Segoe UI';
    ctx.textAlign = 'center';
    ctx.fillText('STALE', cx, cy - 10);
    ctx.font = '22px Segoe UI';
    ctx.fillText(`Age ${fmtInt(snapshot?.age_ms)} ms`, cx, cy + 28);
    ctx.textAlign = 'left';
  }
}

function renderFilesState(files, storage) {
  latestFiles = files || latestFiles;
  latestStorage = storage || latestStorage;
  const fileItems = latestFiles && Array.isArray(latestFiles.files) ? latestFiles.files : [];
  if (!selectedFileName && fileItems.length > 0) selectedFileName = fileItems[0].name;
  if (selectedFileName && !fileItems.some((item) => item.name === selectedFileName)) {
    selectedFileName = fileItems.length > 0 ? fileItems[0].name : '';
  }

  fileSelect.innerHTML = fileItems.map((item) => {
    const selected = item.name === selectedFileName ? ' selected' : '';
    const safeName = String(item.name).replace(/"/g, '&quot;');
    return `<option value="${safeName}"${selected}>${item.name} (${fmtInt(item.size_bytes)} B)</option>`;
  }).join('');
  if (fileItems.length === 0) fileSelect.innerHTML = '<option value="">(no files)</option>';

  const selectedInfo = fileItems.find((item) => item.name === selectedFileName) || null;
  selectedFileMetricEl.textContent = selectedFileDisplay();
  filesJsonEl.textContent = JSON.stringify(fileItems, null, 2);
  renderFields(fileFieldsEl, [
    ['files_complete', latestFiles ? boolText(latestFiles.complete) : '-'],
    ['refresh_inflight', latestFiles ? boolText(latestFiles.refresh_inflight) : '-'],
    ['files_revision', latestFiles ? fmtInt(latestFiles.revision) : '-'],
    ['stored_files', latestFiles ? fmtInt(latestFiles.stored_files) : '-'],
    ['total_files', latestFiles ? fmtInt(latestFiles.total_files) : '-'],
    ['selected_file', fmtText(selectedFileName)],
    ['selected_size', selectedInfo ? fmtInt(selectedInfo.size_bytes) : '-'],
    ['storage_known', latestStorage ? boolText(latestStorage.known) : '-'],
    ['storage_busy', latestStorage ? boolText(latestStorage.busy) : '-'],
    ['backend_ready', latestStorage ? boolText(latestStorage.backend_ready) : '-'],
    ['media_present', latestStorage ? boolText(latestStorage.media_present) : '-'],
    ['mounted', latestStorage ? boolText(latestStorage.mounted) : '-'],
    ['file_count', latestStorage ? fmtInt(latestStorage.file_count) : '-'],
    ['record_prefix', latestStorage ? fmtText(latestStorage.record_prefix) : '-'],
    ['next_record_name', latestStorage ? fmtText(latestStorage.next_record_name) : '-'],
    ['file_last_tx', fmtText(controlStatus.file.tx)],
    ['file_last_ack', fmtText(controlStatus.file.ack)],
    ['file_detail', fmtText(controlStatus.file.detail)],
    ['file_req_id', fmtText(controlStatus.file.reqId)]
  ]);
  setControlsEnabled(true);
}

function renderSnapshot(snapshot, deltaMs) {
  latestSnapshot = snapshot;
  rawJsonEl.textContent = JSON.stringify(snapshot, null, 2);

  const hz = deltaMs && deltaMs > 0 ? (1000 / deltaMs) : null;
  const source = sourceMode(snapshot);
  const hasFix = Number(snapshot.gps_fix_type || 0) >= 2;
  const recordingState = snapshot.recording_active ? 'ACTIVE' : (snapshot.recording_busy ? 'BUSY' : 'IDLE');

  ageMetricEl.textContent = fmtInt(snapshot.age_ms);
  hzMetricEl.textContent = hz ? fmtNumber(hz, 2) : '-';
  rttMetricEl.textContent = fmtInt(snapshot.radio_rtt_ms);
  recordMetricEl.textContent = recordingState;
  pendingMetricEl.textContent = pendingControl ? pendingControl : '-';

  setPill(socketBadgeEl, 'Socket open', 'good');
  setPill(freshBadgeEl, snapshot.fresh ? `Fresh ${fmtInt(snapshot.age_ms)} ms` : `Stale ${fmtInt(snapshot.age_ms)} ms`, snapshot.fresh ? 'good' : 'warn');
  setPill(sourceBadgeEl, `Source ${source}`, source === 'replay' ? 'warn' : (source === 'live' ? 'good' : ''));
  setPill(schemaBadgeEl, `Schema ${fmtInt(snapshot.schema_id)} v${fmtInt(snapshot.schema_version)}`);
  setPill(fixBadgeEl, `Fix ${fmtInt(snapshot.gps_fix_type)} / SV ${fmtInt(snapshot.gps_num_sv)}`, hasFix ? 'good' : 'warn');
  setPill(pfdFreshEl, snapshot.fresh ? 'Fresh' : 'Stale', snapshot.fresh ? 'good' : 'warn');
  setPill(pfdSourceEl, source.toUpperCase(), source === 'replay' ? 'warn' : (source === 'live' ? 'good' : ''));
  setPill(hsiFreshEl, snapshot.fresh ? 'Fresh' : 'Stale', snapshot.fresh ? 'good' : 'warn');
  setPill(hsiSourceEl, source.toUpperCase(), source === 'replay' ? 'warn' : (source === 'live' ? 'good' : ''));

  renderFields(pfdSummaryEl, [
    ['roll_deg', fmtNumber(snapshot.roll_deg, 1)],
    ['pitch_deg', fmtNumber(snapshot.pitch_deg, 1)],
    ['heading_deg', fmtNumber(snapshot.mag_heading_deg, 1)],
    ['track_deg', fmtNumber(Number(snapshot.headMot_1e5deg || 0) / 100000, 1)],
    ['altitude_m', fmtNumber(snapshot.baro_alt_m, 1)],
    ['vsi_mps', fmtNumber(snapshot.baro_vsi_mps, 2)],
    ['groundspeed_mps', fmtNumber(Number(snapshot.gSpeed_mms || 0) / 1000, 1)],
    ['fix_type', fmtInt(snapshot.gps_fix_type)]
  ]);

  renderFields(modeSummaryEl, [
    ['source_mode', source],
    ['fresh', boolText(snapshot.fresh)],
    ['recording_active', boolText(snapshot.recording_active)],
    ['recording_busy', boolText(snapshot.recording_busy)],
    ['replay_active', boolText(snapshot.replay_active)],
    ['replay_paused', boolText(snapshot.replay_paused)],
    ['replay_file_open', boolText(snapshot.replay_file_open)],
    ['replay_current_file', fmtText(snapshot.replay_current_file)],
    ['time_state', fmtText(snapshot.time_state)],
    ['time_source', fmtText(snapshot.time_source)]
  ]);

  renderFields(hsiSummaryEl, [
    ['gps_datetime_utc', gpsDateTimeText(snapshot)],
    ['lat_deg', fmtNumber(Number(snapshot.lat_1e7 || 0) / 1e7, 7)],
    ['lon_deg', fmtNumber(Number(snapshot.lon_1e7 || 0) / 1e7, 7)],
    ['alt_m', fmtNumber(Number(snapshot.hMSL_mm || 0) / 1000, 1)],
    ['groundspeed_mps', fmtNumber(Number(snapshot.gSpeed_mms || 0) / 1000, 1)],
    ['course_deg', fmtNumber(Number(snapshot.headMot_1e5deg || 0) / 100000, 1)],
    ['hAcc_m', fmtNumber(Number(snapshot.hAcc_mm || 0) / 1000, 2)],
    ['sAcc_mps', fmtNumber(Number(snapshot.sAcc_mms || 0) / 1000, 2)]
  ]);

  renderFields(performanceFieldsEl, [
    ['ws_seq', fmtInt(snapshot.ws_seq)],
    ['seq', fmtInt(snapshot.seq)],
    ['source_t_us', fmtInt(snapshot.source_t_us)],
    ['replay_source_seq', fmtInt(snapshot.replay_source_seq)],
    ['replay_source_t_us', fmtInt(snapshot.replay_source_t_us)],
    ['radio_rtt_ms', fmtInt(snapshot.radio_rtt_ms)],
    ['age_ms', fmtInt(snapshot.age_ms)],
    ['drop', fmtInt(snapshot.drop)],
    ['len_err', fmtInt(snapshot.len_err)],
    ['unknown_msg', fmtInt(snapshot.unknown_msg)],
    ['state_gap', fmtInt(snapshot.state_gap)],
    ['state_rewind', fmtInt(snapshot.state_rewind)]
  ]);

  renderFields(statusFieldsEl, [
    ['source_mode', source],
    ['fresh', boolText(snapshot.fresh)],
    ['pending_control', fmtText(pendingControl)],
    ['recording_active', boolText(snapshot.recording_active)],
    ['recording_busy', boolText(snapshot.recording_busy)],
    ['recording_session_id', fmtInt(snapshot.recording_session_id)],
    ['recording_bytes_written', fmtInt(snapshot.recording_bytes_written)],
    ['replay_active', boolText(snapshot.replay_active)],
    ['replay_paused', boolText(snapshot.replay_paused)],
    ['replay_file_open', boolText(snapshot.replay_file_open)],
    ['replay_at_eof', boolText(snapshot.replay_at_eof)],
    ['replay_teensy_seen', boolText(snapshot.replay_teensy_seen)],
    ['replay_session_id', fmtInt(snapshot.replay_session_id)],
    ['replay_records_sent', fmtInt(snapshot.replay_records_sent)],
    ['replay_records_total', fmtInt(snapshot.replay_records_total)],
    ['replay_last_error', fmtInt(snapshot.replay_last_error)],
    ['replay_last_command', fmtInt(snapshot.replay_last_command)],
    ['replay_current_file', fmtText(snapshot.replay_current_file)],
    ['time_state', fmtText(snapshot.time_state)],
    ['time_source', fmtText(snapshot.time_source)],
    ['gps_calendar_present', boolText(snapshot.gps_calendar_present)],
    ['gps_time_valid', boolText(snapshot.gps_time_valid)],
    ['system_time_set', boolText(snapshot.system_time_set)],
    ['system_time_utc_s', fmtInt(snapshot.system_time_utc_s)],
    ['time_last_set_age_ms', fmtInt(snapshot.time_last_set_age_ms)],
    ['time_sync_count', fmtInt(snapshot.time_sync_count)]
  ]);

  renderFields(gpsFieldsEl, [
    ['gps_datetime_utc', gpsDateTimeText(snapshot)],
    ['gps_itow_ms', fmtInt(snapshot.gps_itow_ms)],
    ['lat_1e7', fmtInt(snapshot.lat_1e7)],
    ['lon_1e7', fmtInt(snapshot.lon_1e7)],
    ['lat_deg', fmtNumber(Number(snapshot.lat_1e7 || 0) / 1e7, 7)],
    ['lon_deg', fmtNumber(Number(snapshot.lon_1e7 || 0) / 1e7, 7)],
    ['hMSL_mm', fmtInt(snapshot.hMSL_mm)],
    ['alt_m', fmtNumber(Number(snapshot.hMSL_mm || 0) / 1000, 3)],
    ['gSpeed_mms', fmtInt(snapshot.gSpeed_mms)],
    ['groundspeed_mps', fmtNumber(Number(snapshot.gSpeed_mms || 0) / 1000, 3)],
    ['headMot_1e5deg', fmtInt(snapshot.headMot_1e5deg)],
    ['course_deg', fmtNumber(Number(snapshot.headMot_1e5deg || 0) / 100000, 3)],
    ['hAcc_mm', fmtInt(snapshot.hAcc_mm)],
    ['sAcc_mms', fmtInt(snapshot.sAcc_mms)],
    ['gps_fix_type', fmtInt(snapshot.gps_fix_type)],
    ['gps_num_sv', fmtInt(snapshot.gps_num_sv)]
  ]);

  renderFields(stateFieldsEl, [
    ['roll_deg', fmtNumber(snapshot.roll_deg, 3)],
    ['pitch_deg', fmtNumber(snapshot.pitch_deg, 3)],
    ['yaw_deg', fmtNumber(snapshot.yaw_deg, 3)],
    ['mag_heading_deg', fmtNumber(snapshot.mag_heading_deg, 3)],
    ['baro_temp_c', fmtNumber(snapshot.baro_temp_c, 3)],
    ['baro_press_hpa', fmtNumber(snapshot.baro_press_hpa, 3)],
    ['baro_alt_m', fmtNumber(snapshot.baro_alt_m, 3)],
    ['baro_vsi_mps', fmtNumber(snapshot.baro_vsi_mps, 3)],
    ['fusion_gain', fmtNumber(snapshot.fusion_gain, 3)],
    ['fusion_accel_rej', fmtNumber(snapshot.fusion_accel_rej, 3)],
    ['fusion_mag_rej', fmtNumber(snapshot.fusion_mag_rej, 3)],
    ['fusion_recovery_period', fmtInt(snapshot.fusion_recovery_period)]
  ]);

  renderFields(sensorFieldsEl, [
    ['flags', fmtInt(snapshot.flags)],
    ['raw_present_mask', fmtInt(snapshot.raw_present_mask)],
    ['recording_last_tx', fmtText(controlStatus.recording.tx)],
    ['recording_last_ack', fmtText(controlStatus.recording.ack)],
    ['recording_detail', fmtText(controlStatus.recording.detail)],
    ['recording_req_id', fmtText(controlStatus.recording.reqId)],
    ['fusion_last_tx', fmtText(controlStatus.fusion.tx)],
    ['fusion_last_ack', fmtText(controlStatus.fusion.ack)],
    ['fusion_detail', fmtText(controlStatus.fusion.detail)],
    ['fusion_req_id', fmtText(controlStatus.fusion.reqId)]
  ]);

  if (!fusionInitialized) {
    fusionGainInput.value = Number(snapshot.fusion_gain || 0).toFixed(3);
    fusionAccelInput.value = Number(snapshot.fusion_accel_rej || 0).toFixed(3);
    fusionMagInput.value = Number(snapshot.fusion_mag_rej || 0).toFixed(3);
    fusionRecoveryInput.value = String(Math.round(Number(snapshot.fusion_recovery_period || 0)));
    fusionInitialized = true;
  }

  drawPfd(snapshot);
  drawHsi(snapshot);
  renderFilesState();
}

function clearReconnectTimer() {
  if (!reconnectTimer) return;
  clearTimeout(reconnectTimer);
  reconnectTimer = null;
}

function scheduleReconnect(expectedGeneration) {
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    if (expectedGeneration !== reconnectGeneration) return;
    connect(false);
  }, 1000);
}

function disconnectSocket(manual) {
  manualDisconnect = manual;
  clearReconnectTimer();
  reconnectGeneration += 1;
  const socket = ws;
  ws = null;
  if (socket) {
    socket.onopen = null;
    socket.onmessage = null;
    socket.onclose = null;
    socket.onerror = null;
    try { socket.close(); } catch (_) {}
  }
  setPill(socketBadgeEl, manual ? 'Socket closed' : 'Socket disconnected', manual ? 'warn' : 'bad');
  setControlsEnabled(false);
}

function connect(forced) {
  disconnectSocket(false);
  manualDisconnect = false;
  reconnectGeneration += 1;
  const generation = reconnectGeneration;
  setPill(socketBadgeEl, forced ? 'Socket reconnecting' : 'Socket connecting');
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const socket = new WebSocket(`${proto}//${location.host}/ws`);
  ws = socket;

  socket.onopen = () => {
    if (ws !== socket || generation !== reconnectGeneration) return;
    setPill(socketBadgeEl, 'Socket open', 'good');
    appendEvent('socket open');
    setControlsEnabled(true);
  };

  socket.onmessage = (event) => {
    if (ws !== socket || generation !== reconnectGeneration) return;
    try {
      const msg = JSON.parse(event.data);
      if (msg.type === 'snapshot') {
        const nowMs = Date.now();
        const deltaMs = lastMessageMs ? (nowMs - lastMessageMs) : null;
        lastMessageMs = nowMs;
        renderSnapshot(msg, deltaMs);
        return;
      }
      if (msg.type === 'hello') {
        appendEvent(`hello snapshot_hz=${fmtInt(msg.snapshot_hz)}`);
        return;
      }
      if (msg.type === 'ack') {
        applyAck(msg);
        appendEvent(`ack op=${fmtText(msg.op)} req=${fmtInt(msg.req_id)} ok=${boolText(msg.ok)} code=${fmtInt(msg.code)} detail=${fmtText(msg.detail)}`);
        if (latestSnapshot) renderSnapshot(latestSnapshot, null);
        else renderFilesState();
        return;
      }
      if (msg.type === 'files') {
        appendEvent(`files req=${fmtInt(msg.req_id)} stored=${fmtInt(msg.stored_files)} total=${fmtInt(msg.total_files)} complete=${boolText(msg.complete)}`);
        renderFilesState(msg, null);
        return;
      }
      if (msg.type === 'storage') {
        appendEvent(`storage req=${fmtInt(msg.req_id)} ready=${boolText(msg.backend_ready)} busy=${boolText(msg.busy)} files=${fmtInt(msg.file_count)}`);
        renderFilesState(null, msg);
        return;
      }
      appendEvent(`rx ${fmtText(msg.type)}`);
    } catch (err) {
      rawJsonEl.textContent = `JSON parse error\n\n${String(err)}\n\n${event.data}`;
    }
  };

  socket.onclose = () => {
    if (ws === socket) ws = null;
    if (generation !== reconnectGeneration) return;
    setPill(socketBadgeEl, 'Socket closed', 'warn');
    appendEvent('socket closed');
    setControlsEnabled(false);
    if (!manualDisconnect) scheduleReconnect(generation);
  };

  socket.onerror = () => {
    if (ws !== socket || generation !== reconnectGeneration) return;
    setPill(socketBadgeEl, 'Socket error', 'bad');
    appendEvent('socket error');
  };
}

document.querySelectorAll('.tabs button').forEach((button) => {
  button.addEventListener('click', () => {
    const nextTab = button.dataset.tab;
    if (!nextTab || nextTab === activeTab) return;
    activeTab = nextTab;
    document.querySelectorAll('.tabs button').forEach((el) => el.classList.toggle('active', el === button));
    document.querySelectorAll('.tab').forEach((tab) => tab.classList.toggle('active', tab.id === `tab-${nextTab}`));
    if (latestSnapshot) {
      drawPfd(latestSnapshot);
      drawHsi(latestSnapshot);
    }
  });
});

reconnectBtn.addEventListener('click', () => connect(true));
disconnectBtn.addEventListener('click', () => disconnectSocket(true));
clearBtn.addEventListener('click', () => {
  latestSnapshot = null;
  lastMessageMs = 0;
  latestFiles = null;
  latestStorage = null;
  selectedFileName = '';
  pendingControl = null;
  fusionInitialized = false;
  rawJsonEl.textContent = '{}';
  filesJsonEl.textContent = '[]';
  eventLines.length = 0;
  eventLogEl.textContent = '(no events yet)';
  ageMetricEl.textContent = '-';
  hzMetricEl.textContent = '-';
  rttMetricEl.textContent = '-';
  recordMetricEl.textContent = '-';
  pendingMetricEl.textContent = '-';
  selectedFileMetricEl.textContent = '-';
  fileSelect.innerHTML = '<option value="">(no files)</option>';
  renderFields(performanceFieldsEl, []);
  renderFields(pfdSummaryEl, []);
  renderFields(modeSummaryEl, []);
  renderFields(hsiSummaryEl, []);
  renderFields(statusFieldsEl, []);
  renderFields(gpsFieldsEl, []);
  renderFields(stateFieldsEl, []);
  renderFields(sensorFieldsEl, []);
  renderFields(fileFieldsEl, []);
  updateControlStatus('recording', '-', '-', '-', '-');
  updateControlStatus('file', '-', '-', '-', '-');
  updateControlStatus('fusion', '-', '-', '-', '-');
  setPill(freshBadgeEl, 'Freshness unknown');
  setPill(sourceBadgeEl, 'Source --');
  setPill(schemaBadgeEl, 'Schema --');
  setPill(fixBadgeEl, 'Fix --');
  setPill(pfdFreshEl, 'Fresh --');
  setPill(pfdSourceEl, 'Source --');
  setPill(hsiFreshEl, 'Fresh --');
  setPill(hsiSourceEl, 'Source --');
  drawPfd(null);
  drawHsi(null);
  renderFilesState();
  setControlsEnabled(true);
});

recordStartBtn.addEventListener('click', () => {
  const req = nextReqId();
  recordPending('recording', 'start', req);
  sendControl({ type: 'control', req_id: req, category: 'recording', action: 'start' });
});

recordStopBtn.addEventListener('click', () => {
  const req = nextReqId();
  recordPending('recording', 'stop', req);
  sendControl({ type: 'control', req_id: req, category: 'recording', action: 'stop' });
});

filesRefreshBtn.addEventListener('click', () => {
  const req = nextReqId();
  recordPending('file', 'refresh', req);
  sendControl({ type: 'control', req_id: req, category: 'file', action: 'refresh' });
});

fileDeleteBtn.addEventListener('click', () => {
  if (!selectedFileName) {
    appendEvent('delete blocked: no selected file');
    return;
  }
  const req = nextReqId();
  recordPending('file', `delete ${selectedFileName}`, req);
  sendControl({ type: 'control', req_id: req, category: 'file', action: 'delete', name: selectedFileName });
});

fileExportBtn.addEventListener('click', () => {
  if (!selectedFileName) {
    appendEvent('export blocked: no selected file');
    return;
  }
  const req = nextReqId();
  recordPending('file', `export_csv ${selectedFileName}`, req);
  sendControl({ type: 'control', req_id: req, category: 'file', action: 'export_csv', name: selectedFileName });
});

fileSelect.addEventListener('change', () => {
  selectedFileName = fileSelect.value || '';
  selectedFileMetricEl.textContent = selectedFileDisplay();
  renderFilesState();
});

fusionApplyBtn.addEventListener('click', () => {
  const req = nextReqId();
  recordPending('fusion', 'set', req);
  sendControl({
    type: 'control',
    req_id: req,
    category: 'fusion',
    action: 'set',
    gain: Number(fusionGainInput.value),
    accelerationRejection: Number(fusionAccelInput.value),
    magneticRejection: Number(fusionMagInput.value),
    recoveryTriggerPeriod: Number(fusionRecoveryInput.value)
  });
});

window.addEventListener('resize', () => {
  if (latestSnapshot) {
    drawPfd(latestSnapshot);
    drawHsi(latestSnapshot);
  }
});

drawPfd(null);
drawHsi(null);
renderFilesState();
connect(false);
