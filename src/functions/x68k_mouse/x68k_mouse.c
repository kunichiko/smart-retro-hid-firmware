// ===================================================================================
// X68000 マウス機能
// ===================================================================================
// プロトコル:
//   4800bps 8N2, TTL 5V
//   3 byte パケット (本体 → MCU → 本体 への片方向):
//     byte 0: [Y_UNF Y_OVF X_UNF X_OVF . . RightBtn LeftBtn]
//     byte 1: dX (signed 8bit, 右が正)
//     byte 2: dY (signed 8bit, 下が正)
//
//   注: 旧版の Inside X68000 では bit0=R, bit1=L とあったが、正誤表
//       (https://kg68k.github.io/InsideX68000-errata/) で訂正されており、
//       bit0=L, bit1=R が正しい。実機検証もこの方向で一致。
//
// ピン (CH32X035 C8T6):
//   PA18: USART3_TX (MSDATA, AFIO PCFR1 USART3_REMAP = 10)
//   PA19: MSCTRL モニタ出力 (デバッグ用、M ビットをそのまま出力 = 1:HIGH/0:LOW)
//
// 本体は KEY RxD (= USART2 RX = PA16) 経由で MSCTRL コマンド (0b01000xxM) を
// 送る。M=1 でマウスデータ要求。検出はキーボードモジュール側で行い、
// x68k_mouse_handle_msctrl() を呼んで本モジュールから 3 byte 送出する。
// ===================================================================================
#include "x68k_mouse.h"
#include "ch32fun.h"
#include "funconfig.h"

// ---------------------------------------------------------------------------
// 設定
// ---------------------------------------------------------------------------

#define MIDI_CH_MOUSE  2

#define NOTE_LEFT   0
#define NOTE_RIGHT  1

#define CC_DX  0x30
#define CC_DY  0x31

// ---------------------------------------------------------------------------
// 内部状態
// ---------------------------------------------------------------------------

// byte 0: bit0 = 左ボタン, bit1 = 右ボタン (Inside X68000 正誤表に従う)
#define BTN_LEFT_MASK   0x01
#define BTN_RIGHT_MASK  0x02

static volatile uint8_t  s_buttons = 0;
static volatile int16_t  s_accum_dx = 0;
static volatile int16_t  s_accum_dy = 0;
// 前回の MSCTRL 状態 (M ビット): エッジ検出用。初期値 1 (アイドル) で初回 M=0 を H→L エッジとして扱う
static volatile uint8_t  s_prev_m = 1;

// ---------------------------------------------------------------------------
// UART 駆動 (USART3 TX のみ)
// ---------------------------------------------------------------------------

static void uart_init(void) {
    RCC->APB1PCENR |= RCC_USART3EN;

    // USART3 を PA18(TX) / PB14(RX) にリマップ (PCFR1 USART3_REMAP = 0b10)
    AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_USART3_REMAP) | AFIO_PCFR1_USART3_REMAP_1;

    // PA18 (TX): Push-Pull AF 出力, PA19: 汎用 Push-Pull 出力 (MSCTRL モニタ)
    // CFGXR pin 18 = bits[11:8], pin 19 = bits[15:12]
    GPIOA->CFGXR &= ~((0xf << (4 * (18 - 16))) | (0xf << (4 * (19 - 16))));
    GPIOA->CFGXR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP_AF) << (4 * (18 - 16));
    GPIOA->CFGXR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP)    << (4 * (19 - 16));

    // PA19 デフォルト HIGH (MSCTRL アイドル, M=1 と同じ状態)
    GPIOA->BSXR = GPIO_BSXR_BS19;

    // 4800bps 8N2 (TX のみ使用、RX 不要)
    USART3->BRR = F_CPU / 4800;
    USART3->CTLR2 = USART_CTLR2_STOP_1;  // STOP[13:12] = 10 → 2 stop bits
    USART3->CTLR1 = USART_CTLR1_TE | USART_CTLR1_UE;
}

