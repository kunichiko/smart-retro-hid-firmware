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

// ジョイスティック GPIO ピンマッピング (PA0-PA7)
// ATARI D-SUB 9pin:
//   Pin1=UP, Pin2=DOWN, Pin3=LEFT, Pin4=RIGHT, Pin6=TRIG-A, Pin7=TRIG-B, Pin8=COMMON
#define JOY_PIN_UP      1  // PA1: UP     (D-SUB pin 1)
#define JOY_PIN_DOWN    2  // PA2: DOWN   (D-SUB pin 2)
#define JOY_PIN_LEFT    3  // PA3: LEFT   (D-SUB pin 3)
#define JOY_PIN_RIGHT   4  // PA4: RIGHT  (D-SUB pin 4)
#define JOY_PIN_TRIG_A  6  // PA6: TRIG-A (D-SUB pin 6)
#define JOY_PIN_TRIG_B  7  // PA7: TRIG-B (D-SUB pin 7)
#define JOY_PIN_COMMON  0  // PA0: COMMON (D-SUB pin 8, 入力: 通常GND、本体側でHighにできる)

// MIDI Note 番号 → GPIO ピンの変換テーブル
// Note 1=UP, 2=DOWN, 3=LEFT, 4=RIGH (存在しない), 6=TRIG-A, 7=TRIG-B
// Note 8 = COMMON 入力状態 (デバイス→ホスト通知用、出力には使わない)
#define JOY_NOTE_UP      1
#define JOY_NOTE_DOWN    2
#define JOY_NOTE_LEFT    3
#define JOY_NOTE_RIGHT   4
#define JOY_NOTE_TRIG_A  6
#define JOY_NOTE_TRIG_B  7
#define JOY_NOTE_COMMON  8  // COMMON 入力状態の通知用

// Note 番号 → GPIO ピン変換 (0xFF = 無効)
static uint8_t joy_note_to_pin(uint8_t note) {
    switch (note) {
    case JOY_NOTE_UP:     return JOY_PIN_UP;
    case JOY_NOTE_DOWN:   return JOY_PIN_DOWN;
    case JOY_NOTE_LEFT:   return JOY_PIN_LEFT;
    case JOY_NOTE_RIGHT:  return JOY_PIN_RIGHT;
    case JOY_NOTE_TRIG_A: return JOY_PIN_TRIG_A;
    case JOY_NOTE_TRIG_B: return JOY_PIN_TRIG_B;
    default:              return 0xFF;
    }
}

// Capability Types
#define CAP_LED_COUNT     0x03
#define CAP_LED_NAME      0x04
#define CAP_KEYCODE_RANGE 0x10
#define CAP_BIDI          0x20

// ---------------------------------------------------------------------------
// デバッグ LED (PB3, PB4: Low アクティブ)
// ---------------------------------------------------------------------------
// Note On/Off (Channel 15) で制御
//   note 0 → PB3, note 1 → PB4

#define DEBUG_LED0_PIN  3  // PB3
#define DEBUG_LED1_PIN  4  // PB4

static void gpio_init_debug_leds(void) {
    // PB3: Push-Pull 出力
    GPIOB->CFGLR &= ~(0xf << (4 * DEBUG_LED0_PIN));
    GPIOB->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP) << (4 * DEBUG_LED0_PIN);
    GPIOB->BSHR = (1 << DEBUG_LED0_PIN);  // High = 消灯

    // PB4: Push-Pull 出力
    GPIOB->CFGLR &= ~(0xf << (4 * DEBUG_LED1_PIN));
    GPIOB->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP) << (4 * DEBUG_LED1_PIN);
    GPIOB->BSHR = (1 << DEBUG_LED1_PIN);  // High = 消灯
}

static void debug_led_set(uint8_t led, uint8_t on) {
    uint8_t pin = (led == 0) ? DEBUG_LED0_PIN : DEBUG_LED1_PIN;
    if (led > 1) return;
    if (on) {
        GPIOB->BCR = (1 << pin);   // Low = 点灯
    } else {
        GPIOB->BSHR = (1 << pin);  // High = 消灯
    }
}

