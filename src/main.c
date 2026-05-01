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

// ---------------------------------------------------------------------------
// GPIO 初期化
// ---------------------------------------------------------------------------

// ATARI ジョイスティック用 GPIO (出力)
// PA0: 上, PA1: 下, PA2: 左, PA3: 右, PA4: ボタン1, PA5: ボタン2
static void gpio_init_joystick(void) {
    // PA0-PA5 を Push-Pull 出力に設定
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
    // USART1 クロック有効化
    RCC->APB2PCENR |= RCC_USART1EN;

    // PC1 (TX): Push-Pull Alternate Function 出力
    GPIOC->CFGLR &= ~(0xf << (4 * 1));
    GPIOC->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP_AF) << (4 * 1);

    // PC0 (RX): フローティング入力
    GPIOC->CFGLR &= ~(0xf << (4 * 0));
    GPIOC->CFGLR |= (GPIO_Speed_In | GPIO_CNF_IN_FLOATING) << (4 * 0);

    // USART1 を PC0/PC1 にリマップ (必要に応じて)
    // AFIO->PCFR1 で設定

    // USART1 設定: 2400bps, 8N1
    // BRR = F_CPU / baudrate = 48000000 / 2400 = 20000
    USART1->BRR = 20000;
    USART1->CTLR1 = USART_CTLR1_TE | USART_CTLR1_RE;  // TX, RX 有効
    USART1->CTLR1 |= USART_CTLR1_UE;                   // USART 有効
}

// ---------------------------------------------------------------------------
// ATARI ジョイスティック制御
// ---------------------------------------------------------------------------

// ジョイスティック状態を GPIO に反映する
// directions: bit0=上, bit1=下, bit2=左, bit3=右
// buttons:    bit0=ボタン1, bit1=ボタン2
// 各ビット 1=押下, 0=解放
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

// キーコードを X68000 本体に送信する
// keycode: bit7=0:押下/1:解放, bit6-0=キーコード
static void x68k_keyboard_send(uint8_t keycode) {
    while (!(USART1->STATR & USART_STATR_TXE))
        ;
    USART1->DATAR = keycode;
}

// X68000 本体からの LED コマンドを受信する (ポーリング)
// 戻り値: 受信データ (受信なしの場合は -1)
static int x68k_keyboard_receive(void) {
    if (USART1->STATR & USART_STATR_RXNE) {
        return (uint8_t)USART1->DATAR;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// USB-MIDI (stub)
// ---------------------------------------------------------------------------

// TODO: USB-MIDI デバイスの初期化
// ch32v003fun の USB ドライバ、または TinyUSB を使用して
// USB-MIDI デバイスとして動作させる
static void usb_midi_init(void) {
    // USB クロック有効化
    // RCC->APB1PCENR |= RCC_USBEN;

    // TODO: USB デバイス初期化
    // TODO: MIDI デスクリプタ設定
}

// USB-MIDI からデータを受信する (stub)
// 戻り値: 受信したMIDIメッセージ長 (受信なしの場合は 0)
static int usb_midi_receive(uint8_t* buf, int max_len) {
    (void)buf;
    (void)max_len;
    // TODO: 実装
    return 0;
}

// USB-MIDI にデータを送信する (stub)
static void usb_midi_send(const uint8_t* buf, int len) {
    (void)buf;
    (void)len;
    // TODO: 実装
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

    // メインループ
    uint8_t midi_buf[64];

    while (1) {
        // USB-MIDI からコマンド受信
        int midi_len = usb_midi_receive(midi_buf, sizeof(midi_buf));
        if (midi_len > 0) {
            // TODO: プロトコルに従ってコマンドをディスパッチ
            // - ジョイスティック制御コマンド → joystick_update()
            // - キーボード制御コマンド → x68k_keyboard_send()
        }

        // X68000 本体からの LED コマンド受信
        int led_cmd = x68k_keyboard_receive();
        if (led_cmd >= 0) {
            // LED 状態を USB-MIDI 経由でスマホに通知
            uint8_t notify[] = {0xF0, 0x7D, 0x01, (uint8_t)led_cmd, 0xF7};
            usb_midi_send(notify, sizeof(notify));
        }
    }
}
