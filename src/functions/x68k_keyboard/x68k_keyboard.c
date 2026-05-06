// ===================================================================================
// X68000 キーボード機能
// ===================================================================================
// プロトコル:
//   2400bps 8N1, TTL 5V
//   キーボード→本体: 1 byte/key (bit7=1:break, 0:make / bit6-0:scancode)
//   本体→キーボード: LED 制御 (bit7=1) / リピート設定 (0x60-0x7F) など
//
// ピン (CH32X035 C8T6 - PC0/PC1 が外に出ていないため USART2 リマップ):
//   PA15: USART2_TX  (キーコード送信)
//   PA16: USART2_RX  (本体→キーボードのコマンド受信)
//   PA17: READY      (本体→キーボード, 1=ホスト準備完了, 0=送信抑止)
//   AFIO->PCFR1 USART2_REMAP = 0b010
// ===================================================================================
#include "x68k_keyboard.h"
#include "ch32fun.h"
#include "funconfig.h"
#include "../../usb_midi.h"
#include "../x68k_mouse/x68k_mouse.h"

// ---------------------------------------------------------------------------
// 設定
// ---------------------------------------------------------------------------

#define MIDI_CH_KEYBOARD  1   // ホスト→デバイス: キー押下/解放
                              // デバイス→ホスト: ターゲット機受信バイト (SysEx TARGET_RX)

// SysEx TARGET_RX (0x05): ターゲット機から受信した生バイトをホストへ転送
#define SYSEX_CMD_TARGET_RX  0x05

// ---------------------------------------------------------------------------
// 送信キュー (READY=Low の間にキーが来ても取りこぼさないため)
// ---------------------------------------------------------------------------

#define TX_QUEUE_SIZE 32  // 2 のべき乗にしておくと head/tail のラップが安い
static volatile uint8_t  tx_queue[TX_QUEUE_SIZE];
static volatile uint16_t tx_head = 0;  // 次に書き込む位置
static volatile uint16_t tx_tail = 0;  // 次に読み出す位置

static inline int tx_queue_empty(void) { return tx_head == tx_tail; }

static inline void tx_queue_push(uint8_t byte) {
    uint16_t next = (tx_head + 1) % TX_QUEUE_SIZE;
    if (next == tx_tail) return;  // フル: 古いほうを優先して新規を捨てる
    tx_queue[tx_head] = byte;
    tx_head = next;
}

// READY 信号 (PA17): 1=ホスト準備完了, 0=送信抑止
static inline int host_ready(void) {
    return (GPIOA->INDR & GPIO_INDR_IDR17) != 0;
}

// ---------------------------------------------------------------------------
// UART 駆動
// ---------------------------------------------------------------------------

static void uart_init(void) {
    RCC->APB1PCENR |= RCC_USART2EN;

    // USART2 を PA15(TX) / PA16(RX) にリマップ (PCFR1 USART2_REMAP = 0b010)
    AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_USART2_REMAP) | AFIO_PCFR1_USART2_REMAP_1;

    // PA15 (TX): Push-Pull AF 出力 — CFGHR bit field for pin 15
    GPIOA->CFGHR &= ~(0xf << (4 * (15 - 8)));
    GPIOA->CFGHR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP_AF) << (4 * (15 - 8));

    // PA16 (RX): フローティング入力 — CFGXR bit field for pin 16
    // PA17 (READY): フローティング入力 — CFGXR bit field for pin 17
    GPIOA->CFGXR &= ~((0xf << (4 * (16 - 16))) | (0xf << (4 * (17 - 16))));
    GPIOA->CFGXR |=  ((GPIO_Speed_In | GPIO_CNF_IN_FLOATING) << (4 * (16 - 16))) |
                     ((GPIO_Speed_In | GPIO_CNF_IN_FLOATING) << (4 * (17 - 16)));

    // 2400bps 8N1
    USART2->BRR = F_CPU / 2400;
    USART2->CTLR1 = USART_CTLR1_TE | USART_CTLR1_RE;
    USART2->CTLR1 |= USART_CTLR1_UE;
}

// TX レジスタが空いていて READY=High なら 1 byte 送り出す
static int uart_try_send(uint8_t byte) {
    if (!host_ready()) return 0;
    if (!(USART2->STATR & USART_STATR_TXE)) return 0;
    USART2->DATAR = byte;
    return 1;
}

// 受信データがあれば返す。なければ -1
static int uart_receive(void) {
    if (USART2->STATR & USART_STATR_RXNE) {
        return (uint8_t)USART2->DATAR;
    }
    return -1;
}

// キューにある間、可能な限り送り出す
static void drain_tx_queue(void) {
    uint8_t byte;
    while (!tx_queue_empty()) {
        // peek: 送信できるか試して、成功した時だけキューから取り除く
        byte = tx_queue[tx_tail];
        if (!uart_try_send(byte)) return;
        tx_tail = (tx_tail + 1) % TX_QUEUE_SIZE;
    }
}

// ---------------------------------------------------------------------------
// ホスト→キーボード コマンド処理
// ---------------------------------------------------------------------------

// 受信バイトを SysEx TARGET_RX (0x05) で生のままホストへ転送する。
// アプリ側でターゲット機固有のコマンド (LED 制御 / キーリピート設定 /
// LED 輝度など) を解釈する。
//
// SysEx レイアウト:
//   F0 7D 01 05 <midi_channel> <byte_hi4> <byte_lo4> F7
static void forward_target_rx(uint8_t byte) {
    uint8_t sysex[8] = {
        0xF0, 0x7D, 0x01, SYSEX_CMD_TARGET_RX,
        MIDI_CH_KEYBOARD,
        (uint8_t)((byte >> 4) & 0x0F),
        (uint8_t)(byte & 0x0F),
        0xF7,
    };
    usb_midi_send_sysex(sysex, sizeof(sysex));
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
    tx_queue_push(note & 0x7F);
}

static void x68k_kb_on_note_off(uint8_t note) {
    // bit7=1 = break
    tx_queue_push(0x80 | (note & 0x7F));
}

static void x68k_kb_poll(void) {
    // RX: ターゲット機から届いた生バイトを処理
    int byte = uart_receive();
    if (byte >= 0) {
        const uint8_t b = (uint8_t)byte;
        // 0b01000xxM (0x40-0x47): MSCTRL コマンド → 内蔵マウスサブシステムへ
        // (アプリにも転送する必要はないので TARGET_RX には流さない)
        if ((b & 0xF8) == 0x40) {
            x68k_mouse_handle_msctrl(b);
        } else {
            forward_target_rx(b);
        }
    }
    // TX: READY=High かつ TXE=1 の間、キューから送り出す
    drain_tx_queue();
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
