# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Environment

ESP-IDF v6.0 runs inside a distrobox container named `esp`. All build commands must be prefixed:

```bash
distrobox enter esp -- bash -c '. $HOME/.espressif/v6.0/esp-idf/export.sh && <command>'
```

Or interactively: `distrobox enter esp && bash`, then source the environment and work inside.

The project uses `idf.py` (CMake wrapper) for all build operations.

## Build Commands

```bash
idf.py build              # Compile
idf.py -p /dev/ttyACM0 flash   # Flash to device
idf.py -p /dev/ttyACM0 monitor # Serial monitor
idf.py menuconfig         # Interactive config → "RemoteBoot Configuration" menu
idf.py fullclean          # Clean everything
```

No test suite exists for this project. There is no `idf.py test` target.

## Architecture

**Target**: ESP32-S3 with 4MB flash + PSRAM (N8R2/N16R8). Custom partition table in `partitions.csv` (NVS + otadata + ota_0 + ota_1 with rollback protection). Web UI files are embedded in the firmware binary via `EMBED_TXTFILES` (not SPIFFS). PSRAM is enabled via `CONFIG_SPIRAM=y` (Octal mode, 80MHz) — 80% of free PSRAM is allocated to the log ring buffer at boot.

**Dependency Inversion Principle (DIP)**: All application-layer code depends on interfaces defined in `main/hal/ihal.h`, never on concrete hardware implementations. This makes the hardware swappable (e.g., POST detection via HDD LED GPIO vs. USB bus events) and simplifies testing.

Three HAL interfaces:
- `IRelayController` — press/release relay for power and reset buttons
- `IPostDetector` — detect when BIOS POST completes (two implementations: HDD LED GPIO `post_detector.c`, USB handshake `usb_post_detector.c`)
- `IUsbHidKeyboard` — send HID keycodes over TinyUSB to control GRUB

**Boot sequence** (`main/main.c:app_main`):
1. NVS init + config load + generate API token if first boot
2. HAL init (relay, USB HID, POST detector — mode selected in Kconfig)
3. App layer init (OS selector, boot manager state machine)
4. WiFi connect (reads SSID/pass from NVS; falls back to Kconfig defaults)
5. Campus network keepalive (ePortal auth on connectivity loss)
6. HTTP server start (REST API on port 80 + embedded Web UI)

**Config system**: Two-layer with NVS priority. Kconfig (menuconfig) provides compile-time defaults; NVS (`config_manager`) provides runtime overrides via Web UI or API. Most settings — relay polarity, bootloader type, POST settle time, GPIO pins, WiFi credentials — are runtime-configurable via `/api/config` PUT. NVS keys are centralised in `config_manager.h` as `CFG_KEY_*` macros.

**System state** (`system_state.h`): Thread-safe singleton using a mutex. PC state machine: `OFFLINE → POWERING_ON → POST_RUNNING → [SELECTING_OS] → ONLINE`. All modules read/write through this API.

**Bootloader modes** (selectable via NVS `btldr_type` or Kconfig):
- **Windows Boot Manager** (default): USB POST detector uses simple mode — first mount = POST done after settle. No OS selection needed.
- **GRUB**: USB POST detector waits for mount→unmount→remount cycle (BIOS→GRUB handoff), then sends keyboard navigation to select the target OS.

**PC-online detection**: USB keyboard mounted = PC is on (replaces ICMP ping which is blocked by Windows firewall). After POST complete, state transitions directly to ONLINE.

## Key Conventions

- All modules follow a `*_create(&config, &instance)` factory pattern returning `esp_err_t`
- HAL modules prefix with their domain (`relay_controller_*`, `usb_hid_keyboard_*`, etc.)
- NVS namespace is `"rb"` (RemoteBoot)
- Web UI files in `main/web/` are embedded at build time via `EMBED_TXTFILES` in CMakeLists.txt (no SPIFFS)
- API Token is a 32-char hex string, auto-generated on first boot, printed to serial console
- **Log ring buffer** (`log_buffer.c`): Captures all ESP_LOGx output to a PSRAM ring buffer via `esp_log_set_vprintf()` hook. 80% of free PSRAM is allocated at boot. Entries are fixed-size 128B (timestamp + level + tag + message). Viewable via `GET /api/log` and the Web UI's "系统日志" tab. Campus net "Internet OK" is filtered to log only on state transitions.
