/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 HiFiPhile
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "bsp/board_api.h"
#include "tusb.h"
#include "config.h"
#include "slots.h"

bool ds_mode() {
    if (get_config().controller_mode == 2) {
        return !is_dse;
    }
    return get_config().controller_mode == 0;
}

// Per-slot gamepad endpoints (emitted inline in the config descriptor below):
// slot 0 keeps the original IN 0x84 / OUT 0x03 pair; slots 1-3 use
// 0x88/0x08, 0x89/0x09, 0x8A/0x0A — clear of audio (0x01/0x82), NCM
// (0x85/0x86/0x05) and the keyboard (0x87). RP2040/RP2350 expose 16 EP
// numbers per direction, so 4 gamepads + everything else fits.

enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING_OUT,
    ITF_NUM_AUDIO_STREAMING_IN,
    ITF_NUM_HID,       // slot 0 gamepad -- the upstream interface number (3)
#ifdef ENABLE_WEBCONFIG
    ITF_NUM_NET,       // CDC-NCM control (IAD spans control + data)
    ITF_NUM_NET_DATA,
#endif
#ifdef ENABLE_WAKE_HID
    ITF_NUM_HID_KBD,
#endif
    ITF_NUM_BASE_TOTAL,
    // Gamepad interfaces for slots 1..MULTI_SLOT_COUNT-1 are appended AFTER
    // the base set, so every base interface keeps its upstream number: NCM
    // keeps its network-adapter identity, the wake keyboard stays put, and
    // slot 0 remains the real-DualSense interface 3. Slot k (k >= 1) is
    // interface ITF_NUM_BASE_TOTAL + k - 1. FULL always exposes every slot
    // (empty ones report neutral input): USB cannot add interfaces without a
    // re-enumeration bounce, so joining/leaving controllers stay seamless and
    // the bus only bounces at first-connect / last-disconnect, exactly like
    // the upstream MINIMAL<->FULL swap. (The tail-append layout would also
    // support truncating to an exposed-slot count, if a hide-empty-slots
    // policy is ever wanted again.)
    ITF_NUM_TOTAL = ITF_NUM_BASE_TOTAL + (MULTI_SLOT_COUNT - 1),

    // The audio function uses an IAD; the device-class triple must be the
    // misc/common/IAD combo whenever the IAD-using CDC-NCM function is present.
    // The 8-byte audio IAD is only emitted in webconfig builds.
    CONFIG_DESC_LEN_AUDIO_IAD =
#if defined(ENABLE_WEBCONFIG)
        8,
#else
        0,
#endif
    // One gamepad interface block: 9 (interface) + 9 (HID class) + 7 (EP IN)
    // + 7 (EP OUT) = 32 bytes.
    CONFIG_DESC_LEN_GAMEPAD = 32,
    // 0x00E3 covers config header + audio function + slot 0's gamepad block
    // (the upstream single-controller descriptor).
    CONFIG_DESC_LEN_BASE = 0x00E3 + CONFIG_DESC_LEN_AUDIO_IAD,
    // Keyboard interface adds 25 bytes:
    //   9 (interface) + 9 (HID class) + 7 (EP IN) = 25
    CONFIG_DESC_LEN_WAKE_KBD =
#ifdef ENABLE_WAKE_HID
        25,
#else
        0,
#endif
    CONFIG_DESC_LEN_NET =
#ifdef ENABLE_WEBCONFIG
        TUD_CDC_NCM_DESC_LEN,
#else
        0,
#endif
    // Full length with every slot's gamepad block; trailing blocks (slots
    // 1..N-1) sit after the base set.
    CONFIG_DESC_LEN_TOTAL = CONFIG_DESC_LEN_BASE + CONFIG_DESC_LEN_WAKE_KBD
        + CONFIG_DESC_LEN_NET
        + (MULTI_SLOT_COUNT - 1) * CONFIG_DESC_LEN_GAMEPAD
};

// String Descriptor Index
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
#ifdef ENABLE_WEBCONFIG
    STRID_NET,
    STRID_MAC,
#endif
};

//--------------------------------------------------------------------+
// Shared descriptor fragments
//
// Reusable byte sequences so the FULL and MINIMAL config descriptors compose
// the SAME bytes for the interfaces they share, instead of being two
// hand-counted blobs that can silently drift. The most important of these is
// the CDC-NCM block: emitting it from one macro guarantees NCM lands on the
// same bInterfaceNumber in both variants, which is what keeps the host from
// creating two separate network adapters across a variant swap.
//--------------------------------------------------------------------+

#ifdef ENABLE_WEBCONFIG
// CDC-NCM (config web UI network interface), parameterised by control-interface
// number `ncm_itf` (the data interface is ncm_itf+1). Endpoints: notif IN 0x85,
// bulk OUT 0x05, bulk IN 0x86.
//
// Hand-emitted (not TUD_CDC_NCM_DESCRIPTOR) so BOTH the IAD iFunction and the
// control-interface iInterface strings are 0. Windows builds the Net adapter's
// FriendlyName as "<iManufacturer> <function/interface string>"; the
// manufacturer ("Sony Interactive Entertainment") must stay for DualSense
// driver matching, and adding a function string only makes the name worse. With
// no strings, Windows shows just the manufacturer-derived name. Byte layout
// matches the macro exactly (TUD_CDC_NCM_DESC_LEN), so the length asserts hold.
#define DS5_NCM_DESC(ncm_itf) \
    /* Interface Association: control + data, iFunction = 0 (no string) */ \
    8, TUSB_DESC_INTERFACE_ASSOCIATION, (ncm_itf), 2, TUSB_CLASS_CDC, \
        CDC_COMM_SUBCLASS_NETWORK_CONTROL_MODEL, 0, 0, \
    /* CDC Control Interface (iInterface = 0, no string) */ \
    9, TUSB_DESC_INTERFACE, (ncm_itf), 0, 1, TUSB_CLASS_CDC, \
        CDC_COMM_SUBCLASS_NETWORK_CONTROL_MODEL, 0, 0, \
    /* CDC Header */ \
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_HEADER, U16_TO_U8S_LE(0x0110), \
    /* CDC Union */ \
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_UNION, (ncm_itf), (uint8_t)((ncm_itf) + 1), \
    /* CDC Ethernet Networking (iMacAddress = STRID_MAC) */ \
    13, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_ETHERNET_NETWORKING, STRID_MAC, 0, 0, 0, 0, \
        U16_TO_U8S_LE(CFG_TUD_NET_MTU), U16_TO_U8S_LE(0), 0, \
    /* CDC-NCM Functional Descriptor */ \
    6, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_NCM, U16_TO_U8S_LE(0x0100), 0, \
    /* Endpoint Notification (IN 0x85) */ \
    7, TUSB_DESC_ENDPOINT, 0x85, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(64), 50, \
    /* CDC Data Interface (default, inactive) */ \
    9, TUSB_DESC_INTERFACE, (uint8_t)((ncm_itf) + 1), 0, 0, TUSB_CLASS_CDC_DATA, 0, \
        NCM_DATA_PROTOCOL_NETWORK_TRANSFER_BLOCK, 0, \
    /* CDC Data Interface (alternative, active) */ \
    9, TUSB_DESC_INTERFACE, (uint8_t)((ncm_itf) + 1), 1, 2, TUSB_CLASS_CDC_DATA, 0, \
        NCM_DATA_PROTOCOL_NETWORK_TRANSFER_BLOCK, 0, \
    /* Endpoint In (bulk 0x86) */ \
    7, TUSB_DESC_ENDPOINT, 0x86, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0, \
    /* Endpoint Out (bulk 0x05) */ \
    7, TUSB_DESC_ENDPOINT, 0x05, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0
#endif // ENABLE_WEBCONFIG

// DualSense gamepad HID interface block, parameterised by interface number and
// endpoint pair. 32 bytes (CONFIG_DESC_LEN_GAMEPAD): 9 (interface) + 9 (HID
// class) + 7 (EP IN) + 7 (EP OUT). wDescriptorLength defaults to the DS value
// (0x0111) and both it and the EP bIntervals are patched at descriptor-fetch
// time in tud_descriptor_configuration_cb() — the patch loop walks blocks by
// this fixed size, so keep the layout in sync with the offsets there.
#define DS5_GAMEPAD_ITF_DESC(itf, ep_in, ep_out) \
    /* Interface: HID gamepad, 2 endpoints */ \
    0x09, 0x04, (itf), 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, \
    /* HID descriptor: bcdHID 1.11, report descriptor length 0x0111 (DS) */ \
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x11, 0x01, \
    /* Endpoint IN: interrupt, 64 bytes, bInterval 1 */ \
    0x07, 0x05, (ep_in), 0x03, 0x40, 0x00, 0x01, \
    /* Endpoint OUT: interrupt, 64 bytes, bInterval 1 */ \
    0x07, 0x05, (ep_out), 0x03, 0x40, 0x00, 0x01

