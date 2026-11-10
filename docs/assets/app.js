import { createFlasher } from "./flasher-core.js";

const el = (id) => document.getElementById(id);
const manifestURL = new URL("../firmware/manifest.json", import.meta.url);
const supportsSerial = window.isSecureContext && Boolean(navigator.serial?.requestPort);
let prepared = null;
let connected = false;
let busy = false;
let action = "prepare";
let activePort = null;
let installed = false;
let expectedDisconnect = false;
let currentPhase = "idle";
const logLines = ["M5 Prom web installer ready."];

function log(value) {
  const message = typeof value === "string" ? value : value?.message || String(value);
  for (const line of message.replace(/\r/g, "").split("\n")) {
    if (line) logLines.push(`[${new Date().toLocaleTimeString([], { hour12: false })}] ${line}`);
  }
  if (logLines.length > 600) logLines.splice(0, logLines.length - 600);
  el("install-log").textContent = `${logLines.join("\n")}\n`;
  el("install-log").scrollTop = el("install-log").scrollHeight;
}

function status(title, description, { tone = "ready", icon = "↗", badge = "Ready" } = {}) {
  el("state-title").textContent = title;
  el("state-description").textContent = description;
  el("state-icon").textContent = icon;
  el("state-panel").dataset.tone = tone;
  el("status-pill").dataset.tone = tone;
  el("status-pill-text").textContent = badge;
}

function stages(files, connect, install) {
  for (const [name, value] of [["files", files], ["connect", connect], ["install", install]]) {
    const item = el(`stage-${name}`);
    item.dataset.status = value || "";
    item.firstElementChild.textContent = value === "done" ? "✓" : String(["files", "connect", "install"].indexOf(name) + 1);
  }
}

function setAction(next, label, enabled = true) {
  action = next;
  el("primary-label").textContent = label;
  el("primary-action").disabled = !enabled;
  el("disconnect-action").hidden = !connected || busy;
  el("manual-mode").disabled = busy || connected;
}

function showProgress(label, percent, detail) {
  const safe = Math.max(0, Math.min(100, Number(percent) || 0));
  el("progress-area").hidden = false;
  el("progress-label").textContent = label;
  el("install-progress").value = safe;
  el("progress-percent").textContent = `${Math.floor(safe)}%`;
  if (detail) el("progress-detail").textContent = detail;
}

function progress(event) {
  const amount = Number(event.overallPercent ?? event.percent ?? 0);
  if (event.phase === "preparing") {
    showProgress("Checking firmware", amount, event.name ? `Checking ${event.name.toLowerCase()}.` : "Downloading and checking the matched firmware files.");
  } else if (event.phase === "verifying") {
    showProgress("Verifying on the Cardputer", Math.min(amount, 99), "Keep the USB cable connected until verification is complete.");
  } else if (event.phase === "flashing") {
    showProgress(event.name ? `Writing ${event.name.toLowerCase()}` : "Writing firmware", Math.min(amount, 99), "Keep the USB cable connected. Verification follows each image.");
  }
}

function engineState(event) {
  currentPhase = event.phase;
  if (event.message) log(event.message);
  if (event.phase === "connecting") {
    status("Connecting to your Cardputer", "Entering the bootloader and checking the connected device.", { tone: "pending", badge: "Connecting", icon: "↔" });
    stages("done", "active", "");
  } else if (event.phase === "flashing") {
    status("Installing M5 Prom", "Keep the Cardputer connected while the firmware is written and verified.", { tone: "pending", badge: "Installing", icon: "↓" });
    stages("done", "done", "active");
  } else if (event.phase === "verifying") {
    status("Verifying your installation", "Checking the data written to the Cardputer before reporting success.", { tone: "pending", badge: "Verifying", icon: "✓" });
  } else if (event.phase === "rebooting") {
    status("Firmware verified", "Restarting the Cardputer into M5 Prom…", { badge: "Verified", icon: "✓" });
  }
}

const flasher = createFlasher({ onLog: log, onProgress: progress, onState: engineState });

function browserNotice() {
  el("browser-note").hidden = supportsSerial;
  if (!window.isSecureContext) {
    el("browser-note-title").textContent = "Open the HTTPS version of this page";
    el("browser-note-text").textContent = "USB installation requires a secure page. Use the published GitHub Pages URL, or localhost when developing.";
  }
}

