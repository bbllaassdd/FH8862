const defaults = {
  receiverIp: "192.168.1.2",
  mainPort: 1234,
  mainWidth: 1280,
  mainHeight: 720,
  mainFps: 25,
  mainBitrate: 1750000,
  subWidth: 640,
  subHeight: 360,
  subFps: 15,
  subBitrate: 512000
};

let firstLoad = true;
let toastTimer;

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];

async function api(path, options = {}) {
  const response = await fetch(path, options);
  const data = await response.json();
  if (!response.ok || data.ok === false) {
    throw new Error(data.error || "操作失败");
  }
  return data;
}

function postJson(path, body) {
  return api(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body)
  });
}

function showToast(message, error = false) {
  const toast = $("#toast");
  toast.textContent = message;
  toast.classList.toggle("error", error);
  toast.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove("show"), 3200);
}

function formatBitrate(value) {
  if (value >= 1_000_000) {
    return `${(value / 1_000_000).toFixed(2).replace(/\.00$/, "")} Mbps`;
  }
  return `${Math.round(value / 1000)} Kbps`;
}

function formatBytes(value) {
  if (value >= 1024 ** 3) return `${(value / 1024 ** 3).toFixed(2)} GB`;
  if (value >= 1024 ** 2) return `${(value / 1024 ** 2).toFixed(2)} MB`;
  if (value >= 1024) return `${(value / 1024).toFixed(1)} KB`;
  return `${value} B`;
}

function formatTime(seconds) {
  const safe = Math.max(0, Math.min(60, Number(seconds) || 0));
  return `00:${String(safe).padStart(2, "0")}`;
}

function fillForm(config) {
  Object.entries(config).forEach(([key, value]) => {
    const input = document.querySelector(`[name="${key}"]`);
    if (input) input.value = value;
  });
}

function readForm() {
  return Object.fromEntries(
    [...new FormData($("#settingsForm")).entries()].map(([key, value]) => {
      return [key, key === "receiverIp" ? value.trim() : Number(value)];
    })
  );
}

function renderConfig(config, boardCommand) {
  $("#mainPortBadge").textContent = config.mainPort;
  $("#subPortBadge").textContent = Number(config.mainPort) + 1;
  $("#mainResolution").textContent = `${config.mainWidth} × ${config.mainHeight}`;
  $("#subResolution").textContent = `${config.subWidth} × ${config.subHeight}`;
  $("#mainFpsMetric").textContent = `${config.mainFps} fps`;
  $("#subFpsMetric").textContent = `${config.subFps} fps`;
  $("#mainBitrateMetric").textContent = formatBitrate(config.mainBitrate);
  $("#subBitrateMetric").textContent = formatBitrate(config.subBitrate);
  $("#boardCommand").textContent = boardCommand;
  if (firstLoad) fillForm(config);
}

function renderJob(stream, job) {
  const active = job.state === "recording";
  const finished = job.state === "finished";
  const state = $(`#${stream}RecordState`);
  const label = $(`#${stream}RecordLabel`);
  const time = $(`#${stream}RecordTime`);
  const progress = $(`#${stream}Progress`);
  const button = document.querySelector(`[data-record-stream="${stream}"]`);

  state.classList.toggle("recording", active);
  label.textContent = active ? `正在保存：${job.file}` : finished ? `已完成：${job.file}` : "等待操作";
  time.textContent = active || finished ? formatTime(job.elapsed) : "00:00";
  progress.style.width = `${job.progress || 0}%`;
  button.disabled = active;
  button.innerHTML = active
    ? '<span class="record-dot"></span>正在录像'
    : '<span class="record-dot"></span>录制 60 秒';
}

function renderFiles(files) {
  const body = $("#fileList");
  const empty = $("#emptyFiles");
  body.replaceChildren();
  empty.classList.toggle("show", files.length === 0);
  for (const file of files) {
    const row = document.createElement("tr");
    const nameCell = document.createElement("td");
    nameCell.textContent = file.name;
    const typeCell = document.createElement("td");
    typeCell.textContent = file.extension;
    const sizeCell = document.createElement("td");
    sizeCell.textContent = formatBytes(file.size);
    const dateCell = document.createElement("td");
    dateCell.textContent = file.modified;
    const actionCell = document.createElement("td");
    const play = document.createElement("button");
    play.className = "play-button";
    play.textContent = "用 VLC 播放";
    play.addEventListener("click", () => openRecording(file.name));
    actionCell.append(play);
    row.append(nameCell, typeCell, sizeCell, dateCell, actionCell);
    body.append(row);
  }
}

