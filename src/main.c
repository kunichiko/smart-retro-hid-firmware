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

#include <stdio.h>
#include <string.h>

#include "ch32fun.h"
#include "funconfig.h"
#include "usb_midi.h"

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

// directions: bit0=上, bit1=下, bit2=左, bit3=右
// buttons:    bit0=ボタン1, bit1=ボタン2
static void joystick_update(uint8_t directions, uint8_t buttons) {
    for (int i = 0; i < 4; i++) {
        if (directions & (1 << i)) {
            GPIOA->BCR = (1 << i);   // Low = アクティブ
        } else {
            GPIOA->BSHR = (1 << i);  // High = 非アクティブ
        }
    }
    for (int i = 0; i < 2; i++) {
        if (buttons & (1 << i)) {
            GPIOA->BCR = (1 << (i + 4));
        } else {
            GPIOA->BSHR = (1 << (i + 4));
        }
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
// USB-MIDI メッセージ処理
// ---------------------------------------------------------------------------

// smart-retro-hid プロトコル (暫定):
// Channel 0: ジョイスティック制御
//   Note On/Off: note=方向/ボタン番号, velocity=状態
//   CC 0x10: 方向ビットマップ一括 (value: bit0-3=上下左右)
//   CC 0x11: ボタンビットマップ一括 (value: bit0-1=ボタン1-2)
//
// Channel 1: X68000 キーボード制御
//   Note On:  note=キーコード → キー押下 (make)
//   Note Off: note=キーコード → キー解放 (break)
//
// Channel 15: デバイス→ホスト通知
//   CC 0x20: LED 状態変化通知

#define MIDI_CH_JOYSTICK  0
#define MIDI_CH_KEYBOARD  1
#define MIDI_CH_NOTIFY    15

#define CC_JOY_DIRECTION  0x10
#define CC_JOY_BUTTONS    0x11
#define CC_LED_STATUS     0x20

static void process_midi_event(uint8_t cin, uint8_t midi0, uint8_t midi1, uint8_t midi2) {
    uint8_t channel = midi0 & 0x0F;

    switch (cin) {
    case CIN_NOTE_ON:
        if (channel == MIDI_CH_KEYBOARD) {
            // キー押下: midi1 = キーコード
            x68k_keyboard_send(midi1 & 0x7F);  // bit7=0 → make
        }
        break;

    case CIN_NOTE_OFF:
        if (channel == MIDI_CH_KEYBOARD) {
            // キー解放: midi1 = キーコード
            x68k_keyboard_send(0x80 | (midi1 & 0x7F));  // bit7=1 → break
        }
        break;

    case CIN_CONTROL_CHANGE:
        if (channel == MIDI_CH_JOYSTICK) {
            if (midi1 == CC_JOY_DIRECTION) {
                joystick_update(midi2 & 0x0F, 0xFF);  // 方向のみ更新（ボタンは変更なし）
            } else if (midi1 == CC_JOY_BUTTONS) {
                joystick_update(0xFF, midi2 & 0x03);  // ボタンのみ更新
            }
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

        // X68000 本体からの LED コマンド受信
        int led_cmd = x68k_keyboard_receive();
        if (led_cmd >= 0) {
            // LED 状態を USB-MIDI 経由でスマホに通知
            usb_midi_control_change(MIDI_CH_NOTIFY, CC_LED_STATUS, (uint8_t)led_cmd);
        }

        // USB-MIDI TX バッファをフラッシュ
        usb_midi_poll();
    }
}
