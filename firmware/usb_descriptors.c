// USB descriptors: a composite device with a serial port and a MIDI port.
#include "pico/unique_id.h"
#include "tusb.h"

// pid.codes' test pair. Fine on your own desk; get a real pair before shipping anything.
#define USB_VID 0x1209
#define USB_PID 0x0001

enum
{
    ITF_CDC,
    ITF_CDC_DATA,
    ITF_MIDI,
    ITF_MIDI_STREAMING,
    ITF_COUNT
};
enum
{
    EP_CDC_NOTIFY = 0x81,
    EP_CDC_OUT = 0x02,
    EP_CDC_IN = 0x82,
    EP_MIDI_OUT = 0x03,
    EP_MIDI_IN = 0x83
};
enum
{
    STR_LANGUAGE,
    STR_MAKER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CDC,
    STR_MIDI
};

static const tusb_desc_device_t device = {
    .bLength = sizeof (tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // Interface association, so the host pairs the CDC interfaces.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MAKER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_LENGTH (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MIDI_DESC_LEN)

static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR (1, ITF_COUNT, 0, CONFIG_LENGTH, 0, 250),
    TUD_CDC_DESCRIPTOR (ITF_CDC, STR_CDC, EP_CDC_NOTIFY, 8, EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_MIDI_DESCRIPTOR (ITF_MIDI, STR_MIDI, EP_MIDI_OUT, EP_MIDI_IN, 64),
};

const uint8_t* tud_descriptor_device_cb (void)
{
    return (const uint8_t*) &device;
}

const uint8_t* tud_descriptor_configuration_cb (uint8_t index)
{
    (void) index;
    return configuration;
}

const uint16_t* tud_descriptor_string_cb (uint8_t index, uint16_t langid)
{
    (void) langid;
    static uint16_t out[33];
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char* text;
    switch (index)
    {
        case STR_LANGUAGE:
            out[1] = 0x0409;
            out[0] = (TUSB_DESC_STRING << 8) | 4;
            return out;
        case STR_MAKER: text = "Charlie Culbert"; break;
        case STR_PRODUCT: text = "PICOH-101"; break;
        case STR_SERIAL:
            pico_get_unique_board_id_string (serial, sizeof serial);
            text = serial;
            break;
        case STR_CDC: text = "PICOH-101 Serial"; break;
        case STR_MIDI: text = "PICOH-101 MIDI"; break;
        default: return NULL;
    }
    uint8_t n = 0;
    while (text[n] && n < 32)
    {
        out[1 + n] = (uint8_t) text[n];
        ++n;
    }
    out[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return out;
}
