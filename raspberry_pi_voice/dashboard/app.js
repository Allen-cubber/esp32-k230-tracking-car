const $ = (id) => document.getElementById(id);

const state = {
  baseTopic: "",
  status: null,
  track: null,
  lastStatusAt: 0,
  packetCount: 0,
  samples: [],
  maxSamples: 180,
  pendingPid: null,
  pidPendingUntil: 0,
  pidDirty: false,
};

const els = {
  topicLine: $("topicLine"),
  wsBadge: $("wsBadge"),
  mqttBadge: $("mqttBadge"),
  espBadge: $("espBadge"),
  stateValue: $("stateValue"),
  modeValue: $("modeValue"),
  distanceValue: $("distanceValue"),
  distanceBar: $("distanceBar"),
  leftSpeedValue: $("leftSpeedValue"),
  rightSpeedValue: $("rightSpeedValue"),
  leftCmdValue: $("leftCmdValue"),
  rightCmdValue: $("rightCmdValue"),
  ageValue: $("ageValue"),
  trackValue: $("trackValue"),
  yawValue: $("yawValue"),
  yawRateValue: $("yawRateValue"),
  panValue: $("panValue"),
  tiltValue: $("tiltValue"),
  headingValue: $("headingValue"),
  turnValue: $("turnValue"),
  ackValue: $("ackValue"),
  speedInput: $("speedInput"),
  speedLabel: $("speedLabel"),
  installButton: $("installButton"),
  pidAckValue: $("pidAckValue"),
  pidKp: $("pidKp"),
  pidKi: $("pidKi"),
  pidKd: $("pidKd"),
  pidTargetMax: $("pidTargetMax"),
  pidIntegralLimit: $("pidIntegralLimit"),
  pidCorrectionLimit: $("pidCorrectionLimit"),
  pidMinDuty: $("pidMinDuty"),
  pidApplyButton: $("pidApplyButton"),
  pidDefaultButton: $("pidDefaultButton"),
  packetCount: $("packetCount"),
  eventLog: $("eventLog"),
  chartCanvas: $("chartCanvas"),
  yawCanvas: $("yawCanvas"),
  trackCanvas: $("trackCanvas"),
};

let installPromptEvent = null;
const defaultPid = {
  target_max_pps: 3000,
  kp: 0.65,
  ki: 0.18,
  kd: 0,
  integral_limit: 4000,
  correction_limit: 2200,
  min_duty: 500,
};
const dashboardVersion = "apk-local1";

function fmt(value, digits = 1) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) return "--";
  return Number(value).toFixed(digits);
}

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function setBadge(el, online, label, warn = false) {
  el.textContent = label;
  el.classList.toggle("online", online && !warn);
  el.classList.toggle("warn", online && warn);
  el.classList.toggle("offline", !online);
}

function topicSuffix(topic) {
  return state.baseTopic && topic.startsWith(`${state.baseTopic}/`)
    ? topic.slice(state.baseTopic.length + 1)
    : topic;
}

function pushLog(packet) {
  state.packetCount += 1;
  els.packetCount.textContent = String(state.packetCount);

  const row = document.createElement("div");
  row.className = "event";
  const time = new Date().toLocaleTimeString();
  const suffix = packet.topic ? topicSuffix(packet.topic) : packet.type;
  row.textContent = `[${time}] ${suffix} ${JSON.stringify(packet.payload ?? packet)}`;
  els.eventLog.prepend(row);

  while (els.eventLog.children.length > 60) {
    els.eventLog.lastElementChild.remove();
  }
}

function addSample(status) {
  const motion = status.motion || {};
  const ultrasonic = status.ultrasonic || {};
  const mpu = status.mpu || {};
  const speed = (Math.abs(Number(motion.left_measured_pps) || 0) +
                 Math.abs(Number(motion.right_measured_pps) || 0)) / 2;
  state.samples.push({
    t: Date.now(),
    speed,
    distance: Number(ultrasonic.distance_cm) || 0,
    yaw: Number(mpu.yaw_deg) || 0,
  });
  if (state.samples.length > state.maxSamples) {
    state.samples.shift();
  }
}

function setInputValue(input, value, digits = 2) {
  if (document.activeElement === input || value === undefined || value === null) return;
  const numeric = Number(value);
  if (Number.isNaN(numeric)) return;
  input.value = digits > 0
    ? numeric.toFixed(digits).replace(/\.?0+$/, "")
    : String(Math.round(numeric));
}

