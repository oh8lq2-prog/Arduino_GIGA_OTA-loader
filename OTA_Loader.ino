/**
 * Arduino Giga R1 — OTA loader
 *
 * Minimal sketch that runs once at power-on:
 *   1. Connect to WiFi (credentials in password_ssid.h)
 *   2. Download the .ota firmware package from the configured URL
 *   3. Decompress LZSS payload and flash it via the Portenta OTA bootloader
 *   4. Reset into the newly installed application sketch
 *
 * All user-tunable settings (WiFi, URLs, HTTPS, LAN vs remote) live in
 * password_ssid.h so the same loader.ino can be reused across projects.
 *
 * Build the .ota package for the main application:
 *   lzss.py --encode sketch.bin sketch.lzss
 *   bin2ota.py GIGA sketch.lzss sketch.ota
 *
 * Or load arduino-cloud-cli tool and run 
 *   "arduino-cloud-cli ota header-encode --file xxxxxxx.bin --fqbn arduino:mbed_giga:giga"
 *
 *
 * QSPI layout note:
 *   MBR partition 2 hosts the user FAT filesystem (/fs). Arduino_Portenta_OTA
 *   also uses that partition during update. The loader must fully unmount and
 *   deinit the block devices before ota.begin(), then remount only on failure.
 *
 *
 * Jari Ojala OH8LQ
 */

#include <Arduino_Portenta_OTA.h>
#include <ArduinoHttpClient.h>
#include <WiFi.h>
#include <WiFiSSLClient.h>
#include "QSPIFBlockDevice.h"
#include "MBRBlockDevice.h"
#include "FATFileSystem.h"
#include "password_ssid.h"

// --- QSPI block devices for user FAT on MBR partition 2 (/fs) ---
QSPIFBlockDevice root;
mbed::MBRBlockDevice part(&root, 2);
mbed::FATFileSystem fs("fs");


// =============================================================================
// QSPI / filesystem helpers
// =============================================================================

/**
 * Release the user FAT mount so Arduino_Portenta_OTA can claim the same QSPI
 * flash. fs.unmount() alone is often insufficient; root/part deinit is required
 * to avoid OtaStorageInit (-3) errors.
 */
static void otaReleaseUserQspiBeforePortentaOtaBegin() {
  int um = fs.unmount();
  if (um != 0) {
    Serial.print("OTA: fs.unmount returned ");
    Serial.println(um);
  }
  delay(80);
  part.deinit();
  root.deinit();
  delay(150);
}

/** Best-effort remount of user FAT after a failed OTA step (for diagnostics / retry). */
static void otaRemountUserFsAfterFail(const char* where) {
  (void)fs.unmount();
  part.deinit();
  root.deinit();
  delay(120);
  root.init();
  part.init();
  int m = fs.mount(&part);
  if (m != 0) {
    Serial.print("OTA: fs.mount ");
    Serial.print(m);
    Serial.print(" @ ");
    Serial.println(where);
  }
}


// =============================================================================
// OTA library error reporting
// =============================================================================

/** Print human-readable hints for Arduino_Portenta_OTA::begin() failure codes. */
static void otaPrintBeginFailure(Arduino_Portenta_OTA::Error err) {
  Serial.print("Arduino_Portenta_OTA::begin() failed: ");
  Serial.println((int)err);
  switch (err) {
    case Arduino_Portenta_OTA::Error::NoCapableBootloader:
      Serial.println("(-1) Update bootloader (OTA not supported).");
      break;
    case Arduino_Portenta_OTA::Error::CaStorageInit:
      Serial.println("(-8) WLAN/certificate QSPI partition will not mount.");
      Serial.println("    Run STM32H747_System -> WiFiFirmwareUpdater.");
      break;
    case Arduino_Portenta_OTA::Error::CaStorageOpen:
      Serial.println("(-9) /wlan/cacert.pem missing or not readable.");
      break;
    case Arduino_Portenta_OTA::Error::OtaStorageInit:
      Serial.println("(-3) OTA cannot mount update partition.");
      break;
    default:
      break;
  }
}

