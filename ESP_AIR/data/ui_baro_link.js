function f(v, d = 3) {
  return (typeof v === "number" && Number.isFinite(v)) ? v.toFixed(d) : "--";
}

export function initBaroLinkUi(Telemetry, summaryRef) {
  const baroAlt = document.getElementById("baroAlt");
  const baroVsi = document.getElementById("baroVsi");
  const baroPress = document.getElementById("baroPress");
  const linkDetails = document.getElementById("linkDetails");

  const linkQuality = document.getElementById("linkQuality");
  const linkRtt = document.getElementById("linkRtt");
  const linkJitter = document.getElementById("linkJitter");
  const linkLoss = document.getElementById("linkLoss");
  const linkAge = document.getElementById("linkAge");
  const linkRssi = document.getElementById("linkRssi");
  let lastLink = {};

  Telemetry.on("baro", (m) => {
    baroAlt.textContent = f(m.alt_m, 3);
    baroVsi.textContent = f(m.vsi_mps, 3);
    baroPress.textContent = f(m.pressure_pa, 1);
    summaryRef.baro = `alt ${baroAlt.textContent} vsi ${baroVsi.textContent}`;
  });

  Telemetry.on("link", (m) => {
    lastLink = m || {};
    linkRssi.textContent = `RSSI ${m.wifi_rssi_dbm ?? "--"}`;
    linkDetails.textContent =
      `mode=${m.wifi_mode ?? "--"} clients=${m.clients ?? "--"}`;
  });

  function setQualityPill(q, active) {
    linkQuality.classList.remove("quality-good", "quality-mid", "quality-bad", "quality-off");
    if (!active || q == null) {
      linkQuality.classList.add("quality-off");
      linkQuality.textContent = "Q --";
      return;
    }
    linkQuality.textContent = `Q ${q}`;
    if (q >= 80) linkQuality.classList.add("quality-good");
    else if (q >= 50) linkQuality.classList.add("quality-mid");
    else linkQuality.classList.add("quality-bad");
  }

  setInterval(() => {
    const m = Telemetry.getLinkMetrics();
    const age = m.ageMs;
    const ageText = age == null ? "--" : age;
    linkAge.textContent = `Age ${ageText}ms`;
    linkRtt.textContent = `RTT ${m.rttMed == null ? "--" : m.rttMed}ms`;
    linkJitter.textContent = `Jit ${m.jitter == null ? "--" : m.jitter}ms`;
    linkLoss.textContent = `Loss ${m.lossPct.toFixed(1)}%`;
    const active = m.wsOpen && age != null && age <= 2000;
    setQualityPill(active ? m.quality : null, active);
    summaryRef.link = `q ${active ? m.quality : "--"} c ${lastLink.clients ?? "--"}`;
  }, 100);
}
