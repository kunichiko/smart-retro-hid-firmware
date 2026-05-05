// ===================================================================================
// Project:  smart-retro-hid-firmware
// Author:   Kunihiko Ohnaka (@kunichiko)
// Year:     2026
// URL:      https://github.com/kunichiko/smart-retro-hid-firmware
// ===================================================================================
//
// USB-MIDI 経由でスマートフォンから制御コマンドを受信し、
// レトロ PC の HID デバイス（キーボード・ジョイスティック等）を模倣する。
//
// プロトコル仕様: https://github.com/kunichiko/smart-retro-hid-protocol
//

#include <stdio.h>
#include <string.h>

#include "ch32fun.h"
#include "funconfig.h"
#include "usb_midi.h"
#include "joystick.h"

// ---------------------------------------------------------------------------
// プロトコル定数 (smart-retro-hid-protocol v0.1.0)
// ---------------------------------------------------------------------------

#define PROTOCOL_VERSION_MAJOR  0
#define PROTOCOL_VERSION_MINOR  3
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  3
#define FW_VERSION_PATCH  0

// MIDI チャンネル
#define MIDI_CH_JOYSTICK  0
#define MIDI_CH_KEYBOARD  1
#define MIDI_CH_LED       14
#define MIDI_CH_STATUS    15

// SysEx コマンド
#define SYSEX_MFR_ID      0x7D
#define SYSEX_SUB_ID      0x01
#define CMD_IDENTIFY_REQ  0x01
#define CMD_IDENTIFY_RSP  0x02
#define CMD_CAPABILITY_REQ 0x03
#define CMD_CAPABILITY_RSP 0x04
#define CMD_SET_CONFIG    0x10
#define CMD_RESET         0x7F

// チャンネル割り当て (デバイスごとに異なる、ビルド時固定)
// このボードは ATARI ジョイスティック専用 (Ch0)
#define HID_TYPE_KEYBOARD  0x01
#define HID_TYPE_JOYSTICK  0x02
#define HID_TYPE_MOUSE     0x03

#define TARGET_GENERIC  0x00
#define TARGET_ATARI    0x01
#define TARGET_X68000   0x02
#define TARGET_MD       0x40

static const uint8_t channel_map[] = {
    // ch, type, target
    0, HID_TYPE_JOYSTICK, TARGET_ATARI,
};
#define NUM_CHANNELS  (sizeof(channel_map) / 3)

static const char DEVICE_NAME[] = "mimic-x-joy";

// SET_CONFIG キー
#define CONFIG_PAD_MODE   0x03

// Capability Types
#define CAP_BUTTON_COUNT  0x01
#define CAP_BIDI          0x20

// デバッグ LED (PB3, PB4)
#define DEBUG_LED0_PIN  3
#define DEBUG_LED1_PIN  4

static void gpio_init_debug_leds(void) {
    GPIOB->CFGLR &= ~(0xf << (4 * DEBUG_LED0_PIN));
    GPIOB->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP) << (4 * DEBUG_LED0_PIN);
    GPIOB->BSHR = (1 << DEBUG_LED0_PIN);
    GPIOB->CFGLR &= ~(0xf << (4 * DEBUG_LED1_PIN));
    GPIOB->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP) << (4 * DEBUG_LED1_PIN);
    GPIOB->BSHR = (1 << DEBUG_LED1_PIN);
}

static void debug_led_set(uint8_t led, uint8_t on) {
    uint8_t pin = (led == 0) ? DEBUG_LED0_PIN : DEBUG_LED1_PIN;
    if (led > 1) return;
    if (on) GPIOB->BCR = (1 << pin);
    else    GPIOB->BSHR = (1 << pin);
}

// ---------------------------------------------------------------------------
// X68000 キーボード制御
// ---------------------------------------------------------------------------

