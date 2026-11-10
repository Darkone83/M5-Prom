/* Run: node tools/test_webflasher.mjs
 * Actual embedded binaries + actual engine + bounded serial/HTTP mocks.
 * No hardware and no browser permissions are used by this test. */
import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { createHash, webcrypto } from "node:crypto";
import { createFlasher, ManagedTransport } from "../docs/assets/flasher-core.js";
import { md5 } from "../docs/assets/vendor/esptool-js.js";

const firmwareDir = new URL("../docs/firmware/", import.meta.url);
const originalManifest = JSON.parse(await readFile(new URL("manifest.json", firmwareDir), "utf8"));
const originalTotal = originalManifest.images.reduce((total, image) => total + image.size, 0);
const originalFiles = new Map(await Promise.all(originalManifest.images.map(async (image) =>
  [image.file, new Uint8Array(await readFile(new URL(image.file, firmwareDir)))])));
const manifestURL = "https://m5.example/docs/firmware/manifest.json";
const digest = (type, bytes) => createHash(type).update(bytes).digest("hex");
const rejectCode = (code, partialWrite) => (error) => {
  assert.equal(error.code, code);
  if (partialWrite !== undefined) assert.equal(error.partialWrite, partialWrite);
  return true;
};

function fixture(settings = {}) {
  const manifest = structuredClone(originalManifest);
  const files = new Map(Array.from(originalFiles, ([name, bytes]) => [name, new Uint8Array(bytes)]));
  const calls = { downloads: [], writes: [], verifies: [], resets: [], closes: 0, signals: [], sleeps: [], states: [] };
  const port = { async open() {}, async close() {} };
  let loader;
  const engine = createFlasher({
    onState: (event) => calls.states.push(event),
    onProgress: settings.onProgress,
    dependencies: {
      manifestURL, baseURL: "https://m5.example/docs/", crypto: webcrypto,
      sleep: async (ms) => calls.sleeps.push(ms),
      fetch: async (url, { signal, redirect, cache }) => {
        assert.equal(redirect, "error"); assert.equal(cache, "no-store");
        if (signal.aborted) throw new DOMException("Aborted", "AbortError");
        calls.downloads.push(url);
        if (settings.waitForDownload) return new Promise((_resolve, reject) =>
          signal.addEventListener("abort", () => reject(new DOMException("Aborted", "AbortError")), { once: true }));
        const name = new URL(url).pathname.split("/").at(-1);
        if (settings.downloadFailure === name) return new Response("Missing", { status: 503 });
        const body = name === "manifest.json" ? JSON.stringify(manifest) : files.get(name);
        assert.ok(body !== undefined, `Unexpected download: ${url}`);
        const headers = settings.encodedResponse ? { "content-encoding": "gzip", "content-length": "17" } : {};
        return new Response(body, { headers });
      },
      transportFactory: (selectedPort) => {
        assert.equal(selectedPort, port);
        return {
          device: port,
          async disconnect() { calls.closes++; },
          async setDTR(value) { calls.signals.push(["DTR", value]); },
          async setRTS(value) { calls.signals.push(["RTS", value]); },
        };
      },
      loaderFactory: (options) => {
        assert.equal(options.baudrate, 115200);
        assert.equal(options.romBaudrate, 115200);
        const written = new Map();
        loader = {
          chip: {
            CHIP_NAME: settings.chip ?? "ESP32-S3",
            async getMajorChipVersion() { return Math.floor((settings.revision ?? 0) / 100); },
            async getMinorChipVersion() { return (settings.revision ?? 0) % 100; },
          },
          IS_STUB: settings.stub ?? true,
          secureDownloadMode: settings.secureDownload ?? false,
          async main(mode) { calls.resets.push(mode); if (settings.connectFailure) throw new Error("No sync"); return "ESP32-S3 (revision v0.0)"; },
          async getSecurityInfo() {
            if (settings.securityFailure) throw new Error("Unreadable efuse");
            return { flashCryptCnt: settings.flashCryptCnt ?? 0, parsedFlags: { SECURE_BOOT_EN: settings.secureBoot ?? false } };
          },
          async detectFlashSize() { return settings.flashSize ?? "8MB"; },
          flashSizeBytes(size) { return Number(size.replace("MB", "")) * 1024 * 1024; },
          async writeFlash(options_) {
            assert.equal(this.WRITE_BLOCK_ATTEMPTS, 1, "No upstream hidden block retries");
            assert.equal(options_.eraseAll, false);
            assert.equal(options_.compress, true);
            assert.equal(options_.flashMode, "keep");
            assert.equal(options_.flashFreq, "keep");
            assert.equal(options_.flashSize, "keep");
            assert.equal(options_.fileArray.length, 1);
            const { data, address } = options_.fileArray[0];
            assert.ok(data instanceof Uint8Array);
            calls.writes.push({ data: new Uint8Array(data), address });
            if (settings.writeFailure === calls.writes.length) throw new Error("USB disappeared after a block");
            options_.reportProgress(0, 0, data.length);
            options_.reportProgress(0, data.length / 2, data.length);
            options_.reportProgress(0, data.length, data.length);
            written.set(address, new Uint8Array(data));
          },
          async flashMd5sum(address, size) {
            calls.verifies.push(address);
            assert.equal(written.get(address)?.length, size);
            return settings.badMD5 === calls.verifies.length ? "0".repeat(32) : digest("md5", written.get(address));
          },
          async eraseFlash() { assert.fail("Whole-flash erase is forbidden"); },
          async after(mode) {
            assert.equal(mode, "hard_reset");
            if (settings.resetFailure) throw new Error("USB reset failed");
            await options.resetConstructors.hardReset(options.transport, true).reset();
          },
        };
        return loader;
      },
    },
  });
  return { engine, manifest, files, calls, port, get loader() { return loader; } };
}
async function preparedConnected(settings) {
  const state = fixture(settings);
  state.snapshot = await state.engine.prepare(manifestURL);
  await state.engine.connect(state.port);
  return state;
}

