#include "audio_out.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "msettings.h"

/* Linux input event codes (linux/input-event-codes.h) */
#define EV_KEY           0x01
#define KEY_VOLUMEDOWN   114
#define KEY_VOLUMEUP     115
#define KEY_NEXTSONG     163
#define KEY_PLAYPAUSE    164
#define KEY_PREVIOUSSONG 165
#define KEY_PLAYCD       200
#define KEY_PAUSECD      201

/* struct input_event on 64-bit Linux */
struct input_event_raw {
    uint64_t tv_sec;
    uint64_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

static bool bluetooth_audio_active = false;
static bool usbdac_audio_active    = false;
static int  usb_hid_fd             = -1;

/* Find the earphone button device by scanning /proc/bus/input/devices:
 * a USB keyboard-type device, or a Bluetooth AVRCP one. */
static int find_audio_hid_device(char *event_path, size_t path_size, bool find_bluetooth)
{
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return -1;

    char line[512];
    char handlers[256] = {0};
    bool is_usb = false;
    bool is_bluetooth_avrcp = false;
    bool has_kbd = false;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "N: Name=", 8) == 0) {
            /* New device, reset state */
            handlers[0] = 0;
            is_usb = false;
            has_kbd = false;
            is_bluetooth_avrcp = strstr(line + 8, "AVRCP") != NULL;
        } else if (strncmp(line, "P: Phys=", 8) == 0) {
            if (strstr(line, "usb-")) is_usb = true;
        } else if (strncmp(line, "H: Handlers=", 12) == 0) {
            strncpy(handlers, line + 12, sizeof(handlers) - 1);
            if (strstr(handlers, "kbd")) has_kbd = true;
        } else if (line[0] == '\n') {
            /* End of device entry */
            bool match = has_kbd && (find_bluetooth ? is_bluetooth_avrcp : is_usb);
            char *event_ptr = match ? strstr(handlers, "event") : NULL;
            int event_num = -1;
            if (event_ptr && sscanf(event_ptr, "event%d", &event_num) == 1 && event_num >= 0) {
                snprintf(event_path, path_size, "/dev/input/event%d", event_num);
                fclose(f);
                return 0;
            }
        }
    }

    fclose(f);
    return -1;
}

static void open_hid(void)
{
    char event_path[64];
    if (usb_hid_fd >= 0) {
        close(usb_hid_fd);
        usb_hid_fd = -1;
    }
    if (usbdac_audio_active && find_audio_hid_device(event_path, sizeof(event_path), false) == 0)
        usb_hid_fd = open(event_path, O_RDONLY | O_NONBLOCK);
    if (usb_hid_fd < 0 && bluetooth_audio_active &&
        find_audio_hid_device(event_path, sizeof(event_path), true) == 0)
        usb_hid_fd = open(event_path, O_RDONLY | O_NONBLOCK);
}

void AudioOut_init(void)
{
    int audio_sink = GetAudioSink();
    if (audio_sink == AUDIO_SINK_USBDAC) usbdac_audio_active = true;

    /* .asoundrc with bluealsa is more reliable than msettings for Bluetooth */
    const char *home = getenv("HOME");
    if (home) {
        char asoundrc_path[512];
        snprintf(asoundrc_path, sizeof(asoundrc_path), "%s/.asoundrc", home);
        FILE *f = fopen(asoundrc_path, "r");
        if (f) {
            char buf[256];
            while (fgets(buf, sizeof(buf), f)) {
                if (strstr(buf, "bluealsa")) {
                    audio_sink = AUDIO_SINK_BLUETOOTH;
                    bluetooth_audio_active = true;
                    break;
                }
            }
            fclose(f);
        }
    }

    if (audio_sink == AUDIO_SINK_BLUETOOTH) {
        /* Every "...A2DP" mixer control to 100%; volume is applied in software */
        system("amixer scontrols 2>/dev/null | grep -i 'A2DP' | "
               "sed \"s/.*'\\([^']*\\)'.*/\\1/\" | "
               "while read ctrl; do amixer sset \"$ctrl\" 127 2>/dev/null; done");
        open_hid();
    }

    if (audio_sink == AUDIO_SINK_USBDAC) {
        /* USB DACs usually appear as card 1; their control names vary */
        system("amixer -c 1 sset PCM 100% 2>/dev/null; "
               "amixer -c 1 sset Master 100% 2>/dev/null; "
               "amixer -c 1 sset Speaker 100% 2>/dev/null; "
               "amixer -c 1 sset Headphone 100% 2>/dev/null; "
               "amixer -c 1 sset Headset 100% 2>/dev/null");
        open_hid();
    }
}

void AudioOut_quit(void)
{
    if (usb_hid_fd >= 0) {
        close(usb_hid_fd);
        usb_hid_fd = -1;
    }
}

bool AudioOut_isBluetoothActive(void) { return bluetooth_audio_active; }
bool AudioOut_isUSBDACActive(void)    { return usbdac_audio_active; }

USBHIDEvent AudioOut_pollUSBHID(void)
{
    if (usb_hid_fd < 0) return USB_HID_EVENT_NONE;

    struct input_event_raw ev;
    while (read(usb_hid_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        /* Key presses only: ignore release (0) and repeat (2) */
        if (ev.type != EV_KEY || ev.value != 1) continue;
        switch (ev.code) {
            case KEY_VOLUMEUP:     return USB_HID_EVENT_VOLUME_UP;
            case KEY_VOLUMEDOWN:   return USB_HID_EVENT_VOLUME_DOWN;
            case KEY_NEXTSONG:     return USB_HID_EVENT_NEXT_TRACK;
            case KEY_PLAYPAUSE:
            case KEY_PLAYCD:
            case KEY_PAUSECD:      return USB_HID_EVENT_PLAY_PAUSE;
            case KEY_PREVIOUSSONG: return USB_HID_EVENT_PREV_TRACK;
        }
    }
    return USB_HID_EVENT_NONE;
}