static void uart_send(uint8_t byte) {
    while (!(USART3->STATR & USART_STATR_TXE));
    USART3->DATAR = byte;
}

// ---------------------------------------------------------------------------
// MSCTRL ハンドラ (キーボードモジュールから呼ばれる)
// ---------------------------------------------------------------------------

void x68k_mouse_handle_msctrl(uint8_t cmd) {
    const uint8_t m = cmd & 0x01;

    // PA19 = M ビットそのまま (MSCTRL: M=1 HIGH アイドル / M=0 LOW 要求)
    if (m) {
        GPIOA->BSXR = GPIO_BSXR_BS19;
    } else {
        GPIOA->BSXR = GPIO_BSXR_BR19;
    }

    // H→L エッジ検出: 前回 M=1 (HIGH) → 今回 M=0 (LOW) のときに送信を起動
    const uint8_t falling = (s_prev_m == 1) && (m == 0);
    s_prev_m = m;
    if (!falling) return;

    // 移動量を [-128, +127] に飽和し、超過分を OVF/UNF フラグで通知
    uint8_t  status = s_buttons & (BTN_LEFT_MASK | BTN_RIGHT_MASK);
    int16_t  dx = s_accum_dx;
    int16_t  dy = s_accum_dy;

    if (dx > 127) {
        status |= 0x10;  // X_OVF
        dx = 127;
    } else if (dx < -128) {
        status |= 0x20;  // X_UNF
        dx = -128;
    }
    if (dy > 127) {
        status |= 0x40;  // Y_OVF
        dy = 127;
    } else if (dy < -128) {
        status |= 0x80;  // Y_UNF
        dy = -128;
    }

    // 3 byte 連続送信 (送信中 ~2.5ms)
    uart_send(status);
    uart_send((uint8_t)(int8_t)dx);
    uart_send((uint8_t)(int8_t)dy);

    // 送信した分だけ蓄積から差し引く (オーバーフロー時の残りは次回へ持ち越し)
    s_accum_dx -= dx;
    s_accum_dy -= dy;
}

// ---------------------------------------------------------------------------
// hid_function インターフェース
// ---------------------------------------------------------------------------

static void mouse_init(void) {
    uart_init();
    s_buttons = 0;
    s_accum_dx = 0;
    s_accum_dy = 0;
    s_prev_m = 1;  // アイドル状態として初期化
}

static void mouse_release_all(void) {
    s_buttons = 0;
}

static void mouse_on_note_on(uint8_t note, uint8_t velocity) {
    (void)velocity;
    if (note == NOTE_RIGHT) {
        s_buttons |= BTN_RIGHT_MASK;
    } else if (note == NOTE_LEFT) {
        s_buttons |= BTN_LEFT_MASK;
    }
}

static void mouse_on_note_off(uint8_t note) {
    if (note == NOTE_RIGHT) {
        s_buttons &= (uint8_t)~BTN_RIGHT_MASK;
    } else if (note == NOTE_LEFT) {
        s_buttons &= (uint8_t)~BTN_LEFT_MASK;
    }
}

static void mouse_on_cc(uint8_t cc, uint8_t value) {
    // value はオフセット表現 (64 = 0, 0 = -64, 127 = +63)
    int16_t delta = (int16_t)value - 64;
    if (cc == CC_DX) {
        s_accum_dx += delta;
    } else if (cc == CC_DY) {
        s_accum_dy += delta;
    }
}

const hid_function_t x68k_mouse_function = {
    .name              = "x68k-mouse",
    .hid_type          = HID_TYPE_MOUSE,
    .target_system     = TARGET_X68000,
    .midi_channel      = MIDI_CH_MOUSE,
    .init              = mouse_init,
    .release_all       = mouse_release_all,
    .on_note_on        = mouse_on_note_on,
    .on_note_off       = mouse_on_note_off,
    .on_cc             = mouse_on_cc,
    .on_set_config     = NULL,
    .poll              = NULL,
    .append_capabilities = NULL,
};
