#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

/*
 * What the UI still needs to know about audio output now that plexd plays
 * the music: whether NextUI sends audio to USB-C or Bluetooth (which get
 * software volume in plexd), and the play/next/volume buttons on those
 * earphones while the UI is open.
 */

#include <stdbool.h>

/* Audio file formats, for icons and labels */
typedef enum {
    AUDIO_FORMAT_UNKNOWN = 0,
    AUDIO_FORMAT_WAV,
    AUDIO_FORMAT_MP3,
    AUDIO_FORMAT_OGG,
    AUDIO_FORMAT_FLAC,
    AUDIO_FORMAT_MOD,
    AUDIO_FORMAT_M4A,
    AUDIO_FORMAT_AAC,
    AUDIO_FORMAT_OPUS
} AudioFormat;

/* Earphone button events */
typedef enum {
    USB_HID_EVENT_NONE = 0,
    USB_HID_EVENT_VOLUME_UP,
    USB_HID_EVENT_VOLUME_DOWN,
    USB_HID_EVENT_NEXT_TRACK,
    USB_HID_EVENT_PLAY_PAUSE,
    USB_HID_EVENT_PREV_TRACK
} USBHIDEvent;

/* Detect the output, set a USB DAC / Bluetooth mixer to 100% (volume is
 * applied in software instead) and open its button device. */
void AudioOut_init(void);
void AudioOut_quit(void);

bool AudioOut_isBluetoothActive(void);
bool AudioOut_isUSBDACActive(void);

/* Next earphone button press, or USB_HID_EVENT_NONE. Non-blocking. */
USBHIDEvent AudioOut_pollUSBHID(void);

#endif /* AUDIO_OUT_H */
