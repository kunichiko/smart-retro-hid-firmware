#ifndef _USB_CONFIG_H
#define _USB_CONFIG_H

// ===================================================================================
// USB-MIDI Configuration for smart-retro-hid
// ===================================================================================
// Uses ch32v003fun fsusb driver with FUSB_USER_HANDLERS.
// EP1 OUT: Host → Device (MIDI data from smartphone)
// EP2 IN:  Device → Host (MIDI data to smartphone)
// ===================================================================================

#include "funconfig.h"
#include "ch32fun.h"

#define FUSB_CONFIG_EPS       3  // EP0 + EP1 + EP2
// fsusb の mode: 1 = TX_EN (IN), -1 = RX_EN (OUT), 2 = 双方向 (TX+RX, AUTO_TOG)
#define USBFS_EP2_MODE        2  // EP2 双方向: OUT (MIDI受信) + IN (MIDI送信)
#define FUSB_SUPPORTS_SLEEP   0
#define FUSB_HID_INTERFACES   0
#define FUSB_CURSED_TURBO_DMA 0
#define FUSB_HID_USER_REPORTS 0
#define FUSB_IO_PROFILE       0
#define FUSB_USE_HPE          0
#define FUSB_USER_HANDLERS    1
#define FUSB_USE_DMA7_COPY    0
#define FUSB_VDD_5V           FUNCONF_USE_5V_VDD

#include "usb_defines.h"

// ---------------------------------------------------------------------------
// USB IDs
// ---------------------------------------------------------------------------
// pid.codes test PIDs (https://pid.codes/)
// TODO: 製品化時は正式な VID/PID を取得すること
#define FUSB_USB_VID 0x1209
#define FUSB_USB_PID 0x0001
#define FUSB_USB_REV 0x0100
#define FUSB_STR_MANUFACTURER u"kunichiko"
#define FUSB_STR_PRODUCT      u"smart-retro-hid"
#define FUSB_STR_SERIAL       u"001"

// ---------------------------------------------------------------------------
// Device Descriptor
// ---------------------------------------------------------------------------
static const uint8_t device_descriptor[] = {
    18,                                             // bLength
    1,                                              // bDescriptorType (Device)
    0x10, 0x01,                                     // bcdUSB (USB 1.1)
    0x00,                                           // bDeviceClass (defined at interface level)
    0x00,                                            // bDeviceSubClass
    0x00,                                           // bDeviceProtocol
    64,                                             // bMaxPacketSize0
    (uint8_t)(FUSB_USB_VID), (uint8_t)(FUSB_USB_VID >> 8),
    (uint8_t)(FUSB_USB_PID), (uint8_t)(FUSB_USB_PID >> 8),
    (uint8_t)(FUSB_USB_REV), (uint8_t)(FUSB_USB_REV >> 8),
    1,                                              // iManufacturer
    2,                                              // iProduct
    3,                                              // iSerialNumber
    1,                                              // bNumConfigurations
};