test("actual embedded build prepares; exact three images install and MD5 verify without NVS erase", async () => {
  const state = await preparedConnected();
  assert.equal(state.snapshot.application.version, originalManifest.version);
  assert.equal(state.snapshot.application.buildDate, originalManifest.buildDate);
  assert.equal(state.snapshot.application.projectName, "cardputer_xbox_eeprom");
  assert.ok(Object.isFrozen(state.snapshot));
  assert.throws(() => { state.snapshot.images[0].offset = 0x9000; }, TypeError);
  assert.equal("data" in state.snapshot.images[0], false);
  const installed = await state.engine.install();
  assert.deepEqual(installed, { verified: true, bytesWritten: originalTotal, images: 3 });
  assert.deepEqual(state.calls.writes.map((write) => write.address), [0, 0x8000, 0x10000]);
  assert.deepEqual(state.calls.verifies, [0, 0x8000, 0x10000]);
  for (let index = 0; index < 3; index++) {
    const write = state.calls.writes[index];
    assert.deepEqual(write.data, originalFiles.get(originalManifest.images[index].file));
    const erasedEnd = write.address + Math.ceil(write.data.length / 4096) * 4096;
    assert.ok(erasedEnd <= 0x9000 || write.address >= 0x10000, "NVS/PHY sectors are untouched");
  }
  assert.deepEqual(await state.engine.reboot(), { resetRequested: true });
  assert.deepEqual(state.calls.signals, [["DTR", false], ["RTS", true], ["RTS", false]]);
  assert.deepEqual(state.calls.sleeps, [200, 200]);
  await state.engine.disconnect();
  assert.equal(state.calls.closes, 1);
  // Disconnect retains the verified bytes, without another network download.
  await state.engine.connect(state.port, { manual: true });
  await state.engine.install();
  assert.equal(state.calls.downloads.length, 4);
  assert.deepEqual(state.calls.resets, ["default_reset", "no_reset"]);
  await state.engine.disconnect();
});