#ifdef ENABLE_WAKE_HID
// Boot-keyboard interface (HID), parameterised by interface number. EP IN 0x87.
// 25 bytes: 9 (interface) + 9 (HID class) + 7 (EP IN).
#define DS5_KBD_ITF_DESC(kbd_itf) \
    0x09, 0x04, (kbd_itf), 0x00, 0x01, 0x03, 0x01, 0x01, 0x00, \
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x2D, 0x00, \
    0x07, 0x05, 0x87, 0x03, 0x08, 0x00, 0x0A

// Inert dummy HID interface used in MINIMAL where the gamepad sits in FULL.
// Holds HID instance 0 so the keyboard stays HID instance 1 across variants
// (the structural "rogue keyboard on wake" fix). Reuses EP IN 0x84; never
// written. wDescriptorLength 21 = sizeof(desc_hid_report_dummy). 25 bytes.
#define DS5_DUMMY_HID_ITF_DESC(dummy_itf) \
    0x09, 0x04, (dummy_itf), 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, \
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x15, 0x00, \
    0x07, 0x05, 0x84, 0x03, 0x40, 0x00, 0x0A

// Inert padding occupying MINIMAL's interfaces 0..2 so NCM lands on the same
// number (4) as in FULL. The three vendor-specific (0xFF), zero-endpoint
// interfaces are grouped under ONE Interface Association Descriptor so the
// Windows composite parent (usbccgp) creates a single child function for them
// instead of collapsing the consecutive same-class interfaces into one unnamed
// unknown device. A WinUSB compatible-ID on the IAD's bFirstInterface (see the
// MINIMAL-only desc_ms_os_20_minimal) then binds that function to WinUSB -> no
// yellow bang. That tag is MINIMAL-only on purpose: in FULL interface 0 is audio
// and must NOT be tagged WinUSB. 8 (IAD) + 3*9 (interfaces) = 35 bytes.
#define DS5_INERT_PAD_DESC(first_itf) \
    /* IAD: groups the 3 inert vendor interfaces into one function */ \
    0x08, TUSB_DESC_INTERFACE_ASSOCIATION, (first_itf), 0x03, 0xFF, 0x00, 0x00, 0x00, \
    0x09, 0x04, (first_itf),       0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, \
    0x09, 0x04, (first_itf) + 1,   0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, \
    0x09, 0x04, (first_itf) + 2,   0x00, 0x00, 0xFF, 0x00, 0x00, 0x00