/** Decode negative return codes from download / decompress / mbed network layer. */
static void otaSerialExplainNegativeFwCode(int code) {
  if (code == -3005) {
    Serial.println("(-3005) NSAPI_ERROR_NO_SOCKET.");
  }
  if (code == static_cast<int>(Arduino_Portenta_OTA::Error::OtaHeaderLength)) {
    Serial.println("(-5) OTA header/length — wrong .ota or bin2ota input.");
  }
  if (code == static_cast<int>(Arduino_Portenta_OTA::Error::OtaHeaderCrc)) {
    Serial.println("(-6) OTA CRC mismatch.");
  }
  if (code == static_cast<int>(Arduino_Portenta_OTA::Error::OtaHeaterMagicNumber)) {
    Serial.println("(-7) OTA magic — use bin2ota.py GIGA.");
  }
  if (code == static_cast<int>(Arduino_Portenta_OTA::Error::OtaDownload)) {
    Serial.println("(-12) OTA download layer error.");
  }
}


// =============================================================================
// HTTP URL parsing and pre-download probe
// =============================================================================

/**
 * Split "http(s)://host/path" into scheme flag, hostname, and path.
 * Returns false if the URL does not start with http:// or https://.
 */
static bool otaParseHttpUrl(const char* url, bool* out_https, String* out_host, String* out_path) {
  String u(url);
  if (u.startsWith("https://")) {
    *out_https = true;
    u = u.substring(8);
  } else if (u.startsWith("http://")) {
    *out_https = false;
    u = u.substring(7);
  } else {
    return false;
  }
  int const slash = u.indexOf('/');
  if (slash < 0) {
    *out_host = u;
    *out_path = "/";
  } else {
    *out_host = u.substring(0, slash);
    *out_path = u.substring(slash);
  }
  if (out_path->length() == 0) {
    *out_path = "/";
  }
  return out_host->length() > 0;
}

/** Discard response body after a probe GET so the TCP socket is cleanly closed. */
static void otaProbeDrainResponse(HttpClient& http) {
  uint8_t buf[256];
  unsigned long const t0 = millis();
  size_t total = 0;
  const size_t kMaxBytes = 16384;
  const unsigned long kMaxMs = 10000;
  while (total < kMaxBytes && (millis() - t0) < kMaxMs) {
    int n = http.read(buf, sizeof(buf));
    if (n > 0) {
      total += (size_t)n;
      continue;
    }
    if (!http.connected()) {
      break;
    }
    delay(2);
  }
}

/** Issue GET, capture status code and Content-Length, then drain the body. */
static bool otaProbeHttpRun(HttpClient& http, const char* path, int* errOut, int* statusOut, long* lenOut) {
  int err = http.get(path);
  *errOut = err;
  if (err != 0) {
    return false;
  }
  *statusOut = http.responseStatusCode();
  *lenOut = http.contentLength();
  otaProbeDrainResponse(http);
  return true;
}

/**
 * Lightweight HEAD-like check: verify the firmware URL is reachable and returns
 * HTTP 200 before starting a long download. Catches redirects and 404 early.
 */
static bool otaProbeFirmwareHead(const char* url, bool const want_https) {
  bool url_https = false;
  String host;
  String path;
  if (!otaParseHttpUrl(url, &url_https, &host, &path)) {
    Serial.println("OTA probe: URL parse failed");
    return false;
  }
  if (url_https != want_https) {
    Serial.println("OTA probe: HTTPS setting does not match URL");
    return false;
  }

  int err = 0;
  int status = 0;
  long contentLen = -1;

  if (want_https) {
    WiFiSSLClient ssl;
    ssl.setTimeout(20000);
    HttpClient http(ssl, host.c_str(), 443);
    http.setHttpResponseTimeout(45000);
    http.setHttpWaitForDataDelay(25);
    (void)otaProbeHttpRun(http, path.c_str(), &err, &status, &contentLen);
    http.stop();
  } else {
    WiFiClient wifi;
    wifi.setTimeout(20000);
    HttpClient http(wifi, host.c_str(), 80);
    http.setHttpResponseTimeout(45000);
    http.setHttpWaitForDataDelay(25);
    (void)otaProbeHttpRun(http, path.c_str(), &err, &status, &contentLen);
    http.stop();
  }

  if (err != 0) {
    Serial.print("OTA probe: GET failed err=");
    Serial.println(err);
    return false;
  }

  Serial.print("OTA probe: HTTP status ");
  Serial.println(status);

  if (status >= 300 && status < 400) {
    Serial.println("OTA: redirect — set OTA_FIRMWARE_USE_REMOTE_HTTPS to 1 in password_ssid.h.");
    return false;
  }
  if (status != 200) {
    Serial.println("OTA: need HTTP 200 for .ota");
    return false;
  }
  return true;
}


// =============================================================================
// Downloaded file inspection
// =============================================================================

/** Return file size in bytes, or -1 on error. Path is mbed FAT style, e.g. "/fs/...". */
static long otaQspiFileSize(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return -1;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return -1;
  }
  long const sz = ftell(f);
  fclose(f);
  return sz;
}

/** Detect HTML error pages masquerading as a binary .ota download. */
static bool otaHeaderLooksLikeHtml(uint8_t const* h, size_t n) {
  if (n < 5) {
    return false;
  }
  if (memcmp(h, "<!DOC", 5) == 0 || memcmp(h, "<!doc", 5) == 0) {
    return true;
  }
  if (memcmp(h, "<html", 5) == 0 || memcmp(h, "<HTML", 5) == 0) {
    return true;
  }
  if (n >= 2 && h[0] == '<' && h[1] == '!') {
    return true;
  }
  return false;
}

/**
 * Validate the bin2ota container written to /fs/UPDATE.BIN.LZSS.
 * Expected layout: 4-byte little-endian payload length + 4-byte CRC + LZSS data.
 * Total file size must equal 8 + length. Rejects gzip and HTML responses.
 */
static bool otaValidateDownloadedOtaContainer() {
  long const fsz = otaQspiFileSize("/fs/UPDATE.BIN.LZSS");
  if (fsz < 24) {
    Serial.println("OTA: UPDATE.BIN.LZSS too small.");
    return false;
  }
  FILE* f = fopen("/fs/UPDATE.BIN.LZSS", "rb");
  if (!f) {
    return false;
  }
  uint8_t h[16];
  size_t const n = fread(h, 1, sizeof(h), f);
  fclose(f);
  if (n < 8) {
    return false;
  }
  if (h[0] == 0x1f && h[1] == 0x8b) {
    Serial.println("OTA: gzip signature — disable gzip for .ota on server.");
    return false;
  }
  uint32_t const lenField =
      (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
  if (lenField >= 12 && lenField <= 32UL * 1024UL * 1024UL &&
      (uint64_t)fsz == (uint64_t)lenField + 8ULL) {
    return true;
  }
  if (otaHeaderLooksLikeHtml(h, n)) {
    Serial.println("OTA: HTML response, not .ota binary.");
    return false;
  }
  if (lenField < 12 || lenField > 32UL * 1024UL * 1024UL) {
    Serial.println("OTA: invalid bin2ota length field.");
    return false;
  }
  Serial.print("OTA: file size ");
  Serial.print(fsz);
  Serial.print(" != 8 + length ");
  Serial.println((unsigned long)(lenField + 8));
  return false;
}


// =============================================================================
// Firmware download
// =============================================================================

/**
 * Stream the .ota file from HTTP(S) into /fs/UPDATE.BIN.LZSS using HttpClient.
 * More reliable than mbed WiFi.download() on some Giga setups (which may return 0).
 * Returns byte count on success, or a negative error code.
 */
static int otaDownloadLzssViaHttpClient(const char* url, bool want_https) {
  static char const kDest[] = "/fs/UPDATE.BIN.LZSS";
  bool url_https = false;
  String host;
  String path;
  if (!otaParseHttpUrl(url, &url_https, &host, &path)) {
    return -1;
  }
  if (url_https != want_https) {
    return -2;
  }

  WiFiClient wifi;
  WiFiSSLClient ssl;
  Client& conn = want_https ? static_cast<Client&>(ssl) : static_cast<Client&>(wifi);
  conn.setTimeout(30000);

  HttpClient http(conn, host.c_str(), want_https ? 443 : 80);
  http.setHttpResponseTimeout(120000);
  http.setHttpWaitForDataDelay(25);

  Serial.println("OTA: HttpClient GET...");
  int const getErr = http.get(path.c_str());
  if (getErr != 0) {
    http.stop();
    return getErr;
  }

  int const status = http.responseStatusCode();
  int const len = http.contentLength();
  if (status != 200) {
    http.stop();
    return -100;
  }

  bool const knownTotal = (len > 0);
  if (knownTotal) {
    Serial.print("OTA HttpClient Content-Length ");
    Serial.println(len);
  }

  remove(kDest);
  FILE* outf = fopen(kDest, "wb");
  if (!outf) {
    http.stop();
    return -101;
  }

  uint8_t buffer[1024];
  int downloaded = 0;
  uint32_t idleStart = 0;
  uint32_t const kIdleBreakMs = 180000U;  // abort if no data for 3 minutes

  for (;;) {
    int ret = http.read(buffer, sizeof(buffer));
    if (ret > 0) {
      idleStart = 0;
      int take = ret;
      if (knownTotal && downloaded + take > len) {
        take = len - downloaded;
      }
      if (take <= 0) {
        break;
      }
      size_t const w = fwrite(buffer, 1, (size_t)take, outf);
      if (w != (size_t)take) {
        fclose(outf);
        http.stop();
        remove(kDest);
        return -102;
      }
      downloaded += take;
      if (knownTotal && downloaded >= len) {
        break;
      }
    } else {
      if (!http.connected()) {
        break;
      }
      if (idleStart == 0) {
        idleStart = millis();
      }
      if ((uint32_t)(millis() - idleStart) > kIdleBreakMs) {
        fclose(outf);
        http.stop();
        remove(kDest);
        return -103;
      }
      delay(2);
    }
  }

  http.stop();
  fclose(outf);

  if (knownTotal && downloaded != len) {
    remove(kDest);
    return -104;
  }
  if (downloaded <= 0) {
    remove(kDest);
    return -105;
  }

  Serial.print("OTA HttpClient: wrote ");
  Serial.print(downloaded);
  Serial.println(" bytes");
  return downloaded;
}


// =============================================================================
// WiFi and main OTA pipeline
// =============================================================================

/** Connect using WIFI_SSID / WIFI_PASSWORD from password_ssid.h. */
static bool connectWiFi() {
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("ERROR: WiFi module not found.");
    return false;
  }

  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.disconnect();
  delay(200);

  int attempts = 0;
  while (WiFi.begin(WIFI_SSID, WIFI_PASSWORD) != WL_CONNECTED && attempts < WIFI_CONNECT_ATTEMPTS) {
    attempts++;
    Serial.print('.');
    delay(WIFI_CONNECT_DELAY_MS);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: connect failed.");
    return false;
  }

  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

/**
 * Full OTA sequence: begin storage → probe URL → download .ota → validate →
 * decompress to UPDATE.BIN → ota.update() → ota.reset().
 * Returns true only if reset is about to happen; false leaves the loader running.
 */
static bool performOtaUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("OTA: WiFi not connected.");
    return false;
  }

  Arduino_Portenta_OTA_QSPI ota(QSPI_FLASH_FATFS_MBR, 2);
  Arduino_Portenta_OTA::Error ota_err = Arduino_Portenta_OTA::Error::None;

  if (!ota.isOtaCapable()) {
    Serial.println("Bootloader does not support OTA.");
    Serial.println("Update: Portenta_System -> PortentaH7_updateBootloader");
    return false;
  }

  root.init();
  part.init();
  Serial.println("OTA: release user QSPI for Portenta OTA...");
  otaReleaseUserQspiBeforePortentaOtaBegin();

  if ((ota_err = ota.begin()) != Arduino_Portenta_OTA::Error::None) {
    otaPrintBeginFailure(ota_err);
    otaRemountUserFsAfterFail("begin");
    return false;
  }

  uint32_t start = millis();
  Serial.print("OTA firmware URL: ");
  Serial.println(OTA_FILE_LOCATION_FIRMWARE);

  if (!otaProbeFirmwareHead(OTA_FILE_LOCATION_FIRMWARE, OTA_FIRMWARE_IS_HTTPS != 0)) {
    Serial.println("OTA probe failed.");
    otaRemountUserFsAfterFail("probe");
    return false;
  }

  int ota_download = 0;

#if OTA_FIRMWARE_USE_SPLIT_DL_DECOMPRESS
  // Step 1: download compressed .ota container to QSPI FAT
  int dlRaw =
      otaDownloadLzssViaHttpClient(OTA_FILE_LOCATION_FIRMWARE, OTA_FIRMWARE_IS_HTTPS != 0);
  if (dlRaw <= 0) {
    Serial.println("OTA: HttpClient failed — trying mbed ota.download()...");
    dlRaw = ota.download(OTA_FILE_LOCATION_FIRMWARE, OTA_FIRMWARE_IS_HTTPS != 0);
  }
  if (dlRaw <= 0) {
    Serial.print("OTA: download failed, code ");
    Serial.println(dlRaw);
    otaSerialExplainNegativeFwCode(dlRaw);
    otaRemountUserFsAfterFail("download");
    return false;
  }

  // Step 2: sanity-check bin2ota header before decompress
  if (!otaValidateDownloadedOtaContainer()) {
    remove("/fs/UPDATE.BIN.LZSS");
    otaRemountUserFsAfterFail("ota_validate");
    return false;
  }

  // Step 3: LZSS decompress → /fs/UPDATE.BIN (raw flash image)
  Serial.println("OTA: LZSS decompress...");
  int const dec = ota.decompress();
  if (dec <= 0) {
    Serial.print("OTA decompress() ");
    Serial.println(dec);
    otaSerialExplainNegativeFwCode(dec);
    otaRemountUserFsAfterFail("decompress");
    return false;
  }
  ota_download = dec;
#else
  // Single-call path: download + decompress in one mbed API call
  ota_download = ota.downloadAndDecompress(OTA_FILE_LOCATION_FIRMWARE, OTA_FIRMWARE_IS_HTTPS != 0);
#endif

  if (ota_download <= 0) {
    Serial.print("OTA download/decompress failed: ");
    Serial.println(ota_download);
    otaSerialExplainNegativeFwCode(ota_download);
    otaRemountUserFsAfterFail("download");
    return false;
  }

  float const elapsed = (millis() - start) / 1000.0f;
  float const speed = (ota_download / elapsed) / 1024.0f;
  Serial.print(ota_download);
  Serial.print(" bytes stored in ");
  Serial.print(elapsed);
  Serial.print(" s (");
  Serial.print(speed);
  Serial.println(" KB/s)");

  // Step 4: hand off UPDATE.BIN to the bootloader and reboot
  Serial.println("Applying firmware update...");
  if ((ota_err = ota.update()) != Arduino_Portenta_OTA::Error::None) {
    Serial.print("ota.update() failed: ");
    Serial.println((int)ota_err);
    otaRemountUserFsAfterFail("update");
    return false;
  }

  delay(1000);  // flush Serial before reset
  Serial.println("Update OK — resetting...");
  ota.reset();
  return true;  // not reached if reset succeeds
}


// =============================================================================
// Arduino entry points
// =============================================================================

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
    delay(10);
  }

  Serial.println();
  Serial.println("=== Giga OTA loader ===");

  if (!connectWiFi()) {
    Serial.println("Loader halted — fix WiFi credentials in password_ssid.h");
    return;
  }

  if (!performOtaUpdate()) {
    Serial.println("OTA failed — retry in 30 s");
  }
}

void loop() {
  static uint32_t lastRetry = 0;

  // Reconnect WiFi if the link dropped, then retry OTA immediately
  if (WiFi.status() != WL_CONNECTED) {
    if (connectWiFi()) {
      performOtaUpdate();
    }
    return;
  }

  // Periodic retry while OTA keeps failing (e.g. server temporarily down)
  if (millis() - lastRetry >= 30000UL) {
    lastRetry = millis();
    Serial.println("Retrying OTA...");
    performOtaUpdate();
  }
}
