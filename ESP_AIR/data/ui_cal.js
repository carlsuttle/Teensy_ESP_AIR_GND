function sendAirCmd(Telemetry, cmd, ackEl) {
  const id = Telemetry.nextCmdId();
  const ok = Telemetry.sendCmd({ type: "cmd", id, cmd });
  ackEl.textContent = ok ? `sent ${cmd} (id ${id})` : `send failed (${cmd})`;
}

export function initCalUi(Telemetry) {
  const modeEl = document.getElementById("calMode");
  const ackEl = document.getElementById("calAck");
  const logEl = document.getElementById("calLogView");
  const magOffEl = document.getElementById("calMagOff");
  const magConfEl = document.getElementById("calMagConf");
  const magSamplesEl = document.getElementById("calMagSamples");
  const magElapsedEl = document.getElementById("calMagElapsed");
  const magStoppedEl = document.getElementById("calMagStopped");
  const spdEl = document.getElementById("calSpd");

  const lines = [];
  const maxLines = 300;

  function pushLine(line) {
    if (!line) return;
    lines.push(line);
    if (lines.length > maxLines) lines.shift();
    logEl.textContent = lines.join("\n");
    logEl.scrollTop = logEl.scrollHeight;
  }

  document.getElementById("calBtnMag").addEventListener("click", () => sendAirCmd(Telemetry, "calmag", ackEl));
  document.getElementById("calBtnImu").addEventListener("click", () => sendAirCmd(Telemetry, "calimu", ackEl));
  document.getElementById("calBtnHdg").addEventListener("click", () => sendAirCmd(Telemetry, "gethdg", ackEl));
  document.getElementById("calBtnSpd").addEventListener("click", () => sendAirCmd(Telemetry, "spdtest", ackEl));
  document.getElementById("calBtnStop").addEventListener("click", () => sendAirCmd(Telemetry, "x", ackEl));

  Telemetry.on("ack", (m) => {
    ackEl.textContent = m.ok ? `ACK ${m.id}: ok` : `ACK ${m.id}: ${m.error || "failed"}`;
  });

  Telemetry.on("console", (m) => {
    const line = String(m.line ?? "");
    if (!line) return;
    pushLine(line);

    if (line.includes("CALMAG START")) modeEl.textContent = "CALMAG";
    else if (line.includes("SPDTEST START")) modeEl.textContent = "SPDTEST";
    else if (line.includes("GETHDG START")) modeEl.textContent = "GETHDG";
    else if (line.includes("Calibrating...")) modeEl.textContent = "CALIMU";
    else if (line.includes("CALMAG EXIT") || line.includes("SPDTEST EXIT") || line.includes("GETHDG EXIT") || line.includes("Calibration complete.")) modeEl.textContent = "IDLE";

    const mag = line.match(/CALMAG RESULT.*off=\(([-0-9.]+),\s*([-0-9.]+),\s*([-0-9.]+)\).*conf=([-0-9.]+).*samples=(\d+).*elapsed=(\d+)ms.*stopped=(\w+)/);
    if (mag) {
      magOffEl.textContent = `${mag[1]}, ${mag[2]}, ${mag[3]}`;
      magConfEl.textContent = mag[4];
      magSamplesEl.textContent = mag[5];
      magElapsedEl.textContent = mag[6];
      magStoppedEl.textContent = mag[7];
    }

    const spd = line.match(/SPDTEST t_ms=(\d+)\s+rssi_dbm=([A-Za-z0-9\-]+)/);
    if (spd) {
      spdEl.textContent = `${spd[1]} / ${spd[2]}`;
    }
  });
}