#define DS5_INERT_PAD_DESC_LEN (8 + 3 * 9)
#endif // ENABLE_WAKE_HID

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t desc_device =
{
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
#ifdef ENABLE_WAKE_HID
    .bcdUSB = 0x0210, // USB 2.1 -- required so the host requests BOS (carries our MS OS 2.0 descriptor)
#else
    .bcdUSB = 0x0200,
#endif

    // Use Interface Association Descriptor (IAD) for Audio.
    // As required by USB Specs IAD's subclass must be common class (2) and protocol must be IAD (1).
    // The FULL variant carries the audio + CDC-NCM IADs, so webconfig builds need
    // the Misc/common/IAD device class.
#if defined(ENABLE_WEBCONFIG)
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
#else
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
#endif
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = 0x054C,
    // .idProduct = 0x0CE6, // DS
    // .idProduct = 0x0DF2, // DSE
    .bcdDevice = 0x0100,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x00,

    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb(void) {
    desc_device.idProduct = ds_mode() ? 0x0CE6 : 0x0DF2;
    return reinterpret_cast<uint8_t const *>(&desc_device);
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
uint8_t descriptor_configuration[] = {
    // --- CONFIGURATION DESCRIPTOR ---
    0x09, // bLength
    0x02, // bDescriptorType (CONFIGURATION)
    U16_TO_U8S_LE(CONFIG_DESC_LEN_TOTAL), // wTotalLength
    ITF_NUM_TOTAL, // bNumInterfaces
    0x01, // bConfigurationValue: 1
    0x00, // iConfiguration: 0
#ifdef ENABLE_WAKE_HID
    0xE0, // bmAttributes: SELF-POWERED + REMOTE-WAKEUP
#else
    0xC0, // bmAttributes: SELF-POWERED, NO REMOTE-WAKEUP
#endif
    0xFA, // bMaxPower: 500mA (250 * 2mA)

#if defined(ENABLE_WEBCONFIG)
    // --- INTERFACE ASSOCIATION DESCRIPTOR: Audio function (interfaces 0-2) ---
    // Required because the CDC-NCM function (also IAD) is present, so the audio
    // function must be properly grouped under its own IAD.
    0x08, // bLength
    TUSB_DESC_INTERFACE_ASSOCIATION, // bDescriptorType
    ITF_NUM_AUDIO_CONTROL, // bFirstInterface
    0x03, // bInterfaceCount
    0x01, // bFunctionClass: Audio
    0x01, // bFunctionSubClass: Audio Control
    0x00, // bFunctionProtocol
    0x00, // iFunction

#endif
    // --- INTERFACE DESCRIPTOR (0.0): Audio Control ---
    0x09, // bLength
    0x04, // bDescriptorType (INTERFACE)
    0x00, // bInterfaceNumber: 0
    0x00, // bAlternateSetting: 0
    0x00, // bNumEndpoints: 0
    0x01, // bInterfaceClass: Audio (0x01)
    0x01, // bInterfaceSubClass: Audio Control (0x01)
    0x00, // bInterfaceProtocol: 0x00
    0x00, // iInterface: 0

    // Class-specific AC Interface Header Descriptor
    0x0A, // bLength: 10
    0x24, // bDescriptorType: CS_INTERFACE (0x24)
    0x01, // bDescriptorSubtype: Header (0x01)
    0x00, 0x01, // bcdADC: 1.00
    0x49, 0x00, // wTotalLength: 73 (0x0049)
    0x02, // bInCollection: 2 streaming interfaces
    0x01, // baInterfaceNr(1): Interface 1
    0x02, // baInterfaceNr(2): Interface 2

    // Input Terminal Descriptor (Terminal ID 1: USB Streaming → Output to Speaker)
    0x0C, // bLength: 12
    0x24, // bDescriptorType: CS_INTERFACE
    0x02, // bDescriptorSubtype: Input Terminal
    0x01, // bTerminalID: 1
    0x01, 0x01, // wTerminalType: USB Streaming (0x0101)
    0x06, // bAssocTerminal: 6 (paired with USB OUT terminal)
    0x04, // bNrChannels: 4
    0x33, 0x00, // wChannelConfig: L/R Front + L/R Surround (0x0033)
    0x00, // iChannelNames: 0
    0x00, // iTerminal: 0

    // Feature Unit Descriptor (Unit ID 2 ← from Terminal 1)
    0x0C, // bLength: 12
    0x24, // bDescriptorType: CS_INTERFACE
    0x06, // bDescriptorSubtype: Feature Unit
    0x02, // bUnitID: 2
    0x01, // bSourceID: 1
    0x01, // bControlSize: 1 byte per control
    0x03, // bmaControls[0]: Master – Mute, Volume
    0x00, 0x00, 0x00, 0x00, 0x00, // bmaControls[1..4]: No per-channel controls

    // Output Terminal Descriptor (Terminal ID 3: Speaker ← from Unit 2)
    0x09, // bLength: 9
    0x24, // bDescriptorType: CS_INTERFACE
    0x03, // bDescriptorSubtype: Output Terminal
    0x03, // bTerminalID: 3
    0x01, 0x03, // wTerminalType: Speaker (0x0301)
    0x04, // bAssocTerminal: 4 (paired with mic input)
    0x02, // bSourceID: 2 (Feature Unit)
    0x00, // iTerminal: 0

    // Input Terminal Descriptor (Terminal ID 4: Headset Mic)
    0x0C, // bLength: 12
    0x24, // bDescriptorType: CS_INTERFACE
    0x02, // bDescriptorSubtype: Input Terminal
    0x04, // bTerminalID: 4
    0x02, 0x04, // wTerminalType: Headset (0x0402)
    0x03, // bAssocTerminal: 3 (paired with speaker)
    0x02, // bNrChannels: 2
    0x03, 0x00, // wChannelConfig: L/R Front (0x0003)
    0x00, // iChannelNames: 0
    0x00, // iTerminal: 0

    // Feature Unit Descriptor (Unit ID 5 ← from Terminal 4)
    0x09, // bLength: 9
    0x24, // bDescriptorType: CS_INTERFACE
    0x06, // bDescriptorSubtype: Feature Unit
    0x05, // bUnitID: 5
    0x04, // bSourceID: 4
    0x01, // bControlSize: 1
    0x03, // bmaControls[0]: Master – Mute, Volume
    0x00, // bmaControls[1]: Ch1 – no controls
    0x00, // iFeature: 0

    // Output Terminal Descriptor (Terminal ID 6: USB Streaming ← from Unit 5)
    0x09, // bLength: 9
    0x24, // bDescriptorType: CS_INTERFACE
    0x03, // bDescriptorSubtype: Output Terminal
    0x06, // bTerminalID: 6
    0x01, 0x01, // wTerminalType: USB Streaming (0x0101)
    0x01, // bAssocTerminal: 1
    0x05, // bSourceID: 5
    0x00, // iTerminal: 0

    // --- INTERFACE DESCRIPTOR (1.0): Audio Streaming (OUT - Alternate 0) ---
    0x09, // bLength
    0x04, // bDescriptorType (INTERFACE)
    0x01, // bInterfaceNumber: 1
    0x00, // bAlternateSetting: 0
    0x00, // bNumEndpoints: 0
    0x01, // bInterfaceClass: Audio
    0x02, // bInterfaceSubClass: Audio Streaming
    0x00, // bInterfaceProtocol
    0x00, // iInterface

    // --- INTERFACE DESCRIPTOR (1.1): Audio Streaming (OUT - Alternate 1) ---
    0x09, // bLength
    0x04, // bDescriptorType (INTERFACE)
    0x01, // bInterfaceNumber: 1
    0x01, // bAlternateSetting: 1
    0x01, // bNumEndpoints: 1
    0x01, // bInterfaceClass: Audio
    0x02, // bInterfaceSubClass: Audio Streaming
    0x00, // bInterfaceProtocol
    0x00, // iInterface

    // AS General Descriptor (for Interface 1.1)
    0x07, // bLength: 7
    0x24, // bDescriptorType: CS_INTERFACE
    0x01, // bDescriptorSubtype: AS_GENERAL
    0x01, // bTerminalLink: connected to Terminal ID 1
    0x01, // bDelay: 1 frame
    0x01, 0x00, // wFormatTag: PCM (0x0001)

    // Format Type Descriptor (4-channel, 16-bit, 48kHz)
    0x0B, // bLength: 11
    0x24, // bDescriptorType: CS_INTERFACE
    0x02, // bDescriptorSubtype: FORMAT_TYPE
    0x01, // bFormatType: TYPE_I
    0x04, // bNrChannels: 4
    0x02, // bSubframeSize: 2 bytes/sample
    0x10, // bBitResolution: 16 bits
    0x01, // bSamFreqType: 1 discrete frequency
    0x80, 0xBB, 0x00, // tSamFreq: 48000 Hz (0x00BB80)

    // Endpoint Descriptor (Audio OUT: EP1)
    0x09, // bLength
    0x05, // bDescriptorType (ENDPOINT)
    0x01, // bEndpointAddress: OUT EP1
    0x09, // bmAttributes: Isochronous, Adaptive
    0x88, 0x01, // wMaxPacketSize: 392 bytes
    0x01, // bInterval: 1
    0x00, // bRefresh
    0x00, // bSynchAddress

    // Class-specific Audio Streaming Endpoint Descriptor (EP1)
    0x07, // bLength
    0x25, // bDescriptorType: CS_ENDPOINT
    0x01, // bDescriptorSubtype: GENERAL
    0x00, // Attributes: No pitch/sampling freq control
    0x00, // Lock Delay Units: Undefined
    0x00, 0x00, // Lock Delay: 0

    // --- INTERFACE DESCRIPTOR (2.0): Audio Streaming IN (Alternate 0) ---
    0x09, // bLength
    0x04, // bDescriptorType (INTERFACE)
    0x02, // bInterfaceNumber: 2
    0x00, // bAlternateSetting: 0
    0x00, // bNumEndpoints: 0
    0x01, // bInterfaceClass: Audio
    0x02, // bInterfaceSubClass: Audio Streaming
    0x00, // bInterfaceProtocol
    0x00, // iInterface

    // --- INTERFACE DESCRIPTOR (2.1): Audio Streaming IN (Alternate 1) ---
    0x09, // bLength
    0x04, // bDescriptorType (INTERFACE)
    0x02, // bInterfaceNumber: 2
    0x01, // bAlternateSetting: 1
    0x01, // bNumEndpoints: 1
    0x01, // bInterfaceClass: Audio
    0x02, // bInterfaceSubClass: Audio Streaming
    0x00, // bInterfaceProtocol
    0x00, // iInterface

    // AS General Descriptor (for Interface 2.1)
    0x07, // bLength: 7
    0x24, // bDescriptorType: CS_INTERFACE
    0x01, // bDescriptorSubtype: AS_GENERAL
    0x06, // bTerminalLink: connected to Terminal ID 6
    0x01, // bDelay: 1 frame
    0x01, 0x00, // wFormatTag: PCM (0x0001)

    // Format Type Descriptor (2-channel, 16-bit, 48kHz)
    0x0B, // bLength: 11
    0x24, // bDescriptorType: CS_INTERFACE
    0x02, // bDescriptorSubtype: FORMAT_TYPE
    0x01, // bFormatType: TYPE_I
    0x02, // bNrChannels: 2
    0x02, // bSubframeSize: 2
    0x10, // bBitResolution: 16
    0x01, // bSamFreqType: 1
    0x80, 0xBB, 0x00, // tSamFreq: 48000 Hz

    // Endpoint Descriptor (Audio IN: EP2)
    0x09, // bLength
    0x05, // bDescriptorType (ENDPOINT)
    0x82, // bEndpointAddress: IN EP2
    0x05, // bmAttributes: Isochronous, Asynchronous
    0xC4, 0x00, // wMaxPacketSize: 196 bytes (48kHz × 2ch × 2B)
    0x01, // bInterval: 1
    0x00, // bRefresh
    0x00, // bSynchAddress

    // Class-specific Audio Streaming Endpoint Descriptor (EP2)
    0x07, // bLength
    0x25, // bDescriptorType: CS_ENDPOINT
    0x01, // bDescriptorSubtype: GENERAL
    0x00, // Attributes: No controls
    0x00, // Lock Delay Units
    0x00, 0x00, // Lock Delay

    // --- INTERFACE DESCRIPTOR (3): HID DualSense gamepad, slot 0. Upstream
    // interface number and endpoints (IN 0x84 / OUT 0x03); wDescriptorLength
    // (DS vs DSE) and the EP bIntervals (polling rate) are patched at fetch
    // time for every gamepad block.
    DS5_GAMEPAD_ITF_DESC(ITF_NUM_HID, 0x84, 0x03),

#ifdef ENABLE_WEBCONFIG
    // --- CDC-NCM (config web UI network interface) at ITF_NUM_NET / +1 ---
    // Same bytes (via DS5_NCM_DESC) as the MINIMAL variant, so NCM keeps the
    // same bInterfaceNumber across the variant swap and the host sees one
    // persistent network adapter rather than two.
    DS5_NCM_DESC(ITF_NUM_NET),
#endif
#ifdef ENABLE_WAKE_HID
    // --- HID Boot Keyboard (wake key only) at ITF_NUM_HID_KBD ---
    DS5_KBD_ITF_DESC(ITF_NUM_HID_KBD),
#endif

    // --- Extra gamepad interfaces (slots 1..N-1), appended last so the
    // descriptor can be truncated to the exposed-slot count without moving
    // any base interface. HID instances follow parse order: slot 0 = 0,
    // keyboard = 1, slot k>=1 = k+1 (see usb_slot_hid_instance in usb.h).
#if MULTI_SLOT_COUNT >= 2
    DS5_GAMEPAD_ITF_DESC(ITF_NUM_BASE_TOTAL + 0, 0x88, 0x08),
#endif
#if MULTI_SLOT_COUNT >= 3
    DS5_GAMEPAD_ITF_DESC(ITF_NUM_BASE_TOTAL + 1, 0x89, 0x09),
#endif
#if MULTI_SLOT_COUNT >= 4
    DS5_GAMEPAD_ITF_DESC(ITF_NUM_BASE_TOTAL + 2, 0x8A, 0x0A),
#endif
};

// Lock the hand-computed wTotalLength against the actual emitted bytes. A
// mismatch (e.g. CONFIG_DESC_LEN_NET not matching the NCM descriptor) would
// silently break enumeration of the trailing interfaces.
static_assert(sizeof(descriptor_configuration) == CONFIG_DESC_LEN_TOTAL,
              "descriptor_configuration size != CONFIG_DESC_LEN_TOTAL");

#ifdef ENABLE_WAKE_HID
// Minimal config descriptor used when no DualSense is connected.
// Presents no audio function and no real gamepad — so the Windows Sound applet
// and joy.cpl don't show ghost devices — while keeping the dongle enumerated
// and remote-wakeup-capable (needed for wake-from-S3/S5).
//
// It keeps the keyboard at HID instance 1 in BOTH variants. TinyUSB numbers HID
// instances by descriptor parse order, counting only HID interfaces; in FULL the
// gamepad is HID instance 0 and the kbd is instance 1. MINIMAL therefore needs
// exactly ONE HID before the kbd (a dummy placeholder) so the kbd stays instance
// 1 -- without that the kbd became instance 0 in MINIMAL and a gamepad report
// (addressed to instance 0/1) could land on it across a swap: the "rogue
// keyboard on wake". The dummy reuses the gamepad's IN endpoint 0x84; it is never
// written to.
//
// Interface layout depends on ENABLE_WEBCONFIG:
//
//   With ENABLE_WEBCONFIG (the release config) MINIMAL also carries the CDC-NCM
//   network interface so the config web page is reachable even when no controller
//   is connected. NCM MUST keep the same bInterfaceNumber as in FULL (4-5), else
//   the host creates a second network adapter on every variant swap. FULL has
//   audio(0-2)+gamepad(3) before NCM, so MINIMAL pads interfaces 0-2 with inert
//   vendor interfaces and puts the dummy HID at interface 3 (the gamepad's slot):
//       0-2 inert vendor (WinUSB compat-id -> no driver, no yellow bang)
//       3   dummy HID            (HID instance 0)
//       4-5 CDC-NCM              (same number as FULL)
//       6   boot keyboard        (HID instance 1)
//
//   Without ENABLE_WEBCONFIG there is no NCM, so the minimal layout is just:
//       0   dummy HID            (HID instance 0)
//       1   boot keyboard        (HID instance 1)
//
// FULL's interface order is canonical/frozen (matches a real DualSense); the
// padding lives entirely in MINIMAL. Interface numbers stay ascending, so Windows
// accepts the config (it rejects out-of-order interfaces).
#ifdef ENABLE_WEBCONFIG
// MINIMAL interface numbers (mirror FULL up to and including NCM).
enum {
    MIN_ITF_INERT0 = 0,
    MIN_ITF_INERT1,
    MIN_ITF_INERT2,
    MIN_ITF_DUMMY_HID,   // 3 -- slot 0 gamepad's seat in FULL; HID instance 0
    MIN_ITF_NET,         // 4 -- NCM control (same as FULL ITF_NUM_NET)
    MIN_ITF_NET_DATA,    // 5
    MIN_ITF_HID_KBD,     // 6 -- HID instance 1
    MIN_ITF_TOTAL
};
static_assert(MIN_ITF_NET == ITF_NUM_NET && MIN_ITF_HID_KBD == ITF_NUM_HID_KBD,
              "MINIMAL must keep NCM/kbd on the same interface numbers as FULL");
//   Config descriptor                 9
//   Inert pad (IAD + 3 vendor itfs)   DS5_INERT_PAD_DESC_LEN (35)
//   Dummy HID interface + HID + EP    25
//   CDC-NCM block                     TUD_CDC_NCM_DESC_LEN (85)
//   Kbd interface + HID + EP          25
#define CONFIG_DESC_LEN_MINIMAL (9 + DS5_INERT_PAD_DESC_LEN + 25 \
                                 + TUD_CDC_NCM_DESC_LEN + 25)
uint8_t descriptor_configuration_minimal[CONFIG_DESC_LEN_MINIMAL] = {
    // --- CONFIGURATION DESCRIPTOR ---
    0x09, // bLength
    0x02, // bDescriptorType (CONFIGURATION)
    U16_TO_U8S_LE(CONFIG_DESC_LEN_MINIMAL), // wTotalLength
    MIN_ITF_TOTAL, // bNumInterfaces
    0x01, // bConfigurationValue: 1
    0x00, // iConfiguration: 0
    0xE0, // bmAttributes: SELF-POWERED + REMOTE-WAKEUP (must keep for wake)
    0xFA, // bMaxPower: 500mA

    // Interfaces 0-2: inert vendor padding (one IAD-grouped WinUSB function) so
    // NCM lands on interface 4-5 as in FULL.
    DS5_INERT_PAD_DESC(MIN_ITF_INERT0),
    // Interface 3: dummy HID placeholder (HID instance 0, keeps kbd at instance 1).
    DS5_DUMMY_HID_ITF_DESC(MIN_ITF_DUMMY_HID),
    // Interfaces 4-5: CDC-NCM, SAME bytes/number as FULL (single source DS5_NCM_DESC).
    DS5_NCM_DESC(MIN_ITF_NET),
    // Interface 6: boot keyboard (HID instance 1).
    DS5_KBD_ITF_DESC(MIN_ITF_HID_KBD),
};
#else // ENABLE_WAKE_HID && !ENABLE_WEBCONFIG -- legacy kbd-only minimal
#define CONFIG_DESC_LEN_MINIMAL (9 + 25 + 25)
uint8_t descriptor_configuration_minimal[CONFIG_DESC_LEN_MINIMAL] = {
    // --- CONFIGURATION DESCRIPTOR ---
    0x09, 0x02, U16_TO_U8S_LE(CONFIG_DESC_LEN_MINIMAL),
    0x02, // bNumInterfaces: dummy + kbd
    0x01, 0x00,
    0xE0, // SELF-POWERED + REMOTE-WAKEUP (must keep for wake)
    0xFA,
    // Interface 0: dummy HID (HID instance 0).
    DS5_DUMMY_HID_ITF_DESC(0),
    // Interface 1: boot keyboard (HID instance 1).
    DS5_KBD_ITF_DESC(1),
};
#endif
static_assert(sizeof(descriptor_configuration_minimal) == CONFIG_DESC_LEN_MINIMAL,
              "descriptor_configuration_minimal size mismatch");

// Runtime selector: which variant to present on the next GET_CONFIGURATION.
// Updated by usb_set_descriptor_variant() (from BT connect/disconnect),
// read by tud_descriptor_configuration_cb() when the host re-enumerates
// after a tud_disconnect()/tud_connect() cycle.
typedef enum {
    DESC_VARIANT_MINIMAL = 0, // kbd only
    DESC_VARIANT_FULL,        // audio + gamepad + kbd
} desc_variant_t;
static volatile desc_variant_t active_variant = DESC_VARIANT_MINIMAL;

void usb_set_descriptor_variant_full(void)    { active_variant = DESC_VARIANT_FULL; }
void usb_set_descriptor_variant_minimal(void) { active_variant = DESC_VARIANT_MINIMAL; }
bool usb_descriptor_variant_is_full(void)     { return active_variant == DESC_VARIANT_FULL; }
// The boot keyboard is HID instance 1 in BOTH variants: in FULL slot 0's
// gamepad is instance 0 (parsed first) and the EXTRA gamepad interfaces come
// after the keyboard (instances 2..N), and MINIMAL keeps a dummy HID at
// instance 0 so the kbd stays instance 1. Stable across variant swaps -- this
// is what makes the "rogue keyboard on wake" structurally impossible.
uint8_t usb_kbd_hid_instance(void) { return 1; }

//--------------------------------------------------------------------+
// Variant swap orchestrator
//--------------------------------------------------------------------+
// State machine that drives a USB re-enumeration when the desired
// descriptor variant differs from the active one. Runs from the main
// loop via usb_variant_task().
//
// Sequence:
//   IDLE     — desired == active, nothing to do.
//   DISCONNECTING — called tud_disconnect(); wait SETTLE_US so the host
//                   sees the disconnect cleanly before we present a
//                   different descriptor.
//   CONNECTING    — flipped active_variant, called tud_connect(); wait
//                   for host re-enumeration to settle, then back to IDLE.
//
// Refuses to start or continue a swap while the host is suspended: a
// re-enumeration mid-suspend defeats the whole point of ENABLE_WAKE_HID
// (the dongle needs to be enumerated when the host wakes so remote-
// wakeup can fire).

#include "pico/time.h"
#include "wake.h"

static volatile desc_variant_t desired_variant = DESC_VARIANT_MINIMAL;
static volatile bool host_suspended_flag = false;

typedef enum {
    SWAP_IDLE,
    SWAP_DISCONNECTING,
    SWAP_CONNECTING,
} swap_state_t;

static swap_state_t  swap_state         = SWAP_IDLE;
static uint64_t      swap_state_entered = 0;
static constexpr uint64_t SWAP_DISCONNECT_SETTLE_US = 500000;  // 500 ms
static constexpr uint64_t SWAP_CONNECT_SETTLE_US    = 1500000; // 1500 ms

void usb_request_variant_full(void)    { desired_variant = DESC_VARIANT_FULL; }
void usb_request_variant_minimal(void) { desired_variant = DESC_VARIANT_MINIMAL; }
void usb_set_host_suspended(bool s)    { host_suspended_flag = s; }
bool usb_variant_swap_in_progress(void) { return swap_state != SWAP_IDLE; }

// Cold-boot autosuspend recovery (issue #4). While the gate below holds a
// pending UP-swap (MINIMAL->FULL) shut, periodically re-issue a USB bus resume
// to coax the host into re-mounting us (which fires tud_resume_cb/tud_mount_cb
// -> clears the gate). Rate-limited so we don't spam resume signaling.
//
// CRITICAL: only the MINIMAL->FULL direction is nudged. The DOWN-swap
// (FULL->MINIMAL, requested when the controller disconnects) can legitimately
// be pending while the host is in a genuine S3 suspend -- a DS5 that powers
// itself off after the host sleeps leaves desired=MINIMAL, active=FULL. Forcing
// a resume there would wake the sleeping host, the exact thing the gate exists
// to prevent. The UP-swap only ever happens right after a controller connects,
// which is precisely when we DO want the bus back up so the gamepad appears.
static constexpr uint64_t SWAP_GATE_RESUME_RETRY_US = 1000000; // 1 s
static uint64_t swap_gate_last_resume_us = 0;

void usb_variant_task(void) {
    if (host_suspended_flag) {
        // Never re-enumerate during host suspend. But if an UP-swap to FULL is
        // pending and the host has us suspended, keep nudging the bus back up so
        // the gate can clear -- otherwise a host that suspends MINIMAL and never
        // re-mounts strands the controller in MINIMAL forever (issue #4).
        if (desired_variant == DESC_VARIANT_FULL && active_variant == DESC_VARIANT_MINIMAL) {
            const uint64_t now = time_us_64();
            if (now - swap_gate_last_resume_us >= SWAP_GATE_RESUME_RETRY_US) {
                swap_gate_last_resume_us = now;
                wake_request_bus_resume();
            }
        }
        return;
    }
    const uint64_t now = time_us_64();
    switch (swap_state) {
        case SWAP_IDLE:
            if (desired_variant != active_variant) {
                wake_reset_for_variant_swap();
                tud_disconnect();
                swap_state = SWAP_DISCONNECTING;
                swap_state_entered = now;
            }
            return;
        case SWAP_DISCONNECTING:
            if (now - swap_state_entered < SWAP_DISCONNECT_SETTLE_US) return;
            active_variant = desired_variant;
            tud_connect();
            swap_state = SWAP_CONNECTING;
            swap_state_entered = now;
            return;
        case SWAP_CONNECTING:
            if (now - swap_state_entered < SWAP_CONNECT_SETTLE_US) return;
            swap_state = SWAP_IDLE;
            return;
    }
}
#endif // ENABLE_WAKE_HID

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void) index; // for multiple configurations
#ifdef ENABLE_WAKE_HID
    if (active_variant == DESC_VARIANT_MINIMAL) {
        return descriptor_configuration_minimal;
    }
