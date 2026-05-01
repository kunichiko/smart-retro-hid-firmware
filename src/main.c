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

// ---------------------------------------------------------------------------
// プロトコル定数 (smart-retro-hid-protocol v0.1.0)
// ---------------------------------------------------------------------------

// プロトコルバージョン
#define PROTOCOL_VERSION_MAJOR  0
#define PROTOCOL_VERSION_MINOR  1

// ファームウェアバージョン
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  1
#define FW_VERSION_PATCH  0

// MIDI チャンネル
#define MIDI_CH_JOYSTICK  0
#define MIDI_CH_KEYBOARD  1
#define MIDI_CH_LED       14
#define MIDI_CH_STATUS    15

// SysEx コマンド
#define SYSEX_MFR_ID      0x7D  // Non-Commercial
#define SYSEX_SUB_ID      0x01  // smart-retro-hid
#define CMD_IDENTIFY_REQ  0x01
#define CMD_IDENTIFY_RSP  0x02
#define CMD_CAPABILITY_REQ 0x03
#define CMD_CAPABILITY_RSP 0x04
#define CMD_RESET         0x7F

// デバイスタイプ・ターゲットシステム (この基板が何か)
// TODO: 基板バリエーションごとにビルド時に切り替える
#define DEVICE_TYPE       0x01  // Keyboard
#define TARGET_SYSTEM     0x02  // X68000

// デバイス名
static const char DEVICE_NAME[] = "smart-retro-hid-x68k-kb";

// CC 番号 (状態通知)
#define CC_DEVICE_CONNECTED  0x00

// ジョイスティック ボタン番号 (Note 番号にマッピング)
#define JOY_UP      0
#define JOY_DOWN    1
#define JOY_LEFT    2
#define JOY_RIGHT   3
#define JOY_BTN1    4
#define JOY_BTN2    5

// Capability Types
#define CAP_LED_COUNT     0x03
#define CAP_LED_NAME      0x04
#define CAP_KEYCODE_RANGE 0x10
#define CAP_BIDI          0x20

// ---------------------------------------------------------------------------
// GPIO 初期化
// ---------------------------------------------------------------------------

// ATARI ジョイスティック用 GPIO (出力)
// PA0: 上, PA1: 下, PA2: 左, PA3: 右, PA4: ボタン1, PA5: ボタン2
static void gpio_init_joystick(void) {
    for (int i = 0; i < 6; i++) {
        GPIOA->CFGLR &= ~(0xf << (4 * i));
        GPIOA->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP) << (4 * i);
        GPIOA->BSHR = (1 << i);  // High = 非アクティブ (active-low)
    }
}

// X68000 キーボード用 UART 初期化
// PC0: USART1 RX (本体からの LED コマンド受信)
// PC1: USART1 TX (キーコード送信)
static void uart_init_x68k_keyboard(void) {
    RCC->APB2PCENR |= RCC_USART1EN;

    // PC1 (TX): Push-Pull AF 出力
    GPIOC->CFGLR &= ~(0xf << (4 * 1));
    GPIOC->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP_AF) << (4 * 1);

    // PC0 (RX): フローティング入力
    GPIOC->CFGLR &= ~(0xf << (4 * 0));
    GPIOC->CFGLR |= (GPIO_Speed_In | GPIO_CNF_IN_FLOATING) << (4 * 0);

    // USART1: 2400bps, 8N1
    USART1->BRR = F_CPU / 2400;
    USART1->CTLR1 = USART_CTLR1_TE | USART_CTLR1_RE;
    USART1->CTLR1 |= USART_CTLR1_UE;
}

// ---------------------------------------------------------------------------
// ATARI ジョイスティック制御
// ---------------------------------------------------------------------------

static void joystick_set_button(uint8_t button, uint8_t pressed) {
    if (button > 5) return;
    if (pressed) {
        GPIOA->BCR = (1 << button);   // Low = アクティブ
    } else {
        GPIOA->BSHR = (1 << button);  // High = 非アクティブ
    }
}

static void joystick_release_all(void) {
    for (int i = 0; i < 6; i++) {
        GPIOA->BSHR = (1 << i);
    }
}

// ---------------------------------------------------------------------------
// X68000 キーボード制御
// ---------------------------------------------------------------------------

static void x68k_keyboard_send(uint8_t keycode) {
    while (!(USART1->STATR & USART_STATR_TXE))
        ;
    USART1->DATAR = keycode;
}

