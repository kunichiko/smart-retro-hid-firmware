// ===================================================================================
// X68000 キーボード機能
// ===================================================================================
// プロトコル:
//   2400bps 8N1, TTL 5V
//   キーボード→本体: 1 byte/key (bit7=1:break, 0:make / bit6-0:scancode)
//   本体→キーボード: LED 制御 (bit7=1) / リピート設定 (0x60-0x7F) など
//
// ピン:
//   PC0: USART1_RX (本体→キーボードのコマンド受信)
//   PC1: USART1_TX (キーコード送信)
// ===================================================================================
#include "x68k_keyboard.h"
#include "ch32fun.h"
#include "funconfig.h"
#include "../../usb_midi.h"

// ---------------------------------------------------------------------------
// 設定
// ---------------------------------------------------------------------------

#define MIDI_CH_KEYBOARD  1   // ホスト→デバイス: キー押下/解放
#define MIDI_CH_LED       14  // デバイス→ホスト: LED 状態通知

// ---------------------------------------------------------------------------
// UART 駆動
// ---------------------------------------------------------------------------

static void uart_init(void) {
    RCC->APB2PCENR |= RCC_USART1EN;

    // PC1 (TX): Push-Pull AF 出力
    GPIOC->CFGLR &= ~(0xf << (4 * 1));
    GPIOC->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP_AF) << (4 * 1);

    // PC0 (RX): フローティング入力
    GPIOC->CFGLR &= ~(0xf << (4 * 0));
    GPIOC->CFGLR |= (GPIO_Speed_In | GPIO_CNF_IN_FLOATING) << (4 * 0);

    // 2400bps 8N1
    USART1->BRR = F_CPU / 2400;
    USART1->CTLR1 = USART_CTLR1_TE | USART_CTLR1_RE;
    USART1->CTLR1 |= USART_CTLR1_UE;
}

static void uart_send(uint8_t byte) {
    while (!(USART1->STATR & USART_STATR_TXE));
    USART1->DATAR = byte;
}

// 受信データがあれば返す。なければ -1
static int uart_receive(void) {
    if (USART1->STATR & USART_STATR_RXNE) {
        return (uint8_t)USART1->DATAR;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// ホスト→キーボード コマンド処理
// ---------------------------------------------------------------------------

// LED 状態通知をホストに送る
// X68000 LED コマンド: bit7=1, 各ビット 0=点灯, 1=消灯
//   bit0: かな, bit1: ローマ字, bit2: コード入力, bit3: CAPS,
//   bit4: INS, bit5: ひらがな, bit6: 全角
static void notify_led_state(uint8_t cmd) {
    for (int i = 0; i < 7; i++) {
        // X68000: 0=点灯, 1=消灯 → MIDI: 127=点灯, 0=消灯 に変換
        uint8_t off = (cmd >> i) & 1;
        usb_midi_control_change(MIDI_CH_LED, i, off ? 0 : 127);
    }
}

static void process_received_command(uint8_t cmd) {
    if (cmd & 0x80) {
        // LED 制御
        notify_led_state(cmd);
    }
    // 0x60-0x7F: キーリピート設定 (現状は未対応、受信のみ)
    // TODO: リピート遅延/間隔をホストに通知 or 内部で保持
}

// ---------------------------------------------------------------------------
// hid_function インターフェース
// ---------------------------------------------------------------------------

static void x68k_kb_init(void) {
    uart_init();
}

static void x68k_kb_release_all(void) {
    // 全キーリリースは MIDI 側のステートに任せる (デバイス側はステートレス)
}

static void x68k_kb_on_note_on(uint8_t note, uint8_t velocity) {
    (void)velocity;
    // bit7=0 = make, bit6-0 = スキャンコード
    uart_send(note & 0x7F);
}

static void x68k_kb_on_note_off(uint8_t note) {
    // bit7=1 = break
    uart_send(0x80 | (note & 0x7F));
}

static void x68k_kb_poll(void) {
    int byte = uart_receive();
    if (byte >= 0) {
        process_received_command((uint8_t)byte);
    }
}

static int x68k_kb_append_capabilities(uint8_t* buf, int max_len) {
    int n = 0;
    // CAP_LED_COUNT (0x03) = 7
    if (n + 3 > max_len) return n;
    buf[n++] = 0x03;
    buf[n++] = 1;
    buf[n++] = 7;
    // CAP_KEYCODE_RANGE (0x10): 0x01-0x73
    if (n + 4 > max_len) return n;
    buf[n++] = 0x10;
    buf[n++] = 2;
    buf[n++] = 0x01;
    buf[n++] = 0x73;
    // CAP_BIDI (0x20): 双方向通信対応
    if (n + 3 > max_len) return n;
    buf[n++] = 0x20;
    buf[n++] = 1;
    buf[n++] = 1;
    return n;
}

const hid_function_t x68k_keyboard_function = {
    .name              = "x68k-keyboard",
    .hid_type          = HID_TYPE_KEYBOARD,
    .target_system     = TARGET_X68000,
    .midi_channel      = MIDI_CH_KEYBOARD,
    .init              = x68k_kb_init,
    .release_all       = x68k_kb_release_all,
    .on_note_on        = x68k_kb_on_note_on,
    .on_note_off       = x68k_kb_on_note_off,
    .on_cc             = NULL,
    .on_set_config     = NULL,
    .poll              = x68k_kb_poll,
    .append_capabilities = x68k_kb_append_capabilities,
};