#endif
    auto bInterval = 0x01;
    switch (get_config().polling_rate_mode) {
        case 0:
            bInterval = 0x04;
            break;
        case 1:
            bInterval = 0x02;
            break;
        case 2:
            bInterval = 0x01;
            break;
    }
    // Patch every gamepad block.
    // Slot 0's block ends at CONFIG_DESC_LEN_BASE; slot j>=1's block ends
    // after the base set + j blocks. Within a block (relative to its end):
    // -1 = EP OUT bInterval, -8 = EP IN bInterval, -16 = wDescriptorLength
    // low byte (DS 0x0111 vs DSE 0x0185; the high bytes 0x01 coincide). All
    // interfaces share the device's DS/DSE identity — one VID/PID for the
    // whole composite, chosen by ds_mode().
    const uint8_t report_len_lo = ds_mode() ? 0x11 : 0x85;
    constexpr size_t base_set_end = CONFIG_DESC_LEN_BASE + CONFIG_DESC_LEN_NET
        + CONFIG_DESC_LEN_WAKE_KBD;
    for (int slot = 0; slot < MULTI_SLOT_COUNT; slot++) {
        const size_t end = slot == 0
            ? (size_t) CONFIG_DESC_LEN_BASE
            : base_set_end + (size_t) slot * CONFIG_DESC_LEN_GAMEPAD;
        descriptor_configuration[end - 1] = bInterval;
        descriptor_configuration[end - 8] = bInterval;
        descriptor_configuration[end - 16] = report_len_lo;
    }
    return descriptor_configuration;
}