static int x68k_keyboard_receive(void) {
    if (USART1->STATR & USART_STATR_RXNE) {
        return (uint8_t)USART1->DATAR;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// SysEx 処理
// ---------------------------------------------------------------------------

// SysEx 受信バッファ
static uint8_t sysex_buf[64];
static uint8_t sysex_len;
static uint8_t sysex_receiving;

static void sysex_reset(void) {
    sysex_len = 0;
    sysex_receiving = 0;
}

static void send_identify_response(void) {
    uint8_t rsp[32];
    int i = 0;
    rsp[i++] = 0xF0;
    rsp[i++] = SYSEX_MFR_ID;
    rsp[i++] = SYSEX_SUB_ID;
    rsp[i++] = CMD_IDENTIFY_RSP;
    rsp[i++] = PROTOCOL_VERSION_MAJOR;
    rsp[i++] = PROTOCOL_VERSION_MINOR;
    rsp[i++] = DEVICE_TYPE;
    rsp[i++] = TARGET_SYSTEM;
    rsp[i++] = FW_VERSION_MAJOR;
    rsp[i++] = FW_VERSION_MINOR;
    rsp[i++] = FW_VERSION_PATCH;
    // デバイス名 (ASCII)
    for (int j = 0; DEVICE_NAME[j] && i < 30; j++) {
        rsp[i++] = DEVICE_NAME[j] & 0x7F;
    }
    rsp[i++] = 0xF7;
    usb_midi_send_sysex(rsp, i);
}

static void send_capability_response(void) {
    uint8_t rsp[64];
    int i = 0;
    rsp[i++] = 0xF0;
    rsp[i++] = SYSEX_MFR_ID;
    rsp[i++] = SYSEX_SUB_ID;
    rsp[i++] = CMD_CAPABILITY_RSP;

    // LED 数: 7 (X68000)
    rsp[i++] = CAP_LED_COUNT;
    rsp[i++] = 1;  // length
    rsp[i++] = 7;

    // キーコード範囲: 0x00 - 0x72
    rsp[i++] = CAP_KEYCODE_RANGE;
    rsp[i++] = 2;  // length
    rsp[i++] = 0x00;
    rsp[i++] = 0x72;

    // 双方向通信対応
    rsp[i++] = CAP_BIDI;
    rsp[i++] = 1;  // length
    rsp[i++] = 1;  // 双方向対応

    rsp[i++] = 0xF7;
    usb_midi_send_sysex(rsp, i);
}

static void process_sysex(const uint8_t* data, int len) {
    // 最小: F0 7D 01 <cmd> F7 = 5 bytes
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
        if (!sysex_receiving) {
            sysex_reset();
            sysex_receiving = 1;
        }
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
        if (channel == MIDI_CH_KEYBOARD) {
            x68k_keyboard_send(midi1 & 0x7F);  // bit7=0 → make
        } else if (channel == MIDI_CH_JOYSTICK) {
            joystick_set_button(midi1, 1);
        }
        break;

    case CIN_NOTE_OFF:
        if (channel == MIDI_CH_KEYBOARD) {
            x68k_keyboard_send(0x80 | (midi1 & 0x7F));  // bit7=1 → break
        } else if (channel == MIDI_CH_JOYSTICK) {
            joystick_set_button(midi1, 0);
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

    RCC->CTLR |= RCC_HSION;  // HSI (48MHz) ON

    // ポートクロック有効化
    RCC->APB2PCENR |= RCC_IOPAEN | RCC_IOPCEN | RCC_AFIOEN;

    // 周辺初期化
    gpio_init_joystick();
    uart_init_x68k_keyboard();
    usb_midi_init();

    uint8_t cin, midi0, midi1, midi2;

    while (1) {
        // USB-MIDI からコマンド受信・処理
        while (usb_midi_receive_event(&cin, &midi0, &midi1, &midi2) > 0) {
            process_midi_event(cin, midi0, midi1, midi2);
        }

        // X68000 本体からの LED コマンド受信 → ホストに通知
        int led_cmd = x68k_keyboard_receive();
        if (led_cmd >= 0) {
            // LED 状態の各ビットを個別の CC で通知
            // X68000 の LED コマンドはビットフィールド (仮: 各ビットが各LEDに対応)
            for (int i = 0; i < 7; i++) {
                uint8_t on = (led_cmd >> i) & 1;
                usb_midi_control_change(MIDI_CH_LED, i, on ? 127 : 0);
            }
        }

        // USB-MIDI TX バッファをフラッシュ
        usb_midi_poll();
    }
}