test("decoded gzip responses use actual decoded byte count and SHA, not compressed Content-Length", async () => {
  const state = fixture({ encodedResponse: true });
  assert.equal((await state.engine.prepare(manifestURL)).totalBytes, originalTotal);
});

for (const [label, mutate, code] of [
  ["unsupported manifest version", (state) => { state.manifest.schemaVersion = 2; }, "MANIFEST_INVALID"],
  ["wrong application address", (state) => { state.manifest.images[2].offset = 0x20000; }, "MANIFEST_INVALID"],
  ["wrong release version", (state) => { state.manifest.version = originalManifest.version === "wrong" ? "other" : "wrong"; }, "VERSION_MISMATCH"],
  ["arbitrary firmware URL", (state) => { state.manifest.images[0].file = "https://elsewhere.example/bootloader.bin"; }, "MANIFEST_INVALID"],
  ["hash mismatch", (state) => { state.files.get("bootloader.bin")[100] ^= 1; }, "HASH_MISMATCH"],
  ["truncated image", (state) => { state.files.set("bootloader.bin", state.files.get("bootloader.bin").slice(1)); }, "LENGTH_MISMATCH"],
  ["oversized image", (state) => { state.files.set("bootloader.bin", new Uint8Array(state.files.get("bootloader.bin").length + 1)); }, "LENGTH_MISMATCH"],
]) {
  test(`${label} refuses preparation and performs no device writes`, async () => {
    const state = fixture(); mutate(state);
    await assert.rejects(state.engine.prepare(manifestURL), rejectCode(code, false));
    assert.equal(state.calls.writes.length, 0);
    await assert.rejects(state.engine.install(), rejectCode("FIRMWARE_NOT_READY", false));
  });
}

test("download failure invalidates a previous verified snapshot", async () => {
  const settings = {};
  const state = fixture(settings);
  await state.engine.prepare(manifestURL);
  settings.downloadFailure = "cardputer_xbox_eeprom.bin";
  await assert.rejects(state.engine.prepare(manifestURL), rejectCode("DOWNLOAD_FAILED", false));
  await state.engine.connect(state.port);
  await assert.rejects(state.engine.install(), rejectCode("FIRMWARE_NOT_READY", false));
  assert.equal(state.calls.writes.length, 0);
});

test("cancelled replacement preparation cannot reuse an older release", async () => {
  const state = fixture();
  await state.engine.prepare(manifestURL);
  const controller = new AbortController(); controller.abort();
  await assert.rejects(state.engine.prepare(manifestURL, { signal: controller.signal }), rejectCode("CANCELLED", false));
  await assert.rejects(state.engine.install(), rejectCode("FIRMWARE_NOT_READY", false));
});

test("in-flight preparation cancellation settles and permits an explicit clean retry", async () => {
  const settings = { waitForDownload: true };
  const state = fixture(settings);
  const controller = new AbortController();
  const preparing = state.engine.prepare(manifestURL, { signal: controller.signal });
  await assert.rejects(state.engine.prepare(manifestURL), rejectCode("BUSY"));
  controller.abort();
  await assert.rejects(preparing, rejectCode("CANCELLED", false));
  settings.waitForDownload = false;
  assert.equal((await state.engine.prepare(manifestURL)).images.length, 3);
});