//--------------------------------------------------------------------+
// HID Report Descriptor
//--------------------------------------------------------------------+

uint8_t const desc_hid_report_ds[] = {
    0x05, 0x01, // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05, // Usage (Game Pad)
    0xA1, 0x01, // Collection (Application)
    0x85, 0x01, //   Report ID (1)
    0x09, 0x30, //   Usage (X)
    0x09, 0x31, //   Usage (Y)
    0x09, 0x32, //   Usage (Z)
    0x09, 0x35, //   Usage (Rz)
    0x09, 0x33, //   Usage (Rx)
    0x09, 0x34, //   Usage (Ry)
    0x15, 0x00, //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x06, //   Report Count (6)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x20, //   Usage (0x20)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x01, //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x39, //   Usage (Hat switch)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x07, //   Logical Maximum (7)
    0x35, 0x00, //   Physical Minimum (0)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x65, 0x14, //   Unit (System: English Rotation, Length: Centimeter)
    0x75, 0x04, //   Report Size (4)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x42, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,Null State)
    0x65, 0x00, //   Unit (None)
    0x05, 0x09, //   Usage Page (Button)
    0x19, 0x01, //   Usage Minimum (0x01)
    0x29, 0x0F, //   Usage Maximum (0x0F)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x0F, //   Report Count (15)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x21, //   Usage (0x21)
    0x95, 0x0D, //   Report Count (13)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x22, //   Usage (0x22)
    0x15, 0x00, //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x34, //   Report Count (52)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x85, 0x02, //   Report ID (2)
    0x09, 0x23, //   Usage (0x23)
    0x95, 0x2F, //   Report Count (47)
    0x91, 0x02, //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x05, //   Report ID (5)
    0x09, 0x33, //   Usage (0x33)
    0x95, 0x28, //   Report Count (40)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x08, //   Report ID (8)
    0x09, 0x34, //   Usage (0x34)
    0x95, 0x2F, //   Report Count (47)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x09, //   Report ID (9)
    0x09, 0x24, //   Usage (0x24)
    0x95, 0x13, //   Report Count (19)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x0A, //   Report ID (10)
    0x09, 0x25, //   Usage (0x25)
    0x95, 0x1A, //   Report Count (26)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    // Report IDs 11/12 (usages 0x41/0x42, 41-byte feature reports) removed: a
    // genuine DualSense / DualSense Edge jumps 0x0A -> 0x20 here (confirmed
    // against real-device descriptor dumps). These were inherited from
    // upstream's base descriptor and never serviced by the firmware; removing
    // them makes the report-ID set match real hardware exactly. -16 bytes each.
    0x85, 0x20, //   Report ID (32)
    0x09, 0x26, //   Usage (0x26)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x21, //   Report ID (33)
    0x09, 0x27, //   Usage (0x27)
    0x95, 0x04, //   Report Count (4)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x22, //   Report ID (34)
    0x09, 0x40, //   Usage (0x40)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x80, //   Report ID (-128)
    0x09, 0x28, //   Usage (0x28)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x81, //   Report ID (-127)
    0x09, 0x29, //   Usage (0x29)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x82, //   Report ID (-126)
    0x09, 0x2A, //   Usage (0x2A)
    0x95, 0x09, //   Report Count (9)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x83, //   Report ID (-125)
    0x09, 0x2B, //   Usage (0x2B)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x84, //   Report ID (-124)
    0x09, 0x2C, //   Usage (0x2C)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x85, //   Report ID (-123)
    0x09, 0x2D, //   Usage (0x2D)
    0x95, 0x02, //   Report Count (2)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xA0, //   Report ID (-96)
    0x09, 0x2E, //   Usage (0x2E)
    0x95, 0x01, //   Report Count (1)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xE0, //   Report ID (-32)
    0x09, 0x2F, //   Usage (0x2F)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF0, //   Report ID (-16)
    0x09, 0x30, //   Usage (0x30)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF1, //   Report ID (-15)
    0x09, 0x31, //   Usage (0x31)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF2, //   Report ID (-14)
    0x09, 0x32, //   Usage (0x32)
    0x95, 0x0F, //   Report Count (15)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF4, //   Report ID (-12)
    0x09, 0x35, //   Usage (0x35)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF5, //   Report ID (-11)
    0x09, 0x36, //   Usage (0x36)
    0x95, 0x03, //   Report Count (3)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    // Report IDs 0xF6-0xF9 (vendor usages 0x37-0x3A) were the old WebHID config
    // command channel; removed (config is served over the NCM web page now).
    0xC0, // End Collection
    // 289 bytes
};
static_assert(sizeof(desc_hid_report_ds) == 0x0111);

