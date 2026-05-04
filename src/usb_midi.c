// ===================================================================================
// USB-MIDI implementation using ch32v003fun fsusb driver
// ===================================================================================
// EP1 OUT (0x01): Host → Device (receive MIDI from smartphone)
// EP2 IN  (0x82): Device → Host (send MIDI to smartphone)
// ===================================================================================

#include "usb_midi.h"

#include <string.h>

#include "ch32fun.h"
#include "fsusb.h"

// ---------------------------------------------------------------------------
// Ring Buffers
// ---------------------------------------------------------------------------

// RX ring buffer: stores 4-byte USB-MIDI event packets received from host
static volatile uint8_t rx_buf[MIDI_RX_BUF_SIZE * 4];
static volatile uint8_t rx_head;  // ISR writes here
static volatile uint8_t rx_tail;  // main loop reads here

// TX ring buffer: stores 4-byte USB-MIDI event packets to send to host
static uint8_t tx_buf[MIDI_TX_BUF_SIZE * 4];
static uint8_t tx_head;  // main loop writes here
static uint8_t tx_tail;  // poll flushes from here

// デバッグ: USBFS_SendEndpoint の最後の戻り値
volatile int debug_send_result;

// ---------------------------------------------------------------------------
// fsusb callbacks (called from USB ISR)
// ---------------------------------------------------------------------------

// Called when IN transfer completes on an endpoint
int HandleInRequest(struct _USBState* ctx, int endp, uint8_t* data, int len) {
    (void)ctx;
    (void)data;
    (void)len;
    return 0;
}

// Called when OUT data is received on an endpoint
void HandleDataOut(struct _USBState* ctx, int endp, uint8_t* data, int len) {
    (void)ctx;
    if (endp == 0) {
        ctx->USBFS_SetupReqLen = 0;
        return;
    }
    if (endp == 1) {
        for (int i = 0; i + 3 < len; i += 4) {
            uint8_t next_head = (rx_head + 1) & (MIDI_RX_BUF_SIZE - 1);
            if (next_head != rx_tail) {
                int offset = rx_head * 4;
                rx_buf[offset + 0] = data[i + 0];
                rx_buf[offset + 1] = data[i + 1];
                rx_buf[offset + 2] = data[i + 2];
                rx_buf[offset + 3] = data[i + 3];
                rx_head = next_head;
            }
        }
    }
}

// Called for non-standard SETUP requests
int HandleSetupCustom(struct _USBState* ctx, int setup_code) {
    (void)ctx;
    (void)setup_code;
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void usb_midi_init(void) {
    rx_head = 0;
    rx_tail = 0;
    tx_head = 0;
    tx_tail = 0;
    debug_send_result = 99;  // 未送信
    USBFSSetup();
}

int usb_midi_send_event(uint8_t cin, uint8_t midi0, uint8_t midi1, uint8_t midi2) {
    uint8_t next_head = (tx_head + 1) & (MIDI_TX_BUF_SIZE - 1);
    if (next_head == tx_tail) {
        return -1;
    }
    int offset = tx_head * 4;
    tx_buf[offset + 0] = (MIDI_CABLE_0 << 4) | (cin & 0x0F);
    tx_buf[offset + 1] = midi0;
    tx_buf[offset + 2] = midi1;
    tx_buf[offset + 3] = midi2;
    tx_head = next_head;
    return 0;
}

int usb_midi_send_sysex(const uint8_t* data, int len) {
    if (len < 2) return -1;

    int i = 0;
    while (i < len) {
        int remaining = len - i;
        uint8_t cin;
        uint8_t b0, b1, b2;

        if (remaining >= 3) {
            if (remaining == 3 && data[i + 2] == 0xF7) {
                cin = CIN_SYSEX_END_3;
            } else {
                cin = CIN_SYSEX_START;
            }
            b0 = data[i];
            b1 = data[i + 1];
            b2 = data[i + 2];
            i += 3;
        } else if (remaining == 2) {
            cin = CIN_SYSEX_END_2;
            b0 = data[i];
            b1 = data[i + 1];
            b2 = 0;
            i += 2;
        } else {
            cin = CIN_SYSEX_END_1;
            b0 = data[i];
            b1 = 0;
            b2 = 0;
            i += 1;
        }

        if (usb_midi_send_event(cin, b0, b1, b2) < 0) {
            return -1;
        }
    }
    return 0;
}

int usb_midi_receive_event(uint8_t* cin, uint8_t* midi0, uint8_t* midi1, uint8_t* midi2) {
    if (rx_head == rx_tail) {
        return 0;
    }
    int offset = rx_tail * 4;
    *cin = rx_buf[offset + 0] & 0x0F;
    *midi0 = rx_buf[offset + 1];
    *midi1 = rx_buf[offset + 2];
    *midi2 = rx_buf[offset + 3];
    rx_tail = (rx_tail + 1) & (MIDI_RX_BUF_SIZE - 1);
    return 4;
}

void usb_midi_poll(void) {
    // Flush TX buffer to EP2 IN
    if (tx_head == tx_tail) return;

    uint8_t* ep_buf = USBFS_GetEPBufferIfAvailable(2);
    if (!ep_buf) return;

    int count = 0;
    while (tx_head != tx_tail && count + 4 <= 64) {
        int offset = tx_tail * 4;
        ep_buf[count + 0] = tx_buf[offset + 0];
        ep_buf[count + 1] = tx_buf[offset + 1];
        ep_buf[count + 2] = tx_buf[offset + 2];
        ep_buf[count + 3] = tx_buf[offset + 3];
        tx_tail = (tx_tail + 1) & (MIDI_TX_BUF_SIZE - 1);
        count += 4;
    }

    if (count > 0) {
        int ret = USBFS_SendEndpoint(2, count);
        debug_send_result = ret;
    }
}
