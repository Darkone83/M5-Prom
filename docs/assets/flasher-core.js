import { ESPLoader, Transport, md5 } from "./vendor/esptool-js.js";

export const ESPTOOL_VERSION = "0.7.1";
export const DEFAULT_MANIFEST_URL = new URL("../firmware/manifest.json", import.meta.url).href;
const FLASH_BYTES = 8 * 1024 * 1024;
const FILES = Object.freeze([
  { role: "bootloader", file: "bootloader.bin", offset: 0, limit: 0x8000 },
  { role: "partitions", file: "partition-table.bin", offset: 0x8000, limit: 0x1000 },
  { role: "application", file: "cardputer_xbox_eeprom.bin", offset: 0x10000, limit: FLASH_BYTES - 0x10000 },
]);
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const hex = (bytes) => Array.from(bytes, (value) => value.toString(16).padStart(2, "0")).join("");
const view = (bytes) => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

export class FlasherError extends Error {
  constructor(code, message, { cause, partialWrite = false } = {}) {
    super(message);
    this.name = code === "CANCELLED" ? "AbortError" : "FlasherError";
    this.code = code;
    this.partialWrite = partialWrite;
    if (cause !== undefined) this.cause = cause;
  }
}
const fail = (code, message) => { throw new FlasherError(code, message); };
const cancelled = (signal) => {
  if (signal?.aborted) fail("CANCELLED", "Operation cancelled.");
};

/* esptool-js 0.7.1 releases its writer only after a successful write. A USB
 * failure must release that lock too, otherwise disconnect can wait forever.
 * Protocol framing remains the upstream implementation. */
export class ManagedTransport extends Transport {
  constructor(port, { checkCancelled = () => {}, sleep = delay } = {}) {
    super(port, false);
    this.checkCancelled = checkCancelled;
    this.sleep = sleep;
  }
  async write(data) {
    this.checkCancelled();
    if (!this.device.writable) fail("PORT_CLOSED", "The USB serial connection is closed.");
    const writer = this.device.writable.getWriter();
    try {
      await writer.write(this.slipWriter(data));
    } finally {
      writer.releaseLock();
    }
    this.checkCancelled();
  }
  async setSignals(dtr, rts) { this.checkCancelled(); return super.setSignals(dtr, rts); }
  async setDTR(value) { this.checkCancelled(); return super.setDTR(value); }
  async setRTS(value) { this.checkCancelled(); return super.setRTS(value); }
  async waitForUnlock() {
    const deadline = Date.now() + 4000;
    while (this.device.readable?.locked || this.device.writable?.locked) {
      if (Date.now() >= deadline) fail("DISCONNECT_FAILED", "USB streams did not unlock. Unplug and reconnect the device.");
      await this.sleep(25);
    }
  }
}

