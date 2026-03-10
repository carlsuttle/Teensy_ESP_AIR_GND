function nowStamp() {
  const d = new Date();
  const p = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}_${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
}

export function initLogUi(Telemetry) {
  const logView = document.getElementById("logView");
  const pauseBtn = document.getElementById("logPauseBtn");
  const clearBtn = document.getElementById("logClearBtn");
  const saveBtn = document.getElementById("logSaveBtn");
  const copyBtn = document.getElementById("logCopyBtn");
  const autoScroll = document.getElementById("logAutoScroll");
  const toast = document.getElementById("logToast");

  let paused = false;
  let lastRenderedCount = -1;

  function showToast(t) {
    toast.textContent = t;
    toast.classList.remove("hidden");
    setTimeout(() => toast.classList.add("hidden"), 1000);
  }

  function render() {
    if (paused) return;
    const rows = Telemetry.getLog().map((r) => JSON.stringify(r));
    if (rows.length === lastRenderedCount) return;
    lastRenderedCount = rows.length;
    logView.textContent = rows.join("\n");
    if (autoScroll.checked) logView.scrollTop = logView.scrollHeight;
  }

  pauseBtn.addEventListener("click", () => {
    paused = !paused;
    pauseBtn.textContent = paused ? "Resume Display" : "Pause Display";
  });

  clearBtn.addEventListener("click", () => {
    Telemetry.clearLog();
    lastRenderedCount = -1;
    render();
  });

  saveBtn.addEventListener("click", () => {
    const rows = Telemetry.getLog().map((r) => JSON.stringify(r));
    const text = rows.join("\n") + (rows.length ? "\n" : "");
    const blob = new Blob([text], { type: "application/jsonl" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `air-telemetry_${nowStamp()}.jsonl`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
  });

  copyBtn.addEventListener("click", async () => {
    const raw = Telemetry.getLastRaw();
    if (!raw) {
      showToast("No message yet");
      return;
    }
    try {
      if (navigator.clipboard && navigator.clipboard.writeText) {
        await navigator.clipboard.writeText(raw);
      } else {
        const ta = document.createElement("textarea");
        ta.value = raw;
        document.body.appendChild(ta);
        ta.select();
        document.execCommand("copy");
        ta.remove();
      }
      showToast("Copied");
    } catch (_) {
      showToast("Copy failed");
    }
  });

  Telemetry.onAny(() => render());
  setInterval(render, 250);
}
