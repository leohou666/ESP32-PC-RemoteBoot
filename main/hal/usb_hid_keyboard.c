/**
 * @file usb_hid_keyboard.c
 * @brief TinyUSB HID Keyboard implementation for ESP32-S3.
 *
 * The ESP32-S3 native USB (GPIO19 D−, GPIO20 D+) is configured as a
 * composite HID keyboard device. Connect the native USB port to the PC.
 *
 * NOTE: Make sure CONFIG_TINYUSB_ENABLED=y and CONFIG_TINYUSB_HID_ENABLED=y
 *       in sdkconfig.
 */
#include "usb_hid_keyboard.h"

#include <stdlib.h>
#include <string.h>
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG        "usb_hid"
#define REPORT_ID  1

/* ─────────────────────────────────────────────────────────────────── */
/* USB mount event notification system                                   */
/* ─────────────────────────────────────────────────────────────────── */
#define USB_MOUNT_CB_MAX 4

typedef struct {
    usb_mount_event_cb_t fn;
    void *arg;
} mount_cb_entry_t;

static mount_cb_entry_t s_mount_cbs[USB_MOUNT_CB_MAX];
static int s_mount_cb_count = 0;

void usb_hid_register_mount_cb(usb_mount_event_cb_t cb, void *arg)
{
    if (s_mount_cb_count < USB_MOUNT_CB_MAX) {
        s_mount_cbs[s_mount_cb_count].fn  = cb;
        s_mount_cbs[s_mount_cb_count].arg = arg;
        s_mount_cb_count++;
    }
}

static void notify_mount_event(usb_mount_event_t event)
{
    for (int i = 0; i < s_mount_cb_count; i++) {
        if (s_mount_cbs[i].fn) {
            s_mount_cbs[i].fn(event, s_mount_cbs[i].arg);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────── */
/* HID Report Descriptor — standard boot keyboard                       */
/* ─────────────────────────────────────────────────────────────────── */
static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID))
};

/* ─────────────────────────────────────────────────────────────────── */
/* TinyUSB callbacks (required by tud_hid driver)                       */
/* ─────────────────────────────────────────────────────────────────── */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer;   (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer;   (void)bufsize;
}

/* ─────────────────────────────────────────────────────────────────── */
/* TinyUSB device-level event adapter for mount tracking                 */
/* ─────────────────────────────────────────────────────────────────── */
static void tinyusb_event_handler(tinyusb_event_t *event, void *arg)
{
    (void)arg;

    if (!event) {
        return;
    }

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "USB device mounted by host");
        notify_mount_event(USB_EVENT_MOUNTED);
        break;

    case TINYUSB_EVENT_DETACHED:
        ESP_LOGW(TAG, "USB device unmounted by host");
        notify_mount_event(USB_EVENT_UNMOUNTED);
        break;

#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    case TINYUSB_EVENT_SUSPENDED:
        ESP_LOGD(TAG, "USB device suspended");
        notify_mount_event(USB_EVENT_SUSPENDED);
        break;
#endif

#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
    case TINYUSB_EVENT_RESUMED:
        ESP_LOGD(TAG, "USB device resumed");
        notify_mount_event(USB_EVENT_RESUMED);
        break;
#endif

    default:
        break;
    }
}

/* ─────────────────────────────────────────────────────────────────── */

typedef struct {
    IUsbHidKeyboard iface;  /* MUST be first */
} hid_kbd_t;

static esp_err_t kbd_send_key(IUsbHidKeyboard *self, uint8_t keycode,
                               uint32_t press_ms)
{
    (void)self;

    if (!tud_mounted()) {
        ESP_LOGW(TAG, "USB not mounted, cannot send key");
        return ESP_ERR_INVALID_STATE;
    }

    /* Wait for HID interface to be ready */
    for (int i = 0; i < 20 && !tud_hid_ready(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!tud_hid_ready()) {
        ESP_LOGW(TAG, "HID not ready");
        return ESP_ERR_TIMEOUT;
    }

    /* Key down */
    uint8_t keycodes[6] = {keycode, 0, 0, 0, 0, 0};
    tud_hid_keyboard_report(REPORT_ID, 0, keycodes);
    vTaskDelay(pdMS_TO_TICKS(press_ms > 0 ? press_ms : 50));

    /* Key up */
    tud_hid_keyboard_report(REPORT_ID, 0, NULL);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGD(TAG, "Sent key 0x%02X for %lu ms", keycode, press_ms);
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────── */
/* HID Configuration Descriptor                                         */
/* ─────────────────────────────────────────────────────────────────── */
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

enum {
    ITF_NUM_HID,
    ITF_NUM_TOTAL
};

static const uint8_t s_hid_configuration_descriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // Interface number, string index, protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_hid_report_desc), 0x81, 16, 10)
};

/* ─────────────────────────────────────────────────────────────────── */

esp_err_t usb_hid_keyboard_create(IUsbHidKeyboard **out)
{
    hid_kbd_t *k = calloc(1, sizeof(hid_kbd_t));
    if (!k) return ESP_ERR_NO_MEM;

    /* TinyUSB device configuration */
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(tinyusb_event_handler);
    tusb_cfg.descriptor.full_speed_config = s_hid_configuration_descriptor;

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(err));
        free(k);
        return err;
    }

    /* Wait for USB enumeration (up to 3 seconds) */
    ESP_LOGI(TAG, "Waiting for USB enumeration...");
    for (int i = 0; i < 30 && !tud_mounted(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!tud_mounted()) {
        ESP_LOGW(TAG, "USB not enumerated (PC not connected?). Continuing anyway.");
    } else {
        ESP_LOGI(TAG, "USB HID keyboard mounted");
    }

    k->iface.ctx      = k;
    k->iface.send_key = kbd_send_key;

    *out = &k->iface;
    return ESP_OK;
}

void usb_hid_keyboard_destroy(IUsbHidKeyboard *kbd)
{
    if (kbd) free(kbd);
}
