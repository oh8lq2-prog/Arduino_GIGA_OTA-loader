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