for (const [label, settings, code] of [
  ["wrong chip", { chip: "ESP32-C3" }, "WRONG_CHIP"],
  ["missing stub", { stub: false }, "STUB_REQUIRED"],
  ["small flash", { flashSize: "4MB" }, "FLASH_SIZE_MISMATCH"],
  ["unknown flash size", { flashSize: "unknown" }, "FLASH_SIZE_MISMATCH"],
  ["unsupported silicon revision", { revision: 100 }, "CHIP_REVISION_MISMATCH"],
  ["secure boot", { secureBoot: true }, "SECURITY_UNSUPPORTED"],
  ["encrypted flash", { flashCryptCnt: 7 }, "SECURITY_UNSUPPORTED"],
  ["unreadable security state", { securityFailure: true }, "DEVICE_CHECK_FAILED"],
  ["automatic reset failure", { connectFailure: true }, "BOOT_MODE_REQUIRED"],
]) {
  test(`${label} refuses installation before any write and releases the port`, async () => {
    const state = fixture(settings);
    await state.engine.prepare(manifestURL);
    await assert.rejects(state.engine.connect(state.port), rejectCode(code, false));
    assert.equal(state.calls.writes.length, 0);
    assert.equal(state.calls.closes, 1);
    await assert.rejects(state.engine.install(), rejectCode("NOT_CONNECTED", false));
  });
}

test("even encryption-counter popcount is disabled, and larger flash preserves the 8MB image header", async () => {
  const state = await preparedConnected({ flashCryptCnt: 3, flashSize: "16MB" });
  await state.engine.install();
  assert.equal(state.calls.writes[0].data[3], 0x3f);
  await state.engine.disconnect();
});

test("partial programming failure is not retried, skips remaining files, and releases the port", async () => {
  const state = await preparedConnected({ writeFailure: 2 });
  await assert.rejects(state.engine.install(), rejectCode("FLASH_FAILED", true));
  assert.equal(state.calls.writes.length, 2);
  assert.deepEqual(state.calls.verifies, [0]);
  assert.equal(state.calls.closes, 1);
  await assert.rejects(state.engine.install(), rejectCode("NOT_CONNECTED", false));
  assert.equal(state.calls.writes.length, 2);
});

test("device MD5 mismatch stops before the next image and never claims installation", async () => {
  const state = await preparedConnected({ badMD5: 1 });
  await assert.rejects(state.engine.install(), rejectCode("VERIFY_FAILED", true));
  assert.equal(state.calls.writes.length, 1);
  assert.equal(state.calls.closes, 1);
  assert.equal(state.calls.states.some((event) => event.phase === "installed"), false);
});

test("cancellation during programming stops at the current command and marks a possible partial write", async () => {
  const controller = new AbortController();
  const state = await preparedConnected({ onProgress: (event) => { if (event.phase === "flashing") controller.abort(); } });
  await assert.rejects(state.engine.install({ signal: controller.signal }), rejectCode("CANCELLED", true));
  assert.equal(state.calls.writes.length, 1);
  assert.equal(state.calls.verifies.length, 0);
  assert.equal(state.calls.closes, 1);
});

test("reboot failure leaves the successful installation result separate", async () => {
  const state = await preparedConnected({ resetFailure: true });
  assert.equal((await state.engine.install()).verified, true);
  await assert.rejects(state.engine.reboot(), rejectCode("REBOOT_FAILED", false));
  assert.equal(state.calls.writes.length, 3);
  await state.engine.disconnect();
  assert.equal(state.calls.closes, 1);
});

test("actual ManagedTransport releases the writer lock when USB write rejects", async () => {
  let released = 0;
  const port = { writable: { getWriter: () => ({
    async write() { throw new Error("USB disconnected"); },
    releaseLock() { released++; },
  }) } };
  const transport = new ManagedTransport(port);
  await assert.rejects(transport.write(new Uint8Array([1, 2, 3])), /USB disconnected/);
  assert.equal(released, 1);
  port.writable = null;
  await assert.rejects(transport.write(new Uint8Array([1])), rejectCode("PORT_CLOSED"));
});

test("local MD5 adapter matches Node for empty, short, binary, and offset views", () => {
  for (const size of [0, 1, 3, 55, 56, 63, 64, 65, 4096]) {
    const array = Uint8Array.from({ length: size + 12 }, (_, index) => index * 97 & 255);
    const bytes = array.subarray(7, 7 + size);
    assert.equal(md5(bytes), digest("md5", bytes));
  }
});