uint8_t const desc_hid_report_dse[] = {
    0x05, 0x01, // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05, // Usage (Game Pad)
    0xA1, 0x01, // Collection (Application)
    0x85, 0x01, //   Report ID (1)
    0x09, 0x30, //   Usage (X)
    0x09, 0x31, //   Usage (Y)
    0x09, 0x32, //   Usage (Z)
    0x09, 0x35, //   Usage (Rz)
    0x09, 0x33, //   Usage (Rx)
    0x09, 0x34, //   Usage (Ry)
    0x15, 0x00, //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x06, //   Report Count (6)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x20, //   Usage (0x20)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x01, //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x39, //   Usage (Hat switch)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x07, //   Logical Maximum (7)
    0x35, 0x00, //   Physical Minimum (0)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x65, 0x14, //   Unit (System: English Rotation, Length: Centimeter)
    0x75, 0x04, //   Report Size (4)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x42, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,Null State)
    0x65, 0x00, //   Unit (None)
    0x05, 0x09, //   Usage Page (Button)
    0x19, 0x01, //   Usage Minimum (0x01)
    0x29, 0x0F, //   Usage Maximum (0x0F)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x0F, //   Report Count (15)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x21, //   Usage (0x21)
    0x95, 0x0D, //   Report Count (13)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x22, //   Usage (0x22)
    0x15, 0x00, //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x34, //   Report Count (52)
    0x81, 0x02, //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x85, 0x02, //   Report ID (2)
    0x09, 0x23, //   Usage (0x23)
    0x95, 0x3F, //   Report Count (63)
    0x91, 0x02, //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x05, //   Report ID (5)
    0x09, 0x33, //   Usage (0x33)
    0x95, 0x28, //   Report Count (40)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x08, //   Report ID (8)
    0x09, 0x34, //   Usage (0x34)
    0x95, 0x2F, //   Report Count (47)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x09, //   Report ID (9)
    0x09, 0x24, //   Usage (0x24)
    0x95, 0x13, //   Report Count (19)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x0A, //   Report ID (10)
    0x09, 0x25, //   Usage (0x25)
    0x95, 0x1A, //   Report Count (26)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    // Report IDs 11/12 (usages 0x41/0x42, 41-byte feature reports) removed: a
    // genuine DualSense / DualSense Edge jumps 0x0A -> 0x20 here (confirmed
    // against real-device descriptor dumps). These were inherited from
    // upstream's base descriptor and never serviced by the firmware; removing
    // them makes the report-ID set match real hardware exactly. -16 bytes each.
    0x85, 0x20, //   Report ID (32)
    0x09, 0x26, //   Usage (0x26)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x21, //   Report ID (33)
    0x09, 0x27, //   Usage (0x27)
    0x95, 0x04, //   Report Count (4)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x22, //   Report ID (34)
    0x09, 0x40, //   Usage (0x40)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x80, //   Report ID (-128)
    0x09, 0x28, //   Usage (0x28)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x81, //   Report ID (-127)
    0x09, 0x29, //   Usage (0x29)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x82, //   Report ID (-126)
    0x09, 0x2A, //   Usage (0x2A)
    0x95, 0x09, //   Report Count (9)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x83, //   Report ID (-125)
    0x09, 0x2B, //   Usage (0x2B)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x84, //   Report ID (-124)
    0x09, 0x2C, //   Usage (0x2C)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x85, //   Report ID (-123)
    0x09, 0x2D, //   Usage (0x2D)
    0x95, 0x02, //   Report Count (2)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xA0, //   Report ID (-96)
    0x09, 0x2E, //   Usage (0x2E)
    0x95, 0x01, //   Report Count (1)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xE0, //   Report ID (-32)
    0x09, 0x2F, //   Usage (0x2F)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF0, //   Report ID (-16)
    0x09, 0x30, //   Usage (0x30)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF1, //   Report ID (-15)
    0x09, 0x31, //   Usage (0x31)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF2, //   Report ID (-14)
    0x09, 0x32, //   Usage (0x32)
    0x95, 0x34, //   Report Count (52)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF4, //   Report ID (-12)
    0x09, 0x35, //   Usage (0x35)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0xF5, //   Report ID (-11)
    0x09, 0x36, //   Usage (0x36)
    0x95, 0x03, //   Report Count (3)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x60, //   Report ID (96)
    0x09, 0x41, //   Usage (0x41)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x61, //   Report ID (97)
    0x09, 0x42, //   Usage (0x42)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x62, //   Report ID (98)
    0x09, 0x43, //   Usage (0x43)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x63, //   Report ID (99)
    0x09, 0x44, //   Usage (0x44)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x64, //   Report ID (100)
    0x09, 0x45, //   Usage (0x45)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x65, //   Report ID (101)
    0x09, 0x46, //   Usage (0x46)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x68, //   Report ID (104)
    0x09, 0x47, //   Usage (0x47)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x70, //   Report ID (112)
    0x09, 0x48, //   Usage (0x48)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x71, //   Report ID (113)
    0x09, 0x49, //   Usage (0x49)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x72, //   Report ID (114)
    0x09, 0x4A, //   Usage (0x4A)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x73, //   Report ID (115)
    0x09, 0x4B, //   Usage (0x4B)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x74, //   Report ID (116)
    0x09, 0x4C, //   Usage (0x4C)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x75, //   Report ID (117)
    0x09, 0x4D, //   Usage (0x4D)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x76, //   Report ID (118)
    0x09, 0x4E, //   Usage (0x4E)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x77, //   Report ID (119)
    0x09, 0x4F, //   Usage (0x4F)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x78, //   Report ID (120)
    0x09, 0x50, //   Usage (0x50)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x79, //   Report ID (121)
    0x09, 0x51, //   Usage (0x51)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x7A, //   Report ID (122)
    0x09, 0x52, //   Usage (0x52)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x85, 0x7B, //   Report ID (123)
    0x09, 0x53, //   Usage (0x53)
    0xB1, 0x02, //   Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    // Report IDs 0xF6-0xF9 (vendor usages 0x37-0x3A) were the old WebHID config
    // command channel; removed (config is served over the NCM web page now).
    0xC0, // End Collection
    // 405 bytes
};
static_assert(sizeof(desc_hid_report_dse) == 0x0185);

#ifdef ENABLE_WAKE_HID
// 41-byte boot-keyboard report descriptor (modifier byte + reserved + 6 keycodes,
// no Report ID -- boot protocol forbids one and avoids collision with the gamepad's Report ID 1).
uint8_t const desc_hid_report_kbd[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x05, 0x07,       //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,       //   Usage Minimum (Left Control)
    0x29, 0xE7,       //   Usage Maximum (Right GUI)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8)
    0x81, 0x02,       //   Input (Data,Var,Abs) -- modifier byte
    0x95, 0x01,       //   Report Count (1)
    0x75, 0x08,       //   Report Size (8)
    0x81, 0x01,       //   Input (Const) -- reserved byte
    0x95, 0x06,       //   Report Count (6)
    0x75, 0x08,       //   Report Size (8)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x65,       //   Logical Maximum (101)
    0x05, 0x07,       //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,       //   Usage Minimum (0)
    0x29, 0x65,       //   Usage Maximum (101)
    0x81, 0x00,       //   Input (Data,Array) -- 6 keycodes
    0xC0              // End Collection
};
_Static_assert(sizeof(desc_hid_report_kbd) == 45, "keyboard report descriptor length must match wDescriptorLength in config descriptor");