static void uart_init_x68k_keyboard(void) {
    RCC->APB2PCENR |= RCC_USART1EN;
    GPIOC->CFGLR &= ~(0xf << (4 * 1));
    GPIOC->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP_AF) << (4 * 1);
    GPIOC->CFGLR &= ~(0xf << (4 * 0));
    GPIOC->CFGLR |= (GPIO_Speed_In | GPIO_CNF_IN_FLOATING) << (4 * 0);
    USART1->BRR = F_CPU / 2400;
    USART1->CTLR1 = USART_CTLR1_TE | USART_CTLR1_RE;
    USART1->CTLR1 |= USART_CTLR1_UE;
}

static void x68k_keyboard_send(uint8_t keycode) {
    while (!(USART1->STATR & USART_STATR_TXE));
    USART1->DATAR = keycode;
}

static int x68k_keyboard_receive(void) {
    if (USART1->STATR & USART_STATR_RXNE) return (uint8_t)USART1->DATAR;
    return -1;
}

// ---------------------------------------------------------------------------
// SysEx 処理
// ---------------------------------------------------------------------------

static uint8_t sysex_buf[64];
static uint8_t sysex_len;
static uint8_t sysex_receiving;

static void sysex_reset(void) { sysex_len = 0; sysex_receiving = 0; }

static void send_identify_response(void) {
    uint8_t rsp[64];
    int i = 0;
    rsp[i++] = 0xF0;
    rsp[i++] = SYSEX_MFR_ID;
    rsp[i++] = SYSEX_SUB_ID;
    rsp[i++] = CMD_IDENTIFY_RSP;
    rsp[i++] = PROTOCOL_VERSION_MAJOR;
    rsp[i++] = PROTOCOL_VERSION_MINOR;
    rsp[i++] = FW_VERSION_MAJOR;
    rsp[i++] = FW_VERSION_MINOR;
    rsp[i++] = FW_VERSION_PATCH;
    rsp[i++] = NUM_CHANNELS;
    for (int j = 0; j < (int)sizeof(channel_map) && i < (int)sizeof(rsp) - 2; j++)
        rsp[i++] = channel_map[j] & 0x7F;
    for (int j = 0; DEVICE_NAME[j] && i < (int)sizeof(rsp) - 1; j++)
        rsp[i++] = DEVICE_NAME[j] & 0x7F;
    rsp[i++] = 0xF7;
    usb_midi_send_sysex(rsp, i);
}

static void send_capability_response(void) {
    uint8_t rsp[32];
    int i = 0;
    rsp[i++] = 0xF0;
    rsp[i++] = SYSEX_MFR_ID;
    rsp[i++] = SYSEX_SUB_ID;
    rsp[i++] = CMD_CAPABILITY_RSP;
    // ボタン数
    rsp[i++] = CAP_BUTTON_COUNT;
    rsp[i++] = 1;
    rsp[i++] = 12;  // Up,Down,Left,Right,A,B,C,Start,X,Y,Z,Mode
    // 双方向通信対応
    rsp[i++] = CAP_BIDI;
    rsp[i++] = 1;
    rsp[i++] = 1;
    rsp[i++] = 0xF7;
    usb_midi_send_sysex(rsp, i);
}

static void process_sysex(const uint8_t* data, int len) {
    if (len < 5) return;
    if (data[0] != 0xF0 || data[len - 1] != 0xF7) return;
    if (data[1] != SYSEX_MFR_ID || data[2] != SYSEX_SUB_ID) return;

    uint8_t cmd = data[3];
    switch (cmd) {
    case CMD_IDENTIFY_REQ:
        send_identify_response();
        break;
    case CMD_CAPABILITY_REQ:
        send_capability_response();
        break;
    case CMD_SET_CONFIG:
        // F0 7D 01 10 <key> <value> F7 = 7 bytes minimum
        if (len >= 7 && data[4] == CONFIG_PAD_MODE) {
            joystick_set_mode(data[5]);
            // モード変更を LED で表示
            debug_led_set(0, data[5] == PAD_MODE_MD6);
        }
        break;
    case CMD_RESET:
        joystick_release_all();
        break;
    }
}

// ---------------------------------------------------------------------------
// USB-MIDI メッセージ処理
// ---------------------------------------------------------------------------