function syncPidInputs(pid) {
  if (!pid) return;
  setInputValue(els.pidKp, pid.kp, 3);
  setInputValue(els.pidKi, pid.ki, 3);
  setInputValue(els.pidKd, pid.kd, 3);
  setInputValue(els.pidTargetMax, pid.target_max_pps, 0);
  setInputValue(els.pidIntegralLimit, pid.integral_limit, 0);
  setInputValue(els.pidCorrectionLimit, pid.correction_limit, 0);
  setInputValue(els.pidMinDuty, pid.min_duty, 0);
}

function pidMatches(a, b) {
  if (!a || !b) return false;
  const keys = [
    "target_max_pps",
    "kp",
    "ki",
    "kd",
    "integral_limit",
    "correction_limit",
    "min_duty",
  ];
  return keys.every((key) => Math.abs(Number(a[key]) - Number(b[key])) < 0.001);
}

function renderStatus() {
  const status = state.status || {};
  const motion = status.motion || {};
  const ultrasonic = status.ultrasonic || {};
  const mpu = status.mpu || {};
  const track = status.track || state.track || {};
  const control = status.control || {};
  const pid = status.pid || null;
  const hasStatus = Boolean(state.status);

  els.stateValue.textContent = status.state || (state.track ? "等待 ESP32" : "--");
  els.modeValue.textContent = hasStatus
    ? `${status.manual_mode ? "手动保持" : "自动跟随"} / ${status.obstacle_hold ? "避障保持" : "通行"}`
    : (state.track ? "已收到 K230 track，未收到 ESP32 status" : "--");

  const distance = Number(ultrasonic.distance_cm);
  els.distanceValue.textContent = fmt(distance, 1);
  const distancePct = clamp((distance / 120) * 100, 0, 100);
  els.distanceBar.style.width = `${Number.isFinite(distancePct) ? distancePct : 0}%`;
  els.distanceBar.style.background = distance > 0 && distance < 15 ? "var(--danger)" : distance < 25 ? "var(--warn)" : "var(--accent)";

  els.leftSpeedValue.textContent = fmt(motion.left_measured_pps, 0);
  els.rightSpeedValue.textContent = fmt(motion.right_measured_pps, 0);
  els.leftCmdValue.textContent = `cmd ${fmt(motion.left_target_cmd, 0)} / pwm ${fmt(motion.left_applied_duty, 0)}`;
  els.rightCmdValue.textContent = `cmd ${fmt(motion.right_target_cmd, 0)} / pwm ${fmt(motion.right_applied_duty, 0)}`;

  els.yawValue.textContent = fmt(mpu.yaw_deg, 1);
  els.yawRateValue.textContent = fmt(mpu.yaw_rate_dps, 1);
  els.panValue.textContent = fmt(track.pan_deg ?? track.pan, 1);
  els.tiltValue.textContent = fmt(track.tilt_deg ?? track.tilt, 1);
  els.headingValue.textContent = fmt(control.heading_error_deg, 1);
  els.turnValue.textContent = fmt(control.turn_output, 0);
  els.trackValue.textContent = track.valid ? "目标有效" : track.seen ? "目标丢失" : "等待目标";
  if (pidMatches(pid, state.pendingPid)) {
    state.pendingPid = null;
    state.pidPendingUntil = 0;
    state.pidDirty = false;
    els.pidAckValue.textContent = "PID 已同步";
  }
  if (!state.pendingPid && !state.pidDirty) {
    syncPidInputs(pid);
  } else if (Date.now() > state.pidPendingUntil) {
    els.pidAckValue.textContent = "已下发，等待 ESP32 确认";
  }

  const ageMs = Date.now() - state.lastStatusAt;
  els.ageValue.textContent = state.lastStatusAt ? `${Math.round(ageMs)} ms ago` : "--";
  setBadge(els.espBadge,
           state.lastStatusAt && ageMs < 1200,
           state.lastStatusAt ? (ageMs < 1200 ? "ESP32 online" : "ESP32 stale") : "no ESP32 status",
           state.lastStatusAt && ageMs >= 600 && ageMs < 1200);

  drawCharts();
  drawYaw(Number(mpu.yaw_deg) || 0);
  drawTrack(track);
}

