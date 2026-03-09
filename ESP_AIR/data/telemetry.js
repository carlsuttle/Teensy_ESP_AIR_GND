export const Telemetry = (() => {
  const PING_PERIOD_MS = 1000;
  const PING_TIMEOUT_MS = 1000;
  const RTT_WINDOW = 60;
  const LOSS_WINDOW_MS = 30000;

  let ws = null;
  let wsOpen = false;
  let reconnectDelayMs = 250;
  let reconnectTimer = null;
  let lastRxEpochMs = 0;
  let lastMsgObj = null;
  let lastRaw = "";
  let cmdId = 1;
  let pingSeq = 1;

  const pending = new Map(); // seq -> t0
  const rttSamples = []; // {t, rtt}
  const pongEvents = []; // t
  const lossEvents = []; // t

  const handlersByType = new Map();
  const anyHandlers = [];
  let statusCb = null;

  const log = [];
  let maxLog = 2000;

  function setStatus(s) {
    if (statusCb) statusCb(s);
  }

  function onStatus(fn) {
    statusCb = fn;
  }

  function on(type, fn) {
    if (!handlersByType.has(type)) handlersByType.set(type, []);
    handlersByType.get(type).push(fn);
  }

  function onAny(fn) {
    anyHandlers.push(fn);
  }

  function pruneMetricWindows(now) {
    const cutoff = now - LOSS_WINDOW_MS;
    while (pongEvents.length && pongEvents[0] < cutoff) pongEvents.shift();
    while (lossEvents.length && lossEvents[0] < cutoff) lossEvents.shift();
    while (rttSamples.length && rttSamples[0].t < cutoff) rttSamples.shift();
  }

  function median(nums) {
    if (!nums.length) return null;
    const s = nums.slice().sort((a, b) => a - b);
    const mid = Math.floor(s.length / 2);
    if (s.length % 2) return s[mid];
    return (s[mid - 1] + s[mid]) / 2;
  }

  function computeLinkMetrics() {
    const now = Date.now();
    pruneMetricWindows(now);
    const rtts = rttSamples.map((x) => x.rtt);
    const rttMed = median(rtts);
    let jitter = null;
    if (rttMed != null) {
      const absDev = rtts.map((r) => Math.abs(r - rttMed));
      jitter = median(absDev);
    }

    const sentCount = pongEvents.length + lossEvents.length;
    const lossPct = sentCount > 0 ? (lossEvents.length / sentCount) * 100.0 : 0.0;
    const ageMs = lastRxEpochMs ? (now - lastRxEpochMs) : null;

    let score = 100;
    if (rttMed != null) {
      const pRtt = Math.max(0, Math.min(30, (rttMed - 50) * 0.2));
      const pJit = Math.max(0, Math.min(20, ((jitter ?? 0) - 10) * 0.5));
      const pLoss = Math.max(0, Math.min(40, lossPct * 2.0));
      const pAge = ageMs != null && ageMs > 500 ? Math.max(0, Math.min(40, (ageMs - 500) * 0.02)) : 0;
      score -= pRtt + pJit + pLoss + pAge;
    } else {
      score = 0;
    }
    score = Math.max(0, Math.min(100, Math.round(score)));

    return {
      wsOpen,
      rttMed: rttMed == null ? null : Math.round(rttMed),
      jitter: jitter == null ? null : Math.round(jitter),
      lossPct,
      ageMs,
      quality: score
    };
  }

  function dispatch(obj, rawText) {
    lastMsgObj = obj;
    lastRaw = rawText;
    lastRxEpochMs = Date.now();

    log.push({ rx_ms: lastRxEpochMs, msg: obj });
    if (log.length > maxLog) log.splice(0, log.length - maxLog);

    const hs = handlersByType.get(obj.type) || [];
    for (const fn of hs) fn(obj, rawText);
    for (const fn of anyHandlers) fn(obj, rawText);
  }

  function scheduleReconnect() {
    if (reconnectTimer) return;
    setStatus("reconnecting");
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connect();
    }, reconnectDelayMs);
    reconnectDelayMs = Math.min(reconnectDelayMs * 2, 5000);
  }

  function connect() {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
    ws = new WebSocket(`ws://${location.host}/ws`);
    setStatus("reconnecting");

    ws.onopen = () => {
      reconnectDelayMs = 250;
      wsOpen = true;
      setStatus("connected");
    };

    ws.onmessage = (ev) => {
      const raw = String(ev.data ?? "");
      try {
        const obj = JSON.parse(raw);
        if (obj.type === "pong") {
          const seq = Number(obj.seq);
          const t0 = Number(obj.t0);
          if (pending.has(seq)) pending.delete(seq);
          const rtt = Date.now() - t0;
          const now = Date.now();
          rttSamples.push({ t: now, rtt });
          if (rttSamples.length > RTT_WINDOW) rttSamples.splice(0, rttSamples.length - RTT_WINDOW);
          pongEvents.push(now);
          pruneMetricWindows(now);
        }
        dispatch(obj, raw);
      } catch {
        dispatch({ type: "raw", raw }, raw);
      }
    };

    ws.onclose = () => {
      wsOpen = false;
      scheduleReconnect();
    };
    ws.onerror = () => {
      try { ws.close(); } catch (_) {}
    };
  }

  function sendCmd(cmdObj) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return false;
    ws.send(JSON.stringify(cmdObj));
    return true;
  }

  function nextCmdId() {
    return cmdId++;
  }

  function getLastRxAgeMs() {
    return lastRxEpochMs ? (Date.now() - lastRxEpochMs) : null;
  }

  function pingTick() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const seq = pingSeq++;
    const t0 = Date.now();
    pending.set(seq, t0);
    ws.send(JSON.stringify({ type: "ping", seq, t0 }));
  }

  function pingTimeoutTick() {
    const now = Date.now();
    for (const [seq, t0] of pending.entries()) {
      if (now - t0 > PING_TIMEOUT_MS) {
        pending.delete(seq);
        lossEvents.push(now);
      }
    }
    pruneMetricWindows(now);
  }

  setInterval(pingTick, PING_PERIOD_MS);
  setInterval(pingTimeoutTick, 100);

  function getLastMsg() { return lastMsgObj; }
  function getLastRaw() { return lastRaw; }
  function getLog() { return log; }
  function clearLog() { log.length = 0; }
  function setMaxLog(n) { maxLog = n > 0 ? n : maxLog; }
  function getLinkMetrics() { return computeLinkMetrics(); }

  return {
    connect, onStatus, on, onAny, sendCmd, nextCmdId,
    getLastRxAgeMs, getLastMsg, getLastRaw, getLog, clearLog, setMaxLog,
    getLinkMetrics
  };
})();