static void process_midi_event(uint8_t cin, uint8_t midi0, uint8_t midi1, uint8_t midi2) {
    uint8_t channel = midi0 & 0x0F;

    // SysEx の組み立て
    switch (cin) {
    case CIN_SYSEX_START:
        if (!sysex_receiving) { sysex_reset(); sysex_receiving = 1; }
        if (sysex_len + 3 <= sizeof(sysex_buf)) {
            sysex_buf[sysex_len++] = midi0;
            sysex_buf[sysex_len++] = midi1;
            sysex_buf[sysex_len++] = midi2;
        }
        return;
    case CIN_SYSEX_END_1:
        if (sysex_receiving && sysex_len + 1 <= sizeof(sysex_buf)) {
            sysex_buf[sysex_len++] = midi0;
            process_sysex(sysex_buf, sysex_len);
        }
        sysex_reset();
        return;
    case CIN_SYSEX_END_2:
        if (sysex_receiving && sysex_len + 2 <= sizeof(sysex_buf)) {
            sysex_buf[sysex_len++] = midi0;
            sysex_buf[sysex_len++] = midi1;
            process_sysex(sysex_buf, sysex_len);
        }
        sysex_reset();
        return;
    case CIN_SYSEX_END_3:
        if (sysex_receiving && sysex_len + 3 <= sizeof(sysex_buf)) {
            sysex_buf[sysex_len++] = midi0;
            sysex_buf[sysex_len++] = midi1;
            sysex_buf[sysex_len++] = midi2;
            process_sysex(sysex_buf, sysex_len);
        }
        sysex_reset();
        return;
    }

    // 通常の MIDI メッセージ処理
    switch (cin) {
    case CIN_NOTE_ON:
        if (channel == MIDI_CH_JOYSTICK) {
            joystick_set_button_by_note(midi1, 1);
        } else if (channel == MIDI_CH_KEYBOARD) {
            x68k_keyboard_send(midi1 & 0x7F);
        } else if (channel == MIDI_CH_STATUS) {
            debug_led_set(midi1, 1);
        }
        break;

    case CIN_NOTE_OFF:
        if (channel == MIDI_CH_JOYSTICK) {
            joystick_set_button_by_note(midi1, 0);
        } else if (channel == MIDI_CH_KEYBOARD) {
            x68k_keyboard_send(0x80 | (midi1 & 0x7F));
        } else if (channel == MIDI_CH_STATUS) {
            debug_led_set(midi1, 0);
        }
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// メインループ
// ---------------------------------------------------------------------------

int main() {
    SystemInit();
    RCC->CTLR |= RCC_HSION;

    // ポートクロック有効化
    RCC->APB2PCENR |= RCC_IOPAEN | RCC_IOPBEN | RCC_IOPCEN | RCC_AFIOEN;

    // 初期化
    gpio_init_debug_leds();
    joystick_init();
    uart_init_x68k_keyboard();
    usb_midi_init();

    uint8_t cin, midi0, midi1, midi2;

    // クロック確認用: PB3 を Delay_Ms(500) ベースで点滅。1Hz になるはず
    uint32_t blink_counter = 0;
    uint32_t last_systick = 0;

    while (1) {
        // SysTick で 500ms 経過を検出 (HCLK = 48MHz なので 24,000,000 カウントで 500ms)
        uint32_t now = SysTick->CNT;
        if ((uint32_t)(now - last_systick) >= 24000000) {
            last_systick = now;
            blink_counter ^= 1;
            debug_led_set(0, blink_counter);
        }
        // USB-MIDI からコマンド受信・処理
        while (usb_midi_receive_event(&cin, &midi0, &midi1, &midi2) > 0) {
            process_midi_event(cin, midi0, midi1, midi2);
        }

        // ジョイスティックポーリング
        joystick_poll();

        // X68000 本体からの LED コマンド受信 → ホストに通知
        int led_cmd = x68k_keyboard_receive();
        if (led_cmd >= 0) {
            for (int i = 0; i < 7; i++) {
                uint8_t on = (led_cmd >> i) & 1;
                usb_midi_control_change(MIDI_CH_LED, i, on ? 127 : 0);
            }
        }

        // USB-MIDI TX バッファをフラッシュ
        usb_midi_poll();
    }
}