function textField(bytes, offset, length) {
  const field = bytes.subarray(offset, offset + length);
  const zero = field.indexOf(0);
  return new TextDecoder("utf-8", { fatal: true }).decode(zero < 0 ? field : field.subarray(0, zero));
}
function imageHeader(bytes, name) {
  if (bytes.length < 32 || bytes[0] !== 0xe9 || bytes[1] < 1 || bytes[1] > 16 || view(bytes).getUint16(12, true) !== 9)
    fail("INVALID_IMAGE", `${name} is not an ESP32-S3 firmware image.`);
  if (bytes[2] !== 2 || bytes[3] !== 0x3f)
    fail("INVALID_IMAGE", `${name} does not use the supplied DIO / 8 MB / 80 MHz settings.`);
  return { minRevision: view(bytes).getUint16(15, true), maxRevision: view(bytes).getUint16(17, true) };
}
function appMetadata(bytes) {
  if (bytes.length < 256 || view(bytes).getUint32(32, true) !== 0xabcd5432)
    fail("INVALID_IMAGE", "The application has no valid ESP-IDF application descriptor.");
  const rawDate = textField(bytes, 128, 16);
  const match = /^([A-Z][a-z]{2})\s+(\d{1,2})\s+(\d{4})$/.exec(rawDate);
  const months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
  const month = match ? months.indexOf(match[1]) + 1 : 0;
  if (!month) fail("INVALID_IMAGE", "The application build date is invalid.");
  return Object.freeze({
    version: textField(bytes, 48, 32), projectName: textField(bytes, 80, 32),
    buildDate: `${match[3]}-${String(month).padStart(2, "0")}-${match[2].padStart(2, "0")}`,
    buildTime: textField(bytes, 112, 16), idfVersion: textField(bytes, 144, 32),
  });
}
function partitionLayout(bytes, images, hashMD5) {
  const partitions = [];
  let checkedMD5 = false;
  for (let position = 0; position + 32 <= bytes.length; position += 32) {
    const record = bytes.subarray(position, position + 32);
    const data = view(record);
    const magic = data.getUint16(0, true);
    if (magic === 0xffff) break;
    if (magic === 0xebeb) {
      if (checkedMD5 || hex(record.subarray(16)) !== hashMD5(bytes.subarray(0, position)))
        fail("PARTITION_LAYOUT", "The partition table checksum is invalid.");
      checkedMD5 = true;
      continue;
    }
    if (magic !== 0x50aa || checkedMD5) fail("PARTITION_LAYOUT", "The ESP-IDF partition table is invalid.");
    const partition = { type: record[2], subtype: record[3], offset: data.getUint32(4, true), size: data.getUint32(8, true) };
    if (!partition.size || partition.offset < 0x9000 || partition.offset + partition.size > FLASH_BYTES ||
        partition.offset % 0x1000 || partition.size % 0x1000)
      fail("PARTITION_LAYOUT", "The partition table contains an invalid flash range.");
    if (partitions.some((other) => partition.offset < other.offset + other.size && other.offset < partition.offset + partition.size))
      fail("PARTITION_LAYOUT", "The partition table contains overlapping partitions.");
    partitions.push(partition);
  }
  const factory = partitions.filter((part) => part.type === 0 && part.subtype === 0);
  const nvs = partitions.filter((part) => part.type === 1 && part.subtype === 2);
  if (!checkedMD5 || factory.length !== 1 || factory[0].offset !== 0x10000 || !nvs.length)
    fail("PARTITION_LAYOUT", "The supplied table must contain NVS and one factory application at 0x10000.");
  if (Math.ceil(images[2].size / 0x1000) * 0x1000 > factory[0].size)
    fail("PARTITION_LAYOUT", "The application does not fit in the supplied factory partition.");
  for (const image of images) {
    const end = image.offset + Math.ceil(image.size / 0x1000) * 0x1000;
    for (const part of partitions.filter((entry) => entry.type !== 0)) {
      if (image.offset < part.offset + part.size && part.offset < end)
        fail("PARTITION_LAYOUT", "An image erase range would overlap NVS or another data partition.");
    }
  }
  return factory[0];
}
function manifestFields(input) {
  if (!input || input.schemaVersion !== 1 || input.name !== "M5 Prom" || input.chip !== "ESP32-S3" ||
      input.flashMode !== "dio" || input.flashSize !== "8MB" || input.flashFreq !== "80m" ||
      typeof input.version !== "string" || !input.version || input.version.length > 32 ||
      !Array.isArray(input.images) || input.images.length !== 3)
    fail("MANIFEST_INVALID", "This is not a supported M5 Prom ESP32-S3 firmware manifest.");
  const images = FILES.map((policy) => {
    const matches = input.images.filter((image) => image?.file === policy.file);
    if (matches.length !== 1) fail("MANIFEST_INVALID", "The manifest must contain the three matched firmware files exactly once.");
    const image = matches[0];
    if (image.offset !== policy.offset || !Number.isSafeInteger(image.size) || image.size < 32 || image.size > policy.limit ||
        typeof image.sha256 !== "string" || !/^[a-fA-F0-9]{64}$/.test(image.sha256) ||
        typeof image.name !== "string" || !image.name || image.name.length > 80 ||
        (image.role !== undefined && image.role !== policy.role))
      fail("MANIFEST_INVALID", `Invalid address, size, identity, or checksum for ${policy.file}.`);
    return Object.freeze({ name: image.name, role: policy.role, file: policy.file, offset: policy.offset, size: image.size, sha256: image.sha256.toLowerCase() });
  });
  const manifest = Object.freeze({
    schemaVersion: 1, name: input.name, chip: input.chip, version: input.version,
    releaseLabel: typeof input.releaseLabel === "string" ? input.releaseLabel : input.version,
    buildDate: input.buildDate, buildTime: input.buildTime, idfVersion: input.idfVersion,
    flashMode: "dio", flashSize: "8MB", flashFreq: "80m", images: Object.freeze(images),
  });
  return { manifest, images };
}
async function readResponse(response, maximum, signal, exact = false) {
  if (!response.ok) fail("DOWNLOAD_FAILED", `Firmware download failed (HTTP ${response.status}).`);
  const declared = response.headers?.get("content-length");
  const encoding = response.headers?.get("content-encoding");
  // Fetch exposes decoded bytes; a compressed response's Content-Length
  // describes its encoded transfer, not the firmware image length.
  if ((!encoding || encoding.toLowerCase() === "identity") && declared &&
      (Number(declared) > maximum || (exact && Number(declared) !== maximum)))
    fail("LENGTH_MISMATCH", "The server returned an unexpected file length.");
  if (!response.body?.getReader) {
    const bytes = new Uint8Array(await response.arrayBuffer());
    cancelled(signal);
    if (bytes.length > maximum || (exact && bytes.length !== maximum)) fail("LENGTH_MISMATCH", "The downloaded file has an unexpected length.");
    return bytes;
  }
  const reader = response.body.getReader();
  const bytes = new Uint8Array(maximum);
  let used = 0;
  try {
    while (true) {
      cancelled(signal);
      const { done, value } = await reader.read();
      if (done) break;
      if (used + value.length > maximum) fail("LENGTH_MISMATCH", "The downloaded file exceeds its expected length.");
      bytes.set(value, used); used += value.length;
    }
    cancelled(signal);
    if (exact && used !== maximum) fail("LENGTH_MISMATCH", "The downloaded file is incomplete.");
    return used === maximum ? bytes : bytes.slice(0, used);
  } catch (error) {
    try { await reader.cancel(); } catch { /* Preserve the original download error. */ }
    throw error;
  } finally { reader.releaseLock(); }
}