// ---------------------------------------------------------------------------
// GPIO 初期化
// ---------------------------------------------------------------------------

// ATARI ジョイスティック用 GPIO
// PA1-PA4,PA6,PA7: オープンドレイン出力 (方向+ボタン)
// PA0: 入力 (COMMON, 通常GND、本体側でHighにできる)
static void gpio_init_joystick(void) {
    // 出力ピン (オープンドレイン)
    const uint8_t out_pins[] = {
        JOY_PIN_UP, JOY_PIN_DOWN, JOY_PIN_LEFT,
        JOY_PIN_RIGHT, JOY_PIN_TRIG_A, JOY_PIN_TRIG_B
    };
    for (int i = 0; i < (int)(sizeof(out_pins) / sizeof(out_pins[0])); i++) {
        uint8_t p = out_pins[i];
        GPIOA->CFGLR &= ~(0xf << (4 * p));
        GPIOA->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_OD) << (4 * p);
        GPIOA->BSHR = (1 << p);  // High = Hi-Z
    }

    // COMMON (PA0): 入力、プルダウン (通常GND)
    GPIOA->CFGLR &= ~(0xf << (4 * JOY_PIN_COMMON));
    GPIOA->CFGLR |= (GPIO_Speed_In | GPIO_CNF_IN_PUPD) << (4 * JOY_PIN_COMMON);
    GPIOA->BCR = (1 << JOY_PIN_COMMON);  // Pull-Down
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

static void joystick_set_button(uint8_t note, uint8_t pressed) {
    uint8_t pin = joy_note_to_pin(note);
    if (pin == 0xFF) return;
    if (pressed) {
        GPIOA->BCR = (1 << pin);   // Low = アクティブ
    } else {
        GPIOA->BSHR = (1 << pin);  // High = Hi-Z
    }
}

static void joystick_release_all(void) {
    const uint8_t notes[] = {
        JOY_NOTE_UP, JOY_NOTE_DOWN, JOY_NOTE_LEFT,
        JOY_NOTE_RIGHT, JOY_NOTE_TRIG_A, JOY_NOTE_TRIG_B
    };
    for (int i = 0; i < (int)(sizeof(notes) / sizeof(notes[0])); i++) {
        uint8_t pin = joy_note_to_pin(notes[i]);
        GPIOA->BSHR = (1 << pin);
    }
}

// COMMON 入力の前回状態 (0=Low/GND, 1=High/+5V)
static uint8_t joy_common_prev;

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
        } else if (channel == MIDI_CH_STATUS) {
            debug_led_set(midi1, 1);  // note 0→PB3, note 1→PB4
        }
        break;

    case CIN_NOTE_OFF:
        if (channel == MIDI_CH_KEYBOARD) {
            x68k_keyboard_send(0x80 | (midi1 & 0x7F));  // bit7=1 → break
        } else if (channel == MIDI_CH_JOYSTICK) {
            joystick_set_button(midi1, 0);
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

    RCC->CTLR |= RCC_HSION;  // HSI (48MHz) ON

    // ポートクロック有効化
    RCC->APB2PCENR |= RCC_IOPAEN | RCC_IOPBEN | RCC_IOPCEN | RCC_AFIOEN;

    // デバッグ LED 初期化 (PB3, PB4: Low アクティブ)
    gpio_init_debug_leds();

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

        // COMMON (PA0) 入力の変化を検出 → Note 8 で通知
        {
            uint8_t common_now = (GPIOA->INDR >> JOY_PIN_COMMON) & 1;
            if (common_now != joy_common_prev) {
                joy_common_prev = common_now;
                if (common_now) {
                    usb_midi_note_on(MIDI_CH_JOYSTICK, JOY_NOTE_COMMON, 0x7F);
                } else {
                    usb_midi_note_off(MIDI_CH_JOYSTICK, JOY_NOTE_COMMON, 0x00);
                }
            }
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