// ---------------------------------------------------------------------------
// Configuration Descriptor (Audio Control + MIDI Streaming)
// ---------------------------------------------------------------------------
// Total length: 9 + 9 + 9 + 7 + 6 + 6 + 9 + 9 + 9 + 5 + 9 + 5 = 101 bytes
// Endpoint layout:
//   EP1 OUT (0x01) - Bulk, 64 bytes - Host to Device (MIDI OUT)
//   EP2 IN  (0x82) - Bulk, 64 bytes - Device to Host (MIDI IN)
// ---------------------------------------------------------------------------
static const uint8_t config_descriptor[] = {
    // --- Configuration Descriptor ---
    0x09,                   // bLength
    0x02,                   // bDescriptorType (Configuration)
    101, 0x00,              // wTotalLength
    0x02,                   // bNumInterfaces (AC + MS)
    0x01,                   // bConfigurationValue
    0x00,                   // iConfiguration
    0x80,                   // bmAttributes (Bus Powered)
    0x32,                   // bMaxPower (100mA)

    // --- Interface 0: Audio Control ---
    0x09,                   // bLength
    0x04,                   // bDescriptorType (Interface)
    0x00,                   // bInterfaceNumber
    0x00,                   // bAlternateSetting
    0x00,                   // bNumEndpoints
    0x01,                   // bInterfaceClass (Audio)
    0x01,                   // bInterfaceSubClass (Audio Control)
    0x00,                   // bInterfaceProtocol
    0x00,                   // iInterface

    // --- AC Header Descriptor ---
    0x09,                   // bLength
    0x24,                   // bDescriptorType (CS_INTERFACE)
    0x01,                   // bDescriptorSubtype (HEADER)
    0x00, 0x01,             // bcdADC (1.0)
    0x09, 0x00,             // wTotalLength (AC descriptors only = 9)
    0x01,                   // bInCollection (1 streaming interface)
    0x01,                   // baInterfaceNr(1) -> Interface 1

    // --- Interface 1: MIDI Streaming ---
    0x09,                   // bLength
    0x04,                   // bDescriptorType (Interface)
    0x01,                   // bInterfaceNumber
    0x00,                   // bAlternateSetting
    0x02,                   // bNumEndpoints (EP1 OUT + EP2 IN)
    0x01,                   // bInterfaceClass (Audio)
    0x03,                   // bInterfaceSubClass (MIDI Streaming)
    0x00,                   // bInterfaceProtocol
    0x00,                   // iInterface

    // --- MS Header Descriptor ---
    0x07,                   // bLength
    0x24,                   // bDescriptorType (CS_INTERFACE)
    0x01,                   // bDescriptorSubtype (MS_HEADER)
    0x00, 0x01,             // bcdMSC (1.0)
    // wTotalLength of all MS class-specific descriptors:
    // MS Header(7) + IN Jack Emb(6) + IN Jack Ext(6) + OUT Jack Emb(9) + OUT Jack Ext(9)
    // + EP OUT(9+5) + EP IN(9+5) = 65
    65, 0x00,               // wTotalLength

    // --- MIDI IN Jack (Embedded) - ID=1 ---
    // Represents the device's input from USB (host sends MIDI here)
    0x06,                   // bLength
    0x24,                   // bDescriptorType (CS_INTERFACE)
    0x02,                   // bDescriptorSubtype (MIDI_IN_JACK)
    0x01,                   // bJackType (EMBEDDED)
    0x01,                   // bJackID
    0x00,                   // iJack

    // --- MIDI IN Jack (External) - ID=2 ---
    // Represents physical MIDI input (retro PC → device)
    0x06,                   // bLength
    0x24,                   // bDescriptorType (CS_INTERFACE)
    0x02,                   // bDescriptorSubtype (MIDI_IN_JACK)
    0x02,                   // bJackType (EXTERNAL)
    0x02,                   // bJackID
    0x00,                   // iJack

    // --- MIDI OUT Jack (Embedded) - ID=3 ---
    // Represents the device's output to USB (device sends MIDI here)
    // Source: External IN Jack (ID=2)
    0x09,                   // bLength
    0x24,                   // bDescriptorType (CS_INTERFACE)
    0x03,                   // bDescriptorSubtype (MIDI_OUT_JACK)
    0x01,                   // bJackType (EMBEDDED)
    0x03,                   // bJackID
    0x01,                   // bNrInputPins
    0x02,                   // baSourceID(1) -> External IN Jack (ID=2)
    0x01,                   // baSourcePin(1)
    0x00,                   // iJack

    // --- MIDI OUT Jack (External) - ID=4 ---
    // Represents physical MIDI output (device → retro PC)
    // Source: Embedded IN Jack (ID=1)
    0x09,                   // bLength
    0x24,                   // bDescriptorType (CS_INTERFACE)
    0x03,                   // bDescriptorSubtype (MIDI_OUT_JACK)
    0x02,                   // bJackType (EXTERNAL)
    0x04,                   // bJackID
    0x01,                   // bNrInputPins
    0x01,                   // baSourceID(1) -> Embedded IN Jack (ID=1)
    0x01,                   // baSourcePin(1)
    0x00,                   // iJack

    // --- EP2 OUT (Bulk) - Host to Device ---
    0x09,                   // bLength
    0x05,                   // bDescriptorType (Endpoint)
    0x02,                   // bEndpointAddress (OUT, EP2)
    0x02,                   // bmAttributes (Bulk)
    0x40, 0x00,             // wMaxPacketSize (64)
    0x00,                   // bInterval
    0x00,                   // bRefresh
    0x00,                   // bSynchAddress

    // --- CS Endpoint Descriptor (for EP2 OUT) ---
    0x05,                   // bLength
    0x25,                   // bDescriptorType (CS_ENDPOINT)
    0x01,                   // bDescriptorSubtype (MS_GENERAL)
    0x01,                   // bNumEmbMIDIJack
    0x01,                   // baAssocJackID(1) -> Embedded IN Jack (ID=1)

    // --- EP2 IN (Bulk) - Device to Host ---
    0x09,                   // bLength
    0x05,                   // bDescriptorType (Endpoint)
    0x82,                   // bEndpointAddress (IN, EP2)
    0x02,                   // bmAttributes (Bulk)
    0x40, 0x00,             // wMaxPacketSize (64)
    0x00,                   // bInterval
    0x00,                   // bRefresh
    0x00,                   // bSynchAddress

    // --- CS Endpoint Descriptor (for EP2 IN) ---
    0x05,                   // bLength
    0x25,                   // bDescriptorType (CS_ENDPOINT)
    0x01,                   // bDescriptorSubtype (MS_GENERAL)
    0x01,                   // bNumEmbMIDIJack
    0x03,                   // baAssocJackID(1) -> Embedded OUT Jack (ID=3)
};

// ---------------------------------------------------------------------------
// String Descriptors
// ---------------------------------------------------------------------------
struct usb_string_descriptor_struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wString[];
};

const static struct usb_string_descriptor_struct language __attribute__((section(".rodata"))) = {
    4, 3, {0x0409}
};
const static struct usb_string_descriptor_struct string1 __attribute__((section(".rodata"))) = {
    sizeof(FUSB_STR_MANUFACTURER), 3, FUSB_STR_MANUFACTURER
};
const static struct usb_string_descriptor_struct string2 __attribute__((section(".rodata"))) = {
    sizeof(FUSB_STR_PRODUCT), 3, FUSB_STR_PRODUCT
};
const static struct usb_string_descriptor_struct string3 __attribute__((section(".rodata"))) = {
    sizeof(FUSB_STR_SERIAL), 3, FUSB_STR_SERIAL
};

// ---------------------------------------------------------------------------
// Descriptor Lookup Table
// ---------------------------------------------------------------------------
const static struct descriptor_list_struct {
    uint32_t lIndexValue;
    const uint8_t *addr;
    uint8_t length;
} descriptor_list[] = {
    {0x00000100, device_descriptor, sizeof(device_descriptor)},
    {0x00000200, config_descriptor, sizeof(config_descriptor)},
    {0x00000300, (const uint8_t *)&language, 4},
    {0x04090301, (const uint8_t *)&string1, string1.bLength},
    {0x04090302, (const uint8_t *)&string2, string2.bLength},
    {0x04090303, (const uint8_t *)&string3, string3.bLength},
};
#define DESCRIPTOR_LIST_ENTRIES ((sizeof(descriptor_list)) / (sizeof(struct descriptor_list_struct)))

#endif