// Dummy HID report descriptor for the MINIMAL variant's placeholder interface.
// Its ONLY purpose is to occupy HID instance 0 in MINIMAL exactly as the
// gamepad does in FULL, so the keyboard is HID instance 1 in BOTH variants and
// its instance index never changes across a variant swap. That stability is
// what makes the "rogue keyboard on wake" structurally impossible: a gamepad
// report addressed to instance 0 can never reach the keyboard (instance 1).
//
// Vendor-defined usage page (0xFF00) so no OS binds it to a keyboard/mouse/
// gamepad driver -- it appears as an inert generic HID node with one input
// report we never send. 21 bytes.
uint8_t const desc_hid_report_dummy[] = {
    0x06, 0x00, 0xFF, // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,       // Usage (Vendor Usage 1)
    0xA1, 0x01,       // Collection (Application)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x01,       //   Report Count (1)
    0x09, 0x01,       //   Usage (Vendor Usage 1)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0xC0              // End Collection
};
_Static_assert(sizeof(desc_hid_report_dummy) == 21, "dummy report descriptor length must match wDescriptorLength in minimal config descriptor");
#endif

// Invoked when received GET HID REPORT DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf) {
#ifdef ENABLE_WAKE_HID
    // HID instance layout:
    //   FULL    : 0 = slot 0 gamepad, 1 = keyboard, 2..N = slot 1..N-1 gamepads
    //   MINIMAL : 0 = inert dummy HID, 1 = keyboard
    if (itf == usb_kbd_hid_instance()) return desc_hid_report_kbd;
    // Non-keyboard instance in MINIMAL is the dummy placeholder.
    if (active_variant == DESC_VARIANT_MINIMAL) return desc_hid_report_dummy;
#endif
    (void) itf;
    // Every gamepad instance serves the same report descriptor (one device
    // identity for the whole composite).
    if (ds_mode()) {
        return desc_hid_report_ds;
    }
    return desc_hid_report_dse;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// array of pointer to string descriptors
static char const *string_desc_arr[] =
{
    (const char[]){0x09, 0x04}, // 0: is supported language is English (0x0409)
    "Sony Interactive Entertainment", // 1: Manufacturer
    NULL, // 2: Product
    NULL, // 3: Serials will use unique ID if possible
#ifdef ENABLE_WEBCONFIG
    NULL, // STRID_NET: intentionally no string -- a function/interface name
          //   only makes Windows show "<manufacturer> <name>", and the Sony
          //   manufacturer prefix is unavoidable. Slot kept to align STRID_MAC.
    NULL, // STRID_MAC: generated in the callback (required by NCM)
#endif
};

static uint16_t _desc_str[60 + 1];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;
    size_t chr_count;

    if (ds_mode()) {
        string_desc_arr[2] = "DualSense Wireless Controller";
    }else {
        string_desc_arr[2] = "DualSense Edge Wireless Controller";
    }

    switch (index) {
        case STRID_LANGID:
            memcpy(&_desc_str[1], string_desc_arr[0], 2);
            chr_count = 1;
            break;

        case STRID_SERIAL:
            chr_count = board_usb_get_serial(_desc_str + 1, 32);
            break;

#ifdef ENABLE_WEBCONFIG
        case STRID_MAC: {
            // MAC the host NIC should use, 12 hex chars (CDC-ECM/NCM convention).
            extern uint8_t tud_network_mac_address[6];
            chr_count = 0;
            for (int i = 0; i < 6; i++) {
                _desc_str[1 + chr_count++] = "0123456789ABCDEF"[(tud_network_mac_address[i] >> 4) & 0xf];
                _desc_str[1 + chr_count++] = "0123456789ABCDEF"[tud_network_mac_address[i] & 0xf];
            }
            break;
        }
#endif

        default:
            // Note: the 0xEE index string is a Microsoft OS 1.0 Descriptors.
            // https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors

            if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) return NULL;

            const char *str = string_desc_arr[index];
            if (str == nullptr) return NULL; // unused/placeholder slot (e.g. STRID_NET)

            // Cap at max char
            chr_count = strlen(str);
            size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1; // -1 for string type
            if (chr_count > max_count) chr_count = max_count;

            // Convert ASCII string into UTF-16
            for (size_t i = 0; i < chr_count; i++) {
                _desc_str[1 + i] = str[i];
            }
            break;
    }

    // first byte is length (including header), second byte is string type
    _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return _desc_str;
}

#ifdef ENABLE_WAKE_HID
//--------------------------------------------------------------------+
// Microsoft OS 2.0 descriptors (carried via BOS).
//
// Why this is here: the dongle is a composite device with USB Audio Class
// interfaces. By default Windows audio engine policy keeps USB audio devices
// at D0 even during system S3, blocking selective-suspend for the whole
// composite. Without selective-suspend the device never enters USB suspend,
// so tud_remote_wakeup() never works -- breaking wake-on-PS.
//
// MS OS 2.0 lets us tell Windows "yes, please selective-suspend this audio
// function": we set the registry property "SelectiveSuspendEnabled" = 1 on
// the audio function (interface 0). This causes Windows to write
//   HKLM\SYSTEM\CurrentControlSet\Enum\USB\<VID&PID>\<instance>
//        \Device Parameters\SelectiveSuspendEnabled = 1
// at enumeration time, opting our audio function in to selective suspend
// without breaking haptics.
//
// Reference: "Microsoft OS 2.0 Descriptors Specification".
//--------------------------------------------------------------------+

#define MS_OS_20_VENDOR_CODE 0x01

// Component sizes of the MS OS 2.0 descriptor set (so lengths are computed, not
// hand-counted as the whole set grows):
#define MS_OS_20_SET_HEADER_LEN    10
#define MS_OS_20_CONFIG_SUBSET_LEN 8
#define MS_OS_20_FUNC_SUBSET_HDR_LEN 8
// Registry-property feature: 10 fixed + 48 name + 4 data = 62.
#define MS_OS_20_REG_PROP_LEN      62
// Audio function subset = its header + the SelectiveSuspendEnabled reg property.
#define MS_OS_20_AUDIO_FUNC_LEN    (MS_OS_20_FUNC_SUBSET_HDR_LEN + MS_OS_20_REG_PROP_LEN)
// CompatibleID feature (WINUSB) = 4 fixed + 8 compatible ID + 8 sub-compatible ID.
#define MS_OS_20_COMPATID_LEN      20
// One WinUSB function subset = header + CompatibleID feature.
#define MS_OS_20_WINUSB_FUNC_LEN   (MS_OS_20_FUNC_SUBSET_HDR_LEN + MS_OS_20_COMPATID_LEN)
// What interface 0 IS differs by variant, and the MS OS 2.0 set must describe
// EXACTLY one function subset per function -- never two subsets for the same
// bFirstInterface, which Windows treats as malformed and ignores:
//
//   FULL    : itf 0-2 = USB-Audio function. The set carries ONE function subset
//             for itf 0 = the audio subset (SelectiveSuspendEnabled reg-prop,
//             needed for wake). NO WinUSB tag -- a WinUSB compat-id on itf 0 would
//             make Windows prefer WinUSB over the audio class driver and bang it.
//
//   MINIMAL : itf 0-2 = IAD-grouped inert vendor pad (Class_ff). The set carries
//             ONE function subset for itf 0 = the WinUSB compat-id, so the pad
//             gets a (driverless) WinUSB binding instead of a Code 28 "unknown
//             device" bang. There is NO audio function here, so the audio/
//             SelectiveSuspend subset must NOT appear (it would be a second subset
//             for itf 0, and would target a non-audio interface).
//
// tud_vendor_control_xfer_cb()/tud_descriptor_bos_cb() serve the matching set;
// every variant swap re-enumerates, so the host re-reads BOS + the right set.

// Config-subset length: FULL = audio subset only; MINIMAL = WinUSB subset only.
#define MS_OS_20_CONFIG_SUBSET_TOTAL_LEN_FULL \
    (MS_OS_20_CONFIG_SUBSET_LEN + MS_OS_20_AUDIO_FUNC_LEN)
#define MS_OS_20_CONFIG_SUBSET_TOTAL_LEN_MINIMAL \
    (MS_OS_20_CONFIG_SUBSET_LEN + MS_OS_20_WINUSB_FUNC_LEN)