async function refresh() {
  try {
    const status = await api("/api/status");
    $("#serverDot").classList.add("ok");
    $("#vlcDot").classList.toggle("ok", status.vlcFound);
    $("#vlcDot").classList.toggle("error", !status.vlcFound);
    $("#vlcStatus").textContent = status.vlcFound ? "VLC 已就绪" : "未找到 VLC";
    $("#recordingsPath").textContent = status.recordingsDir;
    renderConfig(status.config, status.boardCommand);
    renderJob("main", status.jobs.main);
    renderJob("sub", status.jobs.sub);
    renderFiles(status.files);
    firstLoad = false;
  } catch (error) {
    $("#serverDot").classList.remove("ok");
    $("#serverDot").classList.add("error");
    if (firstLoad) showToast(error.message, true);
  }
}

async function openStream(stream) {
  try {
    await postJson("/api/vlc/open", { stream });
    showToast(`${stream === "main" ? "主" : "子"}码流 VLC 已启动`);
  } catch (error) {
    showToast(error.message, true);
  }
}

async function startRecording(stream) {
  try {
    const result = await postJson("/api/record/start", { stream });
    showToast(`开始录制 60 秒：${result.file}`);
    await refresh();
  } catch (error) {
    showToast(error.message, true);
  }
}

async function openRecording(name) {
  try {
    await postJson("/api/files/open", { name });
    showToast("已交给 VLC 播放");
  } catch (error) {
    showToast(error.message, true);
  }
}

$$(".tab").forEach((tab) => {
  tab.addEventListener("click", () => {
    $$(".tab").forEach((item) => item.classList.toggle("active", item === tab));
    $$(".tab-page").forEach((page) => page.classList.remove("active"));
    $(`#${tab.dataset.tab}Page`).classList.add("active");
  });
});

$$("[data-open-stream]").forEach((button) => {
  button.addEventListener("click", () => openStream(button.dataset.openStream));
});

$$("[data-record-stream]").forEach((button) => {
  button.addEventListener("click", () => startRecording(button.dataset.recordStream));
});

$("#openBoth").addEventListener("click", async () => {
  await openStream("main");
  setTimeout(() => openStream("sub"), 350);
});

$("#settingsForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  try {
    const result = await postJson("/api/config", readForm());
    firstLoad = true;
    await refresh();
    showToast(result.message + "；请到 Ubuntu 重新编译");
  } catch (error) {
    showToast(error.message, true);
  }
});

$("#resetDefaults").addEventListener("click", () => {
  fillForm(defaults);
  showToast("已恢复表单默认值，点击保存后生效");
});

$("#copyCommand").addEventListener("click", async () => {
  try {
    await navigator.clipboard.writeText($("#boardCommand").textContent);
    showToast("开发板命令已复制");
  } catch {
    showToast("浏览器未允许剪贴板，请手动选择命令", true);
  }
});

$("#openFolder").addEventListener("click", async () => {
  try {
    await postJson("/api/folder/open", {});
  } catch (error) {
    showToast(error.message, true);
  }
});

$("#importButton").addEventListener("click", () => $("#importFile").click());
$("#importFile").addEventListener("change", async (event) => {
  const file = event.target.files[0];
  if (!file) return;
  try {
    showToast(`正在导入 ${file.name}`);
    const dataUrl = await new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result);
      reader.onerror = () => reject(new Error("读取文件失败"));
      reader.readAsDataURL(file);
    });
    await postJson("/api/import", {
      name: encodeURIComponent(file.name),
      data: dataUrl.split(",", 2)[1]
    });
    showToast(`已导入：${file.name}`);
    await refresh();
  } catch (error) {
    showToast(error.message, true);
  } finally {
    event.target.value = "";
  }
});

refresh();
setInterval(refresh, 1000);