function ready(message) {
  busy = false;
  stages("done", "active", "");
  el("progress-area").hidden = true;
  if (supportsSerial) {
    status("Ready when you are", message || "Connect your Cardputer with a USB-C data cable, then choose its USB serial port.", { icon: "↗", badge: "Files verified" });
    setAction("connect", "Connect Cardputer");
    el("action-hint").textContent = "Desktop Chrome or Edge · USB-C data cable";
  } else {
    status("Firmware ready. Switch browsers to install.", "Open this page in desktop Chrome or Edge to use its USB serial connection.", { tone: "warning", icon: "i", badge: "Browser needed" });
    setAction("connect", "Open in Chrome or Edge", false);
    el("action-hint").textContent = "This browser does not offer the required USB serial access.";
  }
  browserNotice();
}

function renderFirmware(bundle) {
  const metadata = bundle.application || {};
  const manifest = bundle.manifest;
  el("build-label").textContent = manifest.buildDate || metadata.buildDate || manifest.releaseLabel;
  el("app-version").textContent = metadata.version || manifest.version || "Unspecified";
  el("idf-version").textContent = metadata.idfVersion || manifest.idfVersion || "Unspecified";
  el("verification-label").textContent = "SHA-256 · all 3 files";
  el("firmware-files").replaceChildren();
  for (const image of bundle.images) {
    const row = document.createElement("div");
    row.className = "firmware-file";
    const link = document.createElement("a");
    link.href = new URL(image.file, manifestURL).href;
    link.textContent = `${image.name} ↓`;
    link.download = image.file.split("/").pop();
    link.title = `SHA-256: ${image.sha256}`;
    const size = document.createElement("small");
    size.textContent = `0x${image.offset.toString(16).toUpperCase().padStart(5, "0")} · ${(image.size / 1024).toFixed(1)} KB`;
    row.append(link, size);
    el("firmware-files").append(row);
  }
}

async function prepare() {
  if (busy) return;
  busy = true;
  prepared = null;
  installed = false;
  stages("active", "", "");
  status("Preparing your firmware", "Downloading and checking all three files before connecting to your Cardputer.", { tone: "pending", icon: "↓", badge: "Checking files" });
  setAction("prepare", "Preparing firmware…", false);
  showProgress("Checking firmware", 0, "The firmware is checked before anything is written.");
  const controller = new AbortController();
  const deadline = setTimeout(() => controller.abort(), 60000);
  try {
    prepared = await flasher.prepare(manifestURL.href, { signal: controller.signal });
    renderFirmware(prepared);
    log(`Prepared ${prepared.images.length} verified images. Application build: ${prepared.manifest.buildDate}, version ${prepared.application?.version || prepared.manifest.version}.`);
    ready();
  } catch (error) {
    busy = false;
    log(`Preparation failed: ${error.message}`);
    status("Firmware could not be prepared", "Nothing has been written. Check your connection and try downloading the files again.", { tone: "error", badge: "Download failed", icon: "!" });
    el("progress-area").hidden = true;
    el("verification-label").textContent = "Not verified";
    setAction("prepare", "Retry firmware download");
    el("action-hint").textContent = "Installation stays disabled until every file passes its checks.";
    browserNotice();
  } finally {
    clearTimeout(deadline);
  }
}

async function cleanup() {
  expectedDisconnect = true;
  try { await flasher.disconnect(); } catch (error) { log(`Port cleanup: ${error.message}`); }
  connected = false;
  activePort = null;
  expectedDisconnect = false;
}

async function connect() {
  if (busy || !prepared || !supportsSerial) return;
  busy = true;
  installed = false;
  setAction("connect", "Choose your Cardputer…", false);
  try {
    // Keep requestPort in the original click handler before any awaited work.
    // Dependencies and firmware are prepared before this button is enabled.
    activePort = await navigator.serial.requestPort();
    setAction("connect", "Connecting…", false);
    const device = await flasher.connect(activePort, { manual: el("manual-mode").checked });
    connected = true;
    busy = false;
    stages("done", "done", "active");
    status("Your Cardputer is connected", `${device.chip} · ${device.flashSize || "8 MB"}. Ready to install the verified M5 Prom firmware.`, { badge: "Connected", icon: "✓" });
    setAction("install", "Install M5 Prom");
    el("action-hint").textContent = "Keep the USB cable connected until installation finishes.";
    el("recovery-note").hidden = true;
  } catch (error) {
    await cleanup();
    busy = false;
    if (error.name === "NotFoundError" || error.name === "AbortError" || error.code === "CANCELLED") {
      log("Connection cancelled. No firmware was written.");
      ready("No device selected. Connect again when your Cardputer is ready.");
      el("recovery-note").hidden = false;
      return;
    }
    log(`Connection failed: ${error.message}`);
    const mismatch = /CHIP|FLASH_SIZE|SECURITY|REVISION/.test(error.code || "");
    const needsBootMode = error.code === "BOOT_MODE_REQUIRED";
    status(mismatch ? "This device cannot use this firmware" : needsBootMode ? "Could not connect to the bootloader" : "Connection checks did not complete", needsBootMode ? "No firmware was written. Check the USB cable, close other serial applications and try download mode below." : `${error.message} No firmware was written.`, { tone: "error", badge: mismatch ? "Device mismatch" : "Connection failed", icon: "!" });
    stages("done", "active", "");
    el("recovery-note").hidden = !needsBootMode;
    setAction("connect", "Connect again");
    el("action-hint").textContent = "Only the supported ESP32-S3 Cardputer can be installed.";
  }
}

