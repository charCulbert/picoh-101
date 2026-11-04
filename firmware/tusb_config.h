// TinyUSB setup: one USB serial port and one USB MIDI port, each also carrying the control page's lines
// (usb.cpp).
#pragma once

#define CFG_TUSB_OS OPT_OS_PICO
#define CFG_TUD_ENABLED 1
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC 1
#define CFG_TUD_MIDI 1
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_VENDOR 0

#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 2048
#define CFG_TUD_CDC_EP_BUFSIZE 64
#define CFG_TUD_MIDI_RX_BUFSIZE 1024
#define CFG_TUD_MIDI_TX_BUFSIZE 2048 // SysEx lines to the page: a whole state fits
