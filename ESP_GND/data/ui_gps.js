function fmt(v, d = 3) {
  return (typeof v === "number" && Number.isFinite(v)) ? v.toFixed(d) : "--";
}

export function initGpsUi(Telemetry, summaryRef) {
  const el = {
    fix: document.getElementById("gpsFix"),
    sv: document.getElementById("gpsSv"),
    lat: document.getElementById("gpsLat"),
    lon: document.getElementById("gpsLon"),
    alt: document.getElementById("gpsAlt"),
    spd: document.getElementById("gpsSpd"),
    crs: document.getElementById("gpsCrs"),
    hacc: document.getElementById("gpsHacc"),
    sacc: document.getElementById("gpsSacc"),
    t: document.getElementById("gpsTime")
  };

  Telemetry.on("gps", (m) => {
    el.fix.textContent = m.fix ?? "--";
    el.sv.textContent = m.sv ?? "--";
    el.lat.textContent = fmt(m.lat, 7);
    el.lon.textContent = fmt(m.lon, 7);
    el.alt.textContent = fmt(m.hMSL_m, 3);
    el.spd.textContent = fmt(m.gSpeed_mps, 3);
    el.crs.textContent = fmt(m.headMot_deg, 5);
    el.hacc.textContent = fmt(m.hAcc_m, 3);
    el.sacc.textContent = fmt(m.sAcc_mps, 3);
    el.t.textContent = `${m.itow_ms ?? "--"} / ${m.utc ?? "--"}`;
    summaryRef.gps = `fix ${m.fix ?? "--"} sv ${m.sv ?? "--"} lat ${fmt(m.lat, 6)} lon ${fmt(m.lon, 6)}`;
  });
}