#define MS_OS_20_DESC_LEN_FULL \
    (MS_OS_20_SET_HEADER_LEN + MS_OS_20_CONFIG_SUBSET_TOTAL_LEN_FULL)
#define MS_OS_20_DESC_LEN_MINIMAL \
    (MS_OS_20_SET_HEADER_LEN + MS_OS_20_CONFIG_SUBSET_TOTAL_LEN_MINIMAL)

// One WinUSB function subset for interface `itf` (used for the inert pads).
#define MS_OS_20_WINUSB_FUNC(itf) \
    U16_TO_U8S_LE(MS_OS_20_FUNC_SUBSET_HDR_LEN), \
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), \
    (itf), 0x00, \
    U16_TO_U8S_LE(MS_OS_20_WINUSB_FUNC_LEN), \
    /* CompatibleID feature: "WINUSB\0\0" + empty sub-compatible id */ \
    U16_TO_U8S_LE(MS_OS_20_COMPATID_LEN), \
    U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), \
    'W','I','N','U','S','B',0,0, \
    0,0,0,0,0,0,0,0

#define BOS_TOTAL_LEN        (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

// The BOS platform-capability descriptor embeds wMSOSDescriptorSetTotalLength,
// which Windows uses as wLength for the follow-up vendor request. It MUST equal
// the Set-Header wTotalLength of the set actually returned, or Windows rejects
// the whole MS OS 2.0 set -- and since this is a USB 2.1 device (BOS required),
// the composite parent fails to start (Code 10). The FULL and MINIMAL sets differ
// in length (MINIMAL carries the extra WinUSB function), so BOS is variant-aware
// too. Both arrays are the same size (only the embedded length value differs), so
// the device sees a consistent BOS size. Every variant swap re-enumerates, so the
// host re-reads BOS and gets the length matching the set it will then fetch.
#define DS5_DESC_BOS(ms_os_20_set_len) { \
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1), \
    TUD_BOS_MS_OS_20_DESCRIPTOR(ms_os_20_set_len, MS_OS_20_VENDOR_CODE) \
}

uint8_t const desc_bos_full[]    = DS5_DESC_BOS(MS_OS_20_DESC_LEN_FULL);
#ifdef ENABLE_WEBCONFIG
uint8_t const desc_bos_minimal[] = DS5_DESC_BOS(MS_OS_20_DESC_LEN_MINIMAL);
#endif

uint8_t const *tud_descriptor_bos_cb(void) {
#ifdef ENABLE_WEBCONFIG
    if (active_variant == DESC_VARIANT_MINIMAL) return desc_bos_minimal;
#endif
    return desc_bos_full;
}

// Audio function subset (identical in both variants): groups interfaces 0-2 as
// one function and sets SelectiveSuspendEnabled=1 on it (needed for wake). This
// does NOT bind a driver -- the audio class driver claims interface 0 by class.
#define MS_OS_20_AUDIO_SUBSET \
    /* --- Function Subset for the Audio function (8 bytes) --- */ \
    /* Audio Control is interface 0; AudioStreaming OUT/IN are 1/2 -- this */ \
    /* subset covers all three because they belong to the same function. */ \
    U16_TO_U8S_LE(0x0008),                  /* wLength */ \
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),  /* wDescriptorType */ \
    0x00,                                   /* bFirstInterface (audio control) */ \
    0x00,                                   /* bReserved */ \
    U16_TO_U8S_LE(MS_OS_20_AUDIO_FUNC_LEN), /* wSubsetLength (this subset + its reg property) */ \
    /* --- Feature: Registry Property "SelectiveSuspendEnabled" = 1 (62 bytes) --- */ \
    U16_TO_U8S_LE(0x003E),                  /* wLength = 62 */ \
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),    /* wDescriptorType */ \
    U16_TO_U8S_LE(0x0004),                  /* wPropertyDataType = REG_DWORD_LITTLE_ENDIAN */ \
    U16_TO_U8S_LE(48),                      /* wPropertyNameLength = 48 bytes (24 UTF-16 chars) */ \
    /* PropertyName "SelectiveSuspendEnabled\0" UTF-16LE (48 bytes) */ \
    'S',0, 'e',0, 'l',0, 'e',0, 'c',0, 't',0, 'i',0, 'v',0, \
    'e',0, 'S',0, 'u',0, 's',0, 'p',0, 'e',0, 'n',0, 'd',0, \
    'E',0, 'n',0, 'a',0, 'b',0, 'l',0, 'e',0, 'd',0,  0,0, \
    U16_TO_U8S_LE(0x0004),                  /* wPropertyDataLength = 4 bytes */ \
    U32_TO_U8S_LE(0x00000001)               /* PropertyData = 1 (enabled) */

// MS OS 2.0 Set Header + Configuration Subset header, parameterised by the set's
// total length and config-subset total length (which differ between variants).
#define MS_OS_20_SET_AND_CONFIG_HEADER(desc_len, cfg_subset_len) \
    /* --- Set Header (10 bytes) --- */ \
    U16_TO_U8S_LE(0x000A),                  /* wLength */ \
    U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),   /* wDescriptorType */ \
    U32_TO_U8S_LE(0x06030000),              /* dwWindowsVersion = Win 8.1+ */ \
    U16_TO_U8S_LE(desc_len),                /* wTotalLength */ \
    /* --- Configuration Subset (8 bytes) --- */ \
    U16_TO_U8S_LE(0x0008),                  /* wLength */ \
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),  /* wDescriptorType */ \
    0x00,                                   /* bConfigurationValue (config index, 0) */ \
    0x00,                                   /* bReserved */ \
    U16_TO_U8S_LE(cfg_subset_len)           /* wTotalLength of this subset */

// FULL variant: interface 0 is audio -> audio subset only, NO WinUSB tag.
uint8_t const desc_ms_os_20_full[] = {
    MS_OS_20_SET_AND_CONFIG_HEADER(MS_OS_20_DESC_LEN_FULL,
                                   MS_OS_20_CONFIG_SUBSET_TOTAL_LEN_FULL),
    MS_OS_20_AUDIO_SUBSET,
};
TU_VERIFY_STATIC(sizeof(desc_ms_os_20_full) == MS_OS_20_DESC_LEN_FULL,
                 "MS OS 2.0 FULL descriptor length mismatch");

#ifdef ENABLE_WEBCONFIG
// MINIMAL variant: interface 0 is the IAD-grouped inert vendor pad -> ONLY a
// WinUSB compatible-ID on interface 0, so the pad gets a (driverless) binding and
// Windows shows no "unknown device" yellow bang for it. No audio subset here:
// there is no audio function in MINIMAL, and a second subset for itf 0 would make
// the whole set malformed (the cause of the lingering MI_00 Code 28 bang).
uint8_t const desc_ms_os_20_minimal[] = {
    MS_OS_20_SET_AND_CONFIG_HEADER(MS_OS_20_DESC_LEN_MINIMAL,
                                   MS_OS_20_CONFIG_SUBSET_TOTAL_LEN_MINIMAL),
    MS_OS_20_WINUSB_FUNC(0),
};
TU_VERIFY_STATIC(sizeof(desc_ms_os_20_minimal) == MS_OS_20_DESC_LEN_MINIMAL,
                 "MS OS 2.0 MINIMAL descriptor length mismatch");
#endif

// Vendor-class control transfer hook. Windows reads BOS, sees the MS OS 2.0
// platform capability, then issues this vendor request to fetch the
// descriptor set itself.
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) return false;
    if (request->bRequest == MS_OS_20_VENDOR_CODE && request->wIndex == 7) {
        // wIndex == 7 -> MS_OS_20_DESCRIPTOR_INDEX
        // Serve the set matching the variant the host is currently enumerating.
        // Only MINIMAL carries the WinUSB tag on interface 0 (the inert pad);
        // FULL must NOT, or Windows binds WinUSB to the audio control interface
        // and bangs it (Code 28). The WinUSB-bearing MINIMAL set only exists with
        // ENABLE_WEBCONFIG (the inert pad is a webconfig-only construct).
        const uint8_t *set = desc_ms_os_20_full;
        size_t set_len = sizeof(desc_ms_os_20_full);
#ifdef ENABLE_WEBCONFIG
        if (active_variant == DESC_VARIANT_MINIMAL) {
            set = desc_ms_os_20_minimal;
            set_len = sizeof(desc_ms_os_20_minimal);
        }
#endif
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)set, set_len);
    }
    return false;
}
#endif // ENABLE_WAKE_HID