function drawCharts() {
  const canvas = els.chartCanvas;
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = "#10151b";
  ctx.fillRect(0, 0, w, h);

  ctx.strokeStyle = "#26303a";
  ctx.lineWidth = 1;
  for (let i = 1; i < 6; i += 1) {
    const y = (h / 6) * i;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  const samples = state.samples;
  if (samples.length < 2) return;

  drawSeries(ctx, samples, "speed", "#42d6a4", 0, 3200, w, h);
  drawSeries(ctx, samples, "distance", "#ffd166", 0, 120, w, h);
  drawSeries(ctx, samples, "yaw", "#6bb8ff", -180, 180, w, h);
}

function drawSeries(ctx, samples, key, color, min, max, w, h) {
  ctx.strokeStyle = color;
  ctx.lineWidth = 2;
  ctx.beginPath();
  samples.forEach((sample, index) => {
    const x = (index / (state.maxSamples - 1)) * w;
    const y = h - ((clamp(sample[key], min, max) - min) / (max - min)) * h;
    if (index === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();
}

function drawYaw(deg) {
  const canvas = els.yawCanvas;
  const ctx = canvas.getContext("2d");
  const cx = canvas.width / 2;
  const cy = canvas.height / 2;
  const r = 86;
  const rad = ((deg - 90) * Math.PI) / 180;

  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = "#10151b";
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.strokeStyle = "#303a46";
  ctx.lineWidth = 10;
  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, Math.PI * 2);
  ctx.stroke();
  ctx.strokeStyle = "#6bb8ff";
  ctx.lineWidth = 4;
  ctx.beginPath();
  ctx.moveTo(cx, cy);
  ctx.lineTo(cx + Math.cos(rad) * (r - 10), cy + Math.sin(rad) * (r - 10));
  ctx.stroke();
  ctx.fillStyle = "#eef3f7";
  ctx.beginPath();
  ctx.arc(cx, cy, 5, 0, Math.PI * 2);
  ctx.fill();
}

function drawTrack(track) {
  const canvas = els.trackCanvas;
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = "#10151b";
  ctx.fillRect(0, 0, w, h);

  ctx.strokeStyle = "#26303a";
  ctx.lineWidth = 1;
  for (let x = 0; x <= w; x += 80) {
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, h);
    ctx.stroke();
  }
  for (let y = 0; y <= h; y += 60) {
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  const fw = 480;
  const fh = 360;
  const bx = Number(track.x) || 0;
  const by = Number(track.y) || 0;
  const bw = Number(track.w) || 0;
  const bh = Number(track.h) || 0;
  const sx = w / fw;
  const sy = h / fh;

  ctx.strokeStyle = track.valid ? "#42d6a4" : "#ff5c7a";
  ctx.lineWidth = 3;
  ctx.strokeRect(bx * sx, by * sy, bw * sx, bh * sy);
  ctx.fillStyle = track.valid ? "rgba(66,214,164,0.12)" : "rgba(255,92,122,0.12)";
  ctx.fillRect(bx * sx, by * sy, bw * sx, bh * sy);
}

function connect() {
  const query = new URLSearchParams(window.location.search);
  const queryWsHost = query.get("wsHost");
  const queryWsPort = query.get("wsPort");
  if (queryWsHost) {
    localStorage.setItem("dashboardWsHost", queryWsHost);
  }
  if (queryWsPort) {
    localStorage.setItem("dashboardWsPort", queryWsPort);
  }
  const defaultWsHost = window.location.protocol === "file:"
    ? "10.0.2.2"
    : (window.location.hostname || "127.0.0.1");
  const wsHost = queryWsHost || localStorage.getItem("dashboardWsHost") || defaultWsHost;
  const wsPort = queryWsPort || localStorage.getItem("dashboardWsPort") || "8765";
  const wsUrl = `ws://${wsHost}:${wsPort}`;
  const ws = new WebSocket(wsUrl);
  console.info(`Tracking car dashboard ${dashboardVersion}`);

  ws.addEventListener("open", () => {
    window.dashboardWs = ws;
    setBadge(els.wsBadge, true, "WebSocket online");
  });

  ws.addEventListener("close", () => {
    setBadge(els.wsBadge, false, "WebSocket offline");
    setTimeout(connect, 1200);
  });

  ws.addEventListener("message", (event) => {
    const packet = JSON.parse(event.data);
    if (packet.type === "hello") {
      state.baseTopic = packet.base_topic;
      els.topicLine.textContent = `${packet.mqtt_host}:${packet.mqtt_port} / ${packet.base_topic}/#`;
      setBadge(els.mqttBadge, packet.connected, packet.connected ? "MQTT online" : "MQTT offline");
      (packet.latest || []).forEach(handlePacket);
      return;
    }
    handlePacket(packet);
  });
}

function setActiveTab(tab) {
  document.querySelectorAll("[data-tab]").forEach((button) => {
    button.classList.toggle("active", button.dataset.tab === tab);
  });
  document.querySelectorAll(".app-view").forEach((view) => {
    view.classList.toggle("active", view.dataset.view === tab);
  });

  requestAnimationFrame(() => {
    drawCharts();
    drawYaw(Number((state.status || {}).mpu?.yaw_deg) || 0);
    drawTrack(((state.status || {}).track) || state.track || {});
  });
}

function handlePacket(packet) {
  if (packet.type === "broker") {
    setBadge(els.mqttBadge, packet.connected, packet.connected ? "MQTT online" : "MQTT offline");
    return;
  }
  if (packet.type !== "mqtt") return;

  const suffix = packet.suffix || topicSuffix(packet.topic);
  if (suffix === "status") {
    state.status = packet.payload;
    state.lastStatusAt = Date.now();
    addSample(packet.payload);
  } else if (suffix === "track") {
    state.track = {
      seen: true,
      valid: packet.payload.valid,
      pan_deg: packet.payload.pan,
      tilt_deg: packet.payload.tilt,
      x: packet.payload.x,
      y: packet.payload.y,
      w: packet.payload.w,
      h: packet.payload.h,
    };
  } else if (suffix === "cmd/ack") {
    els.ackValue.textContent = packet.payload.ok ? "命令成功" : `失败 ${packet.payload.error || ""}`;
    if (packet.payload.cmd === "set_pid") {
      els.pidAckValue.textContent = packet.payload.ok ? "PID 已下发，等状态确认" : `失败 ${packet.payload.error || ""}`;
      if (!packet.payload.ok) {
        state.pendingPid = null;
        state.pidPendingUntil = 0;
      }
    }
  }
  pushLog(packet);
  renderStatus();
}

function actionPayload(target, name) {
  const speed = Number(els.speedInput.value);
  const duration = name === "stop" ? 0 : 700;
  return {
    emotion: name === "stop" ? "alert" : "neutral",
    action: {
      target,
      name,
      speed,
      duration_ms: duration,
      pan_delta_deg: 0,
      tilt_delta_deg: 0,
    },
    voice: { text: "dashboard" },
  };
}

function send(packet) {
  if (!window.dashboardWs || window.dashboardWs.readyState !== WebSocket.OPEN) return;
  window.dashboardWs.send(JSON.stringify(packet));
}

function readPidInputs() {
  return {
    target_max_pps: Number(els.pidTargetMax.value),
    kp: Number(els.pidKp.value),
    ki: Number(els.pidKi.value),
    kd: Number(els.pidKd.value),
    integral_limit: Number(els.pidIntegralLimit.value),
    correction_limit: Number(els.pidCorrectionLimit.value),
    min_duty: Number(els.pidMinDuty.value),
  };
}

function applyPid(payload) {
  els.pidAckValue.textContent = "正在下发...";
  state.pendingPid = payload;
  state.pidPendingUntil = Date.now() + 3000;
  state.pidDirty = true;
  syncPidInputs(payload);
  send({ type: "set_pid", payload });
}

document.querySelectorAll("[data-action]").forEach((button) => {
  button.addEventListener("click", () => {
    send({ type: "execute_action", payload: actionPayload("chassis", button.dataset.action) });
  });
});

document.querySelectorAll("[data-mode]").forEach((button) => {
  button.addEventListener("click", () => {
    send({ type: "execute_action", payload: actionPayload("mode", button.dataset.mode) });
  });
});

document.querySelectorAll("[data-gimbal]").forEach((button) => {
  button.addEventListener("click", () => {
    send({ type: "gimbal", action: button.dataset.gimbal, duration_ms: 800 });
  });
});

els.speedInput.addEventListener("input", () => {
  els.speedLabel.textContent = els.speedInput.value;
});

[
  els.pidKp,
  els.pidKi,
  els.pidKd,
  els.pidTargetMax,
  els.pidIntegralLimit,
  els.pidCorrectionLimit,
  els.pidMinDuty,
].forEach((input) => {
  input.addEventListener("input", () => {
    state.pidDirty = true;
    els.pidAckValue.textContent = "PID 已修改，未应用";
  });
});

els.pidApplyButton.addEventListener("click", () => {
  applyPid(readPidInputs());
});

els.pidDefaultButton.addEventListener("click", () => {
  syncPidInputs(defaultPid);
  applyPid(defaultPid);
});

document.querySelectorAll("[data-tab]").forEach((button) => {
  button.addEventListener("click", () => setActiveTab(button.dataset.tab));
});

window.addEventListener("beforeinstallprompt", (event) => {
  event.preventDefault();
  installPromptEvent = event;
  els.installButton.hidden = false;
});

els.installButton.addEventListener("click", async () => {
  if (!installPromptEvent) return;
  installPromptEvent.prompt();
  await installPromptEvent.userChoice;
  installPromptEvent = null;
  els.installButton.hidden = true;
});

if ("serviceWorker" in navigator) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.getRegistrations()
      .then((registrations) => registrations.forEach((registration) => registration.unregister()))
      .catch(() => {});
  });
}

setInterval(renderStatus, 250);
connect();