export class FlasherCore {
  constructor({ onLog = () => {}, onProgress = () => {}, onState = () => {}, dependencies = {} } = {}) {
    this._callbacks = { onLog, onProgress, onState };
    this._fetch = dependencies.fetch ?? globalThis.fetch?.bind(globalThis);
    this._crypto = dependencies.crypto ?? globalThis.crypto;
    this._md5 = dependencies.md5 ?? md5;
    this._sleep = dependencies.sleep ?? delay;
    this._manifestURL = dependencies.manifestURL ?? DEFAULT_MANIFEST_URL;
    this._baseURL = dependencies.baseURL ?? globalThis.document?.baseURI ?? this._manifestURL;
    this._transportFactory = dependencies.transportFactory ?? ((port, options) => new ManagedTransport(port, options));
    this._loaderFactory = dependencies.loaderFactory ?? ((options) => new ESPLoader(options));
    this._prepared = null; this._active = null; this._transport = null; this._loader = null;
    this._connected = false; this._device = null; this._partialWrite = false;
  }
  _emit(name, value) { try { this._callbacks[name](value); } catch { /* UI callbacks cannot interrupt programming. */ } }
  _state(phase, message, detail = {}) { this._emit("onState", Object.freeze({ phase, message, ...detail })); }
  _check() { cancelled(this._active?.controller.signal); }
  async _sha(bytes) {
    if (!this._crypto?.subtle) fail("CRYPTO_UNAVAILABLE", "Secure browser hashing is unavailable. Open the HTTPS site in a supported browser.");
    return hex(new Uint8Array(await this._crypto.subtle.digest("SHA-256", bytes)));
  }
  _operation(kind, signal, work) {
    if (this._active) return Promise.reject(new FlasherError("BUSY", "Another flasher operation is still running."));
    const controller = new AbortController();
    const abort = () => controller.abort();
    if (signal?.aborted) controller.abort();
    signal?.addEventListener("abort", abort, { once: true });
    const active = { kind, controller, promise: null };
    this._active = active;
    active.promise = (async () => {
      try { cancelled(controller.signal); return await work(controller.signal); }
      catch (original) {
        const error = controller.signal.aborted
          ? new FlasherError("CANCELLED", this._partialWrite ? "Installation stopped. The device may be partly written; reconnect before starting a new installation." : "Operation cancelled.", { cause: original, partialWrite: this._partialWrite })
          : original instanceof FlasherError ? original
            : new FlasherError(kind === "prepare" ? "DOWNLOAD_FAILED" : kind === "install" ? "FLASH_FAILED" : kind === "reboot" ? "REBOOT_FAILED" : "BOOT_MODE_REQUIRED", original?.message || "The operation failed.", { cause: original, partialWrite: this._partialWrite });
        error.partialWrite ||= this._partialWrite;
        if (kind === "prepare") this._prepared = null;
        if ((kind === "connect" || kind === "install") && this._transport) {
          try { await this._close(); } catch (closeError) { this._emit("onLog", `USB cleanup: ${closeError.message}`); }
        }
        this._state(error.code === "CANCELLED" ? "cancelled" : "error", error.message, { code: error.code, partialWrite: error.partialWrite });
        throw error;
      } finally {
        signal?.removeEventListener("abort", abort);
        if (this._active === active) this._active = null;
      }
    })();
    return active.promise;
  }
  prepare(manifestURL = this._manifestURL, { signal } = {}) {
    return this._operation("prepare", signal, async (activeSignal) => {
      this._prepared = null;
      this._partialWrite = false;
      this._state("preparing", "Checking the release manifest and all three firmware files.");
      const url = new URL(manifestURL, this._baseURL);
      if (url.href !== new URL(this._manifestURL).href)
        fail("MANIFEST_INVALID", "Only the embedded M5 Prom firmware manifest can be installed.");
      if (typeof this._fetch !== "function") fail("DOWNLOAD_FAILED", "Browser downloads are unavailable.");
      const fetchFile = (fileURL) => this._fetch(fileURL, { signal: activeSignal, cache: "no-store", credentials: "same-origin", redirect: "error" });
      const rawManifest = await readResponse(await fetchFile(url.href), 65536, activeSignal);
      let parsed;
      try { parsed = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(rawManifest)); }
      catch (cause) { throw new FlasherError("MANIFEST_INVALID", "The firmware manifest is invalid JSON.", { cause }); }
      const { manifest, images } = manifestFields(parsed);
      const buffers = [];
      const totalBytes = images.reduce((total, image) => total + image.size, 0);
      let completed = 0;
      for (let index = 0; index < images.length; index++) {
        const image = images[index];
        const bytes = await readResponse(await fetchFile(new URL(image.file, url).href), image.size, activeSignal, true);
        if (await this._sha(bytes) !== image.sha256) fail("HASH_MISMATCH", `${image.name} failed its SHA-256 integrity check. Reload the page and try the download again.`);
        cancelled(activeSignal);
        buffers.push(bytes); completed += image.size;
        this._emit("onProgress", Object.freeze({ phase: "preparing", imageIndex: index, ...image, completed: image.size, total: image.size, percent: 100, overallPercent: completed / totalBytes * 100 }));
      }
      const bootHeader = imageHeader(buffers[0], images[0].name);
      const appHeader = imageHeader(buffers[2], images[2].name);
      const application = appMetadata(buffers[2]);
      if (application.version !== manifest.version || application.projectName !== "cardputer_xbox_eeprom" ||
          (manifest.buildDate !== undefined && application.buildDate !== manifest.buildDate) ||
          (manifest.buildTime !== undefined && application.buildTime !== manifest.buildTime) ||
          (manifest.idfVersion !== undefined && application.idfVersion !== manifest.idfVersion))
        fail("VERSION_MISMATCH", "The application build metadata does not match the selected release manifest.");
      partitionLayout(buffers[1], images, this._md5);
      cancelled(activeSignal);
      const snapshot = Object.freeze({ manifest, images: manifest.images, totalBytes, application });
      this._prepared = { snapshot, buffers, headers: [bootHeader, appHeader] };
      this._state("ready", "All three firmware files passed their length and SHA-256 checks.", { release: manifest.releaseLabel });
      return snapshot;
    });
  }
  connect(port, { manual = false, signal } = {}) {
    return this._operation("connect", signal, async () => {
      this._partialWrite = false;
      if (!port || typeof port.open !== "function" || typeof port.close !== "function") fail("PORT_INVALID", "Select a USB serial port first.");
      await this._close();
      this._check();
      this._state("connecting", manual ? "Connecting to the ROM bootloader." : "Trying automatic ROM bootloader reset.", { manual });
      const checkCancelled = () => this._check();
      this._transport = this._transportFactory(port, { checkCancelled, sleep: this._sleep });
      const terminal = { clean() {}, write: (message) => this._emit("onLog", String(message)), writeLine: (message) => this._emit("onLog", String(message)) };
      /* The default 0.7.1 HardReset omits RTS assertion. Restore the official
       * esptool reset pulse through its supported reset-constructor API. */
      const hardReset = (transport, usbOtg) => ({ reset: async () => {
        this._check();
        await transport.setDTR(false);
        await transport.setRTS(true);
        await this._sleep(usbOtg ? 200 : 100);
        await transport.setRTS(false);
        if (usbOtg) await this._sleep(200);
      } });
      this._loader = this._loaderFactory({ transport: this._transport, baudrate: 115200, romBaudrate: 115200, terminal, debugLogging: false, enableTracing: false, resetConstructors: { hardReset } });
      // A failed block may already have changed flash. Do not retransmit it.
      this._loader.WRITE_BLOCK_ATTEMPTS = 1;
      if (typeof this._loader.command === "function") {
        const command = this._loader.command.bind(this._loader);
        this._loader.command = async (...arguments_) => { this._check(); const response = await command(...arguments_); this._check(); return response; };
      }
      let description;
      try { description = await this._loader.main(manual ? "no_reset" : "default_reset"); }
      catch (cause) {
        this._check();
        if (this._loader.chip && this._loader.chip.CHIP_NAME !== "ESP32-S3") fail("WRONG_CHIP", "This firmware requires an ESP32-S3 Cardputer.");
        if (this._loader.secureDownloadMode) fail("SECURITY_UNSUPPORTED", "This plaintext firmware cannot be installed in secure download mode.");
        throw new FlasherError("BOOT_MODE_REQUIRED", "The ROM bootloader did not connect. Use the BOOT-button instructions, then select the bootloader serial port.", { cause });
      }
      this._check();
      if (this._loader.chip?.CHIP_NAME !== "ESP32-S3") fail("WRONG_CHIP", "This firmware requires an ESP32-S3 Cardputer. No flash was written.");
      if (this._loader.IS_STUB !== true) fail("STUB_REQUIRED", "The RAM flasher stub did not start. Installation stopped to protect NVS.");
      let flashSize, revision;
      try {
        const security = await this._loader.getSecurityInfo();
        if (!security || !Number.isInteger(security.flashCryptCnt) || security.flashCryptCnt < 0 || !security.parsedFlags)
          fail("DEVICE_CHECK_FAILED", "The device security state could not be established.");
        let count = security.flashCryptCnt >>> 0, bits = 0;
        while (count) { bits += count & 1; count >>>= 1; }
        if (this._loader.secureDownloadMode || security.parsedFlags.SECURE_BOOT_EN || security.parsedFlags.SECURE_DOWNLOAD_ENABLE || bits % 2)
          fail("SECURITY_UNSUPPORTED", "This release is plaintext firmware; secure boot, encrypted flash, and secure download targets are unsupported.");
        const major = await this._loader.chip.getMajorChipVersion(this._loader);
        const minor = await this._loader.chip.getMinorChipVersion(this._loader);
        if (!Number.isInteger(major) || major < 0 || !Number.isInteger(minor) || minor < 0 || minor > 99)
          fail("DEVICE_CHECK_FAILED", "The ESP32-S3 revision could not be read.");
        revision = major * 100 + minor;
        flashSize = await this._loader.detectFlashSize();
        const capacity = typeof flashSize === "string" ? this._loader.flashSizeBytes(flashSize) : NaN;
        if (!Number.isSafeInteger(capacity) || capacity < FLASH_BYTES)
          fail("FLASH_SIZE_MISMATCH", "The detected flash must be at least 8 MB for this release.");
      } catch (cause) {
        this._check();
        if (cause instanceof FlasherError) throw cause;
        throw new FlasherError("DEVICE_CHECK_FAILED", "The device compatibility checks could not be completed. No flash was written.", { cause });
      }
      this._device = Object.freeze({ chip: "ESP32-S3", description, flashSize, revision, manual });
      this._checkRevision();
      this._check(); this._connected = true;
      this._state("connected", "ESP32-S3 connected; the RAM flasher stub and flash capacity are verified.", this._device);
      return this._device;
    });
  }
  _checkRevision() {
    if (!this._prepared || !this._device) return;
    if (this._prepared.headers.some((header) => this._device.revision < header.minRevision || this._device.revision > header.maxRevision))
      fail("CHIP_REVISION_MISMATCH", "This ESP32-S3 revision is outside the supplied firmware's supported range.");
  }
  install({ signal } = {}) {
    return this._operation("install", signal, async () => {
      this._partialWrite = false;
      const prepared = this._prepared;
      if (!prepared) fail("FIRMWARE_NOT_READY", "Prepare and verify the embedded release before installation.");
      if (!this._connected || !this._loader || this._loader.IS_STUB !== true)
        fail("NOT_CONNECTED", "Connect to the ESP32-S3 ROM bootloader and start its RAM stub first.");
      this._checkRevision();
      // Recheck private buffers before the first erase/program command.
      for (let index = 0; index < prepared.buffers.length; index++) {
        if (await this._sha(prepared.buffers[index]) !== prepared.snapshot.images[index].sha256)
          fail("HASH_MISMATCH", "The prepared firmware changed before installation. Download it again.");
        this._check();
      }
      let verifiedBytes = 0;
      for (let index = 0; index < prepared.buffers.length; index++) {
        this._check();
        const image = prepared.snapshot.images[index];
        const data = new Uint8Array(Math.ceil(image.size / 4) * 4).fill(0xff);
        data.set(prepared.buffers[index]);
        const expectedMD5 = this._md5(data);
        this._state("flashing", `Writing ${image.name}. Keep USB connected.`, { imageIndex: index, name: image.name });
        this._partialWrite = true;
        await this._loader.writeFlash({
          fileArray: [{ data, address: image.offset }], eraseAll: false, compress: true,
          // All supplied headers were checked as DIO/8MB/80m. Preserve them.
          flashMode: "keep", flashFreq: "keep", flashSize: "keep",
          reportProgress: (_fileIndex, written, total) => {
            this._check();
            const fraction = total ? Math.max(0, Math.min(1, written / total)) : 0;
            this._emit("onProgress", Object.freeze({ phase: "flashing", imageIndex: index, ...image, completed: Math.round(image.size * fraction), total: image.size, percent: fraction * 100, overallPercent: (verifiedBytes + image.size * fraction) / prepared.snapshot.totalBytes * 100 }));
            this._check();
          },
        });
        this._check();
        this._state("verifying", `Checking ${image.name} against flash readback.`, { imageIndex: index, name: image.name });
        const actualMD5 = await this._loader.flashMd5sum(image.offset, data.length);
        this._check();
        if (typeof actualMD5 !== "string" || actualMD5.toLowerCase() !== expectedMD5)
          fail("VERIFY_FAILED", `${image.name} did not match device readback. Installation stopped; reconnect before retrying.`);
        verifiedBytes += image.size;
        this._emit("onProgress", Object.freeze({ phase: "verifying", imageIndex: index, ...image, completed: image.size, total: image.size, percent: 100, overallPercent: verifiedBytes / prepared.snapshot.totalBytes * 100 }));
      }
      this._partialWrite = false;
      const result = Object.freeze({ verified: true, bytesWritten: verifiedBytes, images: 3 });
      this._state("installed", "All three images matched device readback. Installation is verified.", result);
      return result;
    });
  }
  reboot() {
    return this._operation("reboot", undefined, async () => {
      this._partialWrite = false;
      if (!this._connected || !this._loader) fail("NOT_CONNECTED", "There is no connected device to restart.");
      this._state("rebooting", "Restarting M5 Prom.");
      try { await this._loader.after("hard_reset"); }
      finally { this._connected = false; }
      return Object.freeze({ resetRequested: true });
    });
  }
  async _close() {
    this._connected = false; this._device = null;
    if (!this._transport) { this._loader = null; return; }
    try { await this._transport.disconnect(); }
    catch (cause) {
      const port = this._transport.device;
      if (!(cause?.name === "InvalidStateError" && !port?.readable && !port?.writable))
        throw new FlasherError("DISCONNECT_FAILED", "The USB port could not be released. Unplug and reconnect it.", { cause });
    }
    this._transport = null; this._loader = null;
  }
  async disconnect() {
    if (this._active) {
      const active = this._active;
      active.controller.abort();
      try { await active.promise; } catch { /* The active caller receives its error. */ }
    }
    await this._close();
    this._partialWrite = false;
    // Keep the checked embedded release available for a deliberate reconnect.
    this._state("disconnected", "USB serial port released.");
  }
}

export function createFlasher(options) { return new FlasherCore(options); }
