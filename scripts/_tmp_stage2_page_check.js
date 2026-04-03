
    const socketBadgeEl = document.getElementById('socketBadge');
    const freshBadgeEl = document.getElementById('freshBadge');
    const schemaBadgeEl = document.getElementById('schemaBadge');
    const fixBadgeEl = document.getElementById('fixBadge');
    const deltaMsMetricEl = document.getElementById('deltaMsMetric');
    const hzMetricEl = document.getElementById('hzMetric');
    const rttMetricEl = document.getElementById('rttMetric');
    const ageMetricEl = document.getElementById('ageMetric');
    const performanceFieldsEl = document.getElementById('performanceFields');
    const statusFieldsEl = document.getElementById('statusFields');
    const gpsFieldsEl = document.getElementById('gpsFields');
    const stateFieldsEl = document.getElementById('stateFields');
    const sensorFieldsEl = document.getElementById('sensorFields');
    const fileFieldsEl = document.getElementById('fileFields');
    const filesJsonEl = document.getElementById('filesJson');
    const eventLogEl = document.getElementById('eventLog');
    const rawJsonEl = document.getElementById('rawJson');
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

    function setBadge(el, text, mode = '') {
      el.textContent = text;
      el.className = 'badge' + (mode ? ` ${mode}` : '');
    }

    function fieldHtml(key, value) {
      return `<div class="field"><div class="field-key">${key}</div><div class="field-value">${value}</div></div>`;
    }

    function renderFields(target, entries) {
      target.innerHTML = entries.map(([key, value]) => fieldHtml(key, value)).join('');
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

    function boolText(value) {
      return value ? 'true' : 'false';
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

    function nextReqId() {
      const value = reqId;
      reqId += 1;
      return value;
    }

    function sendControl(message) {
      if (!ws || ws.readyState !== WebSocket.OPEN) {
        appendEvent('control blocked: websocket not open');
        return;
      }
      ws.send(JSON.stringify(message));
      appendEvent(`tx ${message.category}/${message.action} req=${message.req_id}`);
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
        return `<option value="${item.name.replace(/"/g, '&quot;')}"${selected}>${item.name} (${fmtInt(item.size_bytes)} B)</option>`;
      }).join('');
      if (fileItems.length === 0) {
        fileSelect.innerHTML = '<option value="">(no files)</option>';
      }
      const selectedInfo = fileItems.find((item) => item.name === selectedFileName) || null;
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
        ['next_record_name', latestStorage ? fmtText(latestStorage.next_record_name) : '-']
      ]);
      setControlsEnabled(true);
    }

    function renderSnapshot(snapshot, deltaMs) {
      latestSnapshot = snapshot;
      rawJsonEl.textContent = JSON.stringify(snapshot, null, 2);

      const hz = deltaMs && deltaMs > 0 ? (1000 / deltaMs) : null;
      deltaMsMetricEl.textContent = deltaMs ? fmtInt(deltaMs) : '-';
      hzMetricEl.textContent = hz ? fmtNumber(hz, 2) : '-';
      rttMetricEl.textContent = fmtInt(snapshot.radio_rtt_ms);
      ageMetricEl.textContent = fmtInt(snapshot.age_ms);

      setBadge(socketBadgeEl, 'Socket open', 'good');
      setBadge(freshBadgeEl,
               snapshot.fresh ? `Fresh ${fmtInt(snapshot.age_ms)} ms` : `Stale ${fmtInt(snapshot.age_ms)} ms`,
               snapshot.fresh ? 'good' : 'warn');
      setBadge(schemaBadgeEl, `Schema ${fmtInt(snapshot.schema_id)} v${fmtInt(snapshot.schema_version)}`);
      const hasFix = Number(snapshot.gps_fix_type || 0) >= 2;
      setBadge(fixBadgeEl,
               `Fix ${fmtInt(snapshot.gps_fix_type)} / SV ${fmtInt(snapshot.gps_num_sv)}`,
               hasFix ? 'good' : 'warn');

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
        ['state_rewind', fmtInt(snapshot.state_rewind)],
        ['fresh', snapshot.fresh ? 'true' : 'false'],
        ['has_state', snapshot.has_state ? 'true' : 'false'],
        ['has_fix', hasFix ? 'true' : 'false'],
        ['recording_active', snapshot.recording_active ? 'true' : 'false'],
        ['recording_busy', snapshot.recording_busy ? 'true' : 'false'],
        ['recording_session_id', fmtInt(snapshot.recording_session_id)],
        ['recording_bytes_written', fmtInt(snapshot.recording_bytes_written)]
      ]);

      renderFields(statusFieldsEl, [
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
        ['replay_current_file', fmtText(snapshot.replay_current_file)]
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
        ['raw_present_mask', fmtInt(snapshot.raw_present_mask)]
      ]);

      if (!fusionInitialized) {
        fusionGainInput.value = Number(snapshot.fusion_gain || 0).toFixed(3);
        fusionAccelInput.value = Number(snapshot.fusion_accel_rej || 0).toFixed(3);
        fusionMagInput.value = Number(snapshot.fusion_mag_rej || 0).toFixed(3);
        fusionRecoveryInput.value = String(Math.round(Number(snapshot.fusion_recovery_period || 0)));
        fusionInitialized = true;
      }

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
      setBadge(socketBadgeEl, manual ? 'Socket closed' : 'Socket disconnected', manual ? 'warn' : 'bad');
      setControlsEnabled(false);
    }

    function connect(forced) {
      disconnectSocket(false);
      manualDisconnect = false;
      reconnectGeneration += 1;
      const generation = reconnectGeneration;
      setBadge(socketBadgeEl, forced ? 'Socket reconnecting' : 'Socket connecting');
      const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
      const socket = new WebSocket(`${proto}//${location.host}/ws`);
      ws = socket;

      socket.onopen = () => {
        if (ws !== socket || generation !== reconnectGeneration) return;
        setBadge(socketBadgeEl, 'Socket open', 'good');
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
            appendEvent(`ack op=${fmtText(msg.op)} req=${fmtInt(msg.req_id)} ok=${boolText(msg.ok)} code=${fmtInt(msg.code)} detail=${fmtText(msg.detail)}`);
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
        setBadge(socketBadgeEl, 'Socket closed', 'warn');
        appendEvent('socket closed');
        setControlsEnabled(false);
        if (!manualDisconnect) scheduleReconnect(generation);
      };

      socket.onerror = () => {
        if (ws !== socket || generation !== reconnectGeneration) return;
        setBadge(socketBadgeEl, 'Socket error', 'bad');
        appendEvent('socket error');
      };
    }

    reconnectBtn.addEventListener('click', () => connect(true));
    disconnectBtn.addEventListener('click', () => disconnectSocket(true));
    clearBtn.addEventListener('click', () => {
      latestSnapshot = null;
      lastMessageMs = 0;
      rawJsonEl.textContent = '{}';
      deltaMsMetricEl.textContent = '-';
      hzMetricEl.textContent = '-';
      rttMetricEl.textContent = '-';
      ageMetricEl.textContent = '-';
      renderFields(performanceFieldsEl, []);
      renderFields(statusFieldsEl, []);
      renderFields(gpsFieldsEl, []);
      renderFields(stateFieldsEl, []);
      renderFields(sensorFieldsEl, []);
      renderFields(fileFieldsEl, []);
      filesJsonEl.textContent = '[]';
      eventLines.length = 0;
      eventLogEl.textContent = '(no events yet)';
      latestFiles = null;
      latestStorage = null;
      selectedFileName = '';
      fileSelect.innerHTML = '<option value="">(no files)</option>';
      fusionInitialized = false;
      setBadge(freshBadgeEl, 'Freshness unknown');
      setBadge(schemaBadgeEl, 'Schema --');
      setBadge(fixBadgeEl, 'Fix --');
      setControlsEnabled(true);
    });

    recordStartBtn.addEventListener('click', () => {
      sendControl({ type: 'control', req_id: nextReqId(), category: 'recording', action: 'start' });
    });

    recordStopBtn.addEventListener('click', () => {
      sendControl({ type: 'control', req_id: nextReqId(), category: 'recording', action: 'stop' });
    });

    filesRefreshBtn.addEventListener('click', () => {
      sendControl({ type: 'control', req_id: nextReqId(), category: 'file', action: 'refresh' });
    });

    fileDeleteBtn.addEventListener('click', () => {
      if (!selectedFileName) {
        appendEvent('delete blocked: no selected file');
        return;
      }
      sendControl({ type: 'control', req_id: nextReqId(), category: 'file', action: 'delete', name: selectedFileName });
    });

    fileExportBtn.addEventListener('click', () => {
      if (!selectedFileName) {
        appendEvent('export blocked: no selected file');
        return;
      }
      sendControl({ type: 'control', req_id: nextReqId(), category: 'file', action: 'export_csv', name: selectedFileName });
    });

    fileSelect.addEventListener('change', () => {
      selectedFileName = fileSelect.value || '';
      renderFilesState();
    });

    fusionApplyBtn.addEventListener('click', () => {
      sendControl({
        type: 'control',
        req_id: nextReqId(),
        category: 'fusion',
        action: 'set',
        gain: Number(fusionGainInput.value),
        accelerationRejection: Number(fusionAccelInput.value),
        magneticRejection: Number(fusionMagInput.value),
        recoveryTriggerPeriod: Number(fusionRecoveryInput.value)
      });
    });

    connect(false);
  