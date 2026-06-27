#ifndef PASSWORD_SSID_H
#define PASSWORD_SSID_H

// =============================================================================
// WiFi credentials
// =============================================================================

// Network name (SSID) the board joins before OTA download.
#define WIFI_SSID     "xxxxxxxx"

// Pre-shared key (password) for the network above.
#define WIFI_PASSWORD "xxxxxxxx"

// Max connection attempts before setup() gives up (each attempt waits WIFI_CONNECT_DELAY_MS).
#define WIFI_CONNECT_ATTEMPTS 30

// Delay in milliseconds between WiFi.begin() retries.
#define WIFI_CONNECT_DELAY_MS 500


// =============================================================================
// OTA firmware source selection
// =============================================================================

// 1 = download from internet URLs below (OTA_FILE_LOCATION_REMOTE_*)
// 0 = download from local LAN URL (OTA_FILE_LOCATION_LAN)
#define OTA_FIRMWARE_USE_REMOTE 1

// 1 = use HTTPS URL (OTA_FILE_LOCATION_REMOTE_HTTPS, port 443)
// 0 = use HTTP URL  (OTA_FILE_LOCATION_REMOTE_HTTP,  port 80)
// Must match the scheme of the URL you actually use.
#define OTA_FIRMWARE_USE_REMOTE_HTTPS 0


// =============================================================================
// OTA file URLs and filename
// =============================================================================

// Public HTTP URL of the .ota firmware package on your update server.
// Typical path: /update/<sketch_name>.ota
#define OTA_FILE_LOCATION_REMOTE_HTTP  "http://www.xxxxxx.com/xxxx/xxxxx.ota"

// Same file over HTTPS (use when the server redirects HTTP → HTTPS).
#define OTA_FILE_LOCATION_REMOTE_HTTPS "https://www.xxxxxx.com/xxxxx/xxxxx.ota"

// Local LAN URL for bench testing without internet (OTA_FIRMWARE_USE_REMOTE = 0).
// Host is usually a PC running a simple HTTP server serving the .ota file.
#define OTA_FILE_LOCATION_LAN "http://192.168.0.131/xxxxxx.ota"

// Basename of the firmware package on the server (informational; URLs above are authoritative).
#define OTA_FIRMWARE_FILENAME "xxxxxx.ota"


// =============================================================================
// OTA download / install behaviour
// =============================================================================

// 1 = download .ota via HttpClient, validate container, then LZSS decompress (recommended on Giga).
// 0 = single-step mbed ota.downloadAndDecompress().
#define OTA_FIRMWARE_USE_SPLIT_DL_DECOMPRESS 1


// =============================================================================
// Derived settings — do not edit unless you know what you are doing
// =============================================================================

#if OTA_FIRMWARE_USE_REMOTE
#if OTA_FIRMWARE_USE_REMOTE_HTTPS
#define OTA_FILE_LOCATION_FIRMWARE OTA_FILE_LOCATION_REMOTE_HTTPS
#define OTA_FIRMWARE_IS_HTTPS 1
#else
#define OTA_FILE_LOCATION_FIRMWARE OTA_FILE_LOCATION_REMOTE_HTTP
#define OTA_FIRMWARE_IS_HTTPS 0
#endif
#else
#define OTA_FILE_LOCATION_FIRMWARE OTA_FILE_LOCATION_LAN
#define OTA_FIRMWARE_IS_HTTPS 0
#endif

#endif