async function install() {
  if (busy || !connected || !prepared || installed) return;
  busy = true;
  setAction("install", "Installing…", false);
  showProgress("Writing firmware", 0, "Keep the USB cable connected until verification finishes.");
  try {
    const result = await flasher.install();
    if (!result?.verified) throw new Error("The device did not confirm firmware verification.");
    installed = true;
    showProgress("Installation verified", 100, "All three images were written and verified on the Cardputer.");
    stages("done", "done", "done");
    expectedDisconnect = true;
    let restartWarning = false;
    try { await flasher.reboot(); } catch (error) { restartWarning = true; log(`Firmware is verified; automatic restart did not complete: ${error.message}`); }
    await cleanup();
    busy = false;
    status("M5 Prom is installed", restartWarning ? "The firmware is verified. Press RESET once, without holding G0, to start M5 Prom." : "Your firmware is verified. If M5 Prom has not started, press RESET once without holding G0.", { badge: "Installed", icon: "✓" });
    setAction("connect", "Connect another Cardputer");
    el("action-hint").textContent = "You can now disconnect the USB cable.";
    log("Installation complete. All device verification checks passed.");
  } catch (error) {
    log(`Installation failed${error.partialWrite ? " after programming may have started" : ""}: ${error.message}`);
    await cleanup();
    busy = false;
    installed = false;
    const beforeWrite = error.partialWrite === false;
    status("Installation did not complete", beforeWrite ? `${error.message} No firmware was written. Reconnect to try again.` : "The firmware may be partially written. Put the Cardputer in download mode, reconnect, and explicitly start the installation again.", { tone: "error", badge: "Install failed", icon: "!" });
    el("progress-label").textContent = "Installation stopped";
    el("progress-detail").textContent = "No automatic write retry was performed. The installation has not been marked successful.";
    el("recovery-note").hidden = false;
    el("log-details").open = true;
    setAction("connect", "Reconnect Cardputer");
    el("action-hint").textContent = "The ROM bootloader can be used to install again.";
  }
}

el("primary-action").addEventListener("click", () => {
  if (action === "prepare") void prepare();
  else if (action === "connect") void connect();
  else if (action === "install") void install();
});
el("disconnect-action").addEventListener("click", async () => {
  if (busy) return;
  busy = true;
  setAction("connect", "Disconnecting…", false);
  await cleanup();
  ready("Disconnected. No installation was started.");
});
el("download-log").addEventListener("click", () => {
  const blob = new Blob([`${logLines.join("\n")}\n`], { type: "text/plain;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = `M5Prom-Flasher-${new Date().toISOString().replace(/[:.]/g, "-")}.log`;
  document.body.append(link);link.click();link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});
window.addEventListener("beforeunload", (event) => {
  if (!busy || !["flashing", "verifying"].includes(currentPhase)) return;
  event.preventDefault();
  event.returnValue = "";
});
if (navigator.serial?.addEventListener) navigator.serial.addEventListener("disconnect", (event) => {
  if (expectedDisconnect || !activePort || (event.target !== activePort && event.port !== activePort)) return;
  log("The selected USB serial device disconnected.");
  if (busy || installed) return; // The active operation must settle its own outcome.
  connected = false;
  void cleanup().then(() => ready("The Cardputer disconnected. Reconnect it to continue."));
});

// GitHub project pages reveal their repository in the first path segment.
// No repository name is guessed when previewing locally or on a custom domain.
if (/^[a-z0-9-]+\.github\.io$/i.test(location.hostname)) {
  const owner = location.hostname.split(".")[0];
  const repo = location.pathname.split("/").filter(Boolean)[0];
  const url = `https://github.com/${encodeURIComponent(owner)}/${encodeURIComponent(repo || `${owner}.github.io`)}`;
  document.querySelectorAll(".repo-link").forEach((link) => { link.href = url; });
}
browserNotice();
void prepare();
