// ===================================================================================
// Joystick / Gamepad emulation for ATARI and Mega Drive 6-button
// ===================================================================================
// ATARI mode:  GPIO direct output (open-drain)
// MD 6B mode:  TH (PA0) edge interrupt driven, 8-step cycle with 1.8ms timeout
// ===================================================================================

// MD 6B の TH エッジ応答性を高めるため、このファイルだけ O3 で最適化
#pragma GCC optimize ("O3")

#include "joystick.h"
#include "ch32fun.h"
#include "funconfig.h"

// ---------------------------------------------------------------------------
// ボタン状態 (全モード共通、1=押下, 0=解放)
// ---------------------------------------------------------------------------
static volatile uint8_t btn_state[BTN_COUNT];
static volatile uint8_t pad_mode = PAD_MODE_ATARI;

// MD 6B: TH サイクルカウンタ (0-7)
static volatile uint8_t th_cycle;

// MIDI Note → ボタンインデックス変換テーブル
static int note_to_btn(uint8_t note) {
    switch (note) {
    case 1:  return BTN_UP;
    case 2:  return BTN_DOWN;
    case 3:  return BTN_LEFT;
    case 4:  return BTN_RIGHT;
    case 6:  return BTN_A;
    case 7:  return BTN_B;
    case 9:  return BTN_C;
    case 10: return BTN_START;
    case 11: return BTN_X;
    case 12: return BTN_Y;
    case 13: return BTN_Z;
    case 14: return BTN_MODE;
    default: return -1;
    }
}

// ---------------------------------------------------------------------------
// GPIO 出力ヘルパー
// ---------------------------------------------------------------------------

// PA の指定ピンを Low (アクティブ) または Hi-Z (非アクティブ) にする
// active-low: pressed(1) → Low, released(0) → Hi-Z
static inline void set_pin(uint8_t pin, uint8_t active) {
    if (active) {
        GPIOA->BCR = (1 << pin);
    } else {
        GPIOA->BSHR = (1 << pin);
    }
}

// D0-D5 を一括設定 (割り込みハンドラ用、高速)
// 各ビットが 1 = Low (アクティブ), 0 = Hi-Z
static inline void set_d0_d5(uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4, uint8_t d5) {
    // BCR でセット (Low出力)、BSHR でクリア (Hi-Z)
    uint32_t bcr = 0;  // Low にするピン
    uint32_t bshr = 0; // High にするピン

    if (d0) bcr |= (1 << PIN_D0); else bshr |= (1 << PIN_D0);
    if (d1) bcr |= (1 << PIN_D1); else bshr |= (1 << PIN_D1);
    if (d2) bcr |= (1 << PIN_D2); else bshr |= (1 << PIN_D2);
    if (d3) bcr |= (1 << PIN_D3); else bshr |= (1 << PIN_D3);
    if (d4) bcr |= (1 << PIN_D4); else bshr |= (1 << PIN_D4);
    if (d5) bcr |= (1 << PIN_D5); else bshr |= (1 << PIN_D5);

    GPIOA->BCR = bcr;
    GPIOA->BSHR = bshr;
}

// ---------------------------------------------------------------------------
// MD 6B: 事前計算ルックアップテーブル
// ---------------------------------------------------------------------------
// インデックス 0..7 = step 1..8 (TH の各エッジに対応)
// step 0 = TH=LOW (Step 1)
// step 1 = TH=HIGH (Step 2)
// step 2 = TH=LOW (Step 3)
// step 3 = TH=HIGH (Step 4)
// step 4 = TH=LOW (Step 5, 6B 識別)
// step 5 = TH=HIGH (Step 6, 拡張ボタン)
// step 6 = TH=LOW (Step 7, 確認)
// step 7 = TH=HIGH (Step 8)
//
// 値は BSHR への単一 32bit 書き込み:
//   下位 16bit: BSx (Hi-Z にするピン) — オープンドレイン出力で High = Hi-Z
//   上位 16bit: BCx (Low にするピン)
static volatile uint32_t md6_lut[8];

#define D_PINS_MASK ((1<<PIN_D0)|(1<<PIN_D1)|(1<<PIN_D2)|(1<<PIN_D3)|(1<<PIN_D4)|(1<<PIN_D5))

// btn_state を元に LUT を再計算する
// active=1 のビットを BCR (Low)、active=0 を BSHR (Hi-Z) に振り分ける
static void md6_rebuild_lut(void) {
    uint8_t up    = btn_state[BTN_UP];
    uint8_t down  = btn_state[BTN_DOWN];
    uint8_t left  = btn_state[BTN_LEFT];
    uint8_t right = btn_state[BTN_RIGHT];
    uint8_t a     = btn_state[BTN_A];
    uint8_t b     = btn_state[BTN_B];
    uint8_t c     = btn_state[BTN_C];
    uint8_t start = btn_state[BTN_START];
    uint8_t x     = btn_state[BTN_X];
    uint8_t y     = btn_state[BTN_Y];
    uint8_t z     = btn_state[BTN_Z];
    uint8_t mode  = btn_state[BTN_MODE];

    // d0..d5 各ピンの active 値を 8 ステップ分定義
    // step[i][0..5] = D0..D5 の active 値 (1=Low, 0=Hi-Z)
    static const uint8_t pins[6] = {PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5};

    // ステップごとの値テーブルを構築
    uint8_t step_vals[8][6] = {
        // Step 1 (cycle 0, LOW): Up, Down, 0, 0, A, Start  ※D2/D3 は固定 Low (=1)
        { up, down, 1, 1, a, start },
        // Step 2 (cycle 0, HIGH): Up, Down, Left, Right, B, C
        { up, down, left, right, b, c },
        // Step 3 (cycle 1, LOW)
        { up, down, 1, 1, a, start },
        // Step 4 (cycle 1, HIGH)
        { up, down, left, right, b, c },
        // Step 5 (cycle 2, LOW): D0-D3 全 Low (=1) = 6B 識別
        { 1, 1, 1, 1, a, start },
        // Step 6 (cycle 2, HIGH): Z, Y, X, Mode, 1(Hi-Z), 1(Hi-Z)
        { z, y, x, mode, 0, 0 },
        // Step 7 (cycle 3, LOW): D0-D3 全 Hi-Z (=0) = 確認
        { 0, 0, 0, 0, a, start },
        // Step 8 (cycle 3, HIGH): 通常状態に復帰
        { up, down, left, right, b, c },
    };

    // BSHR (= BSRR) フォーマット: 上位16bit = Low にするピン, 下位16bit = High (Hi-Z) にするピン
    for (int s = 0; s < 8; s++) {
        uint32_t set = 0, reset = 0;
        for (int p = 0; p < 6; p++) {
            uint32_t bit = 1u << pins[p];
            if (step_vals[s][p]) reset |= bit;  // active = Low
            else                 set   |= bit;  // inactive = Hi-Z (Pull-up で High)
        }
        md6_lut[s] = set | (reset << 16);
    }
}

// ---------------------------------------------------------------------------
// ATARI モード: GPIO 直接出力
// ---------------------------------------------------------------------------

static void atari_update_gpio(void) {
    set_pin(PIN_D0, btn_state[BTN_UP]);
    set_pin(PIN_D1, btn_state[BTN_DOWN]);
    set_pin(PIN_D2, btn_state[BTN_LEFT]);
    set_pin(PIN_D3, btn_state[BTN_RIGHT]);
    set_pin(PIN_D4, btn_state[BTN_A]);
    set_pin(PIN_D5, btn_state[BTN_B]);
}

// ---------------------------------------------------------------------------
// TH 割り込みハンドラ (EXTI line 0, PA0)
// ---------------------------------------------------------------------------

// 最初の TH エッジで割り込み発生 → そのまま ISR 内でポーリングで全ステップ処理
// ポインタをローカル const に置いてループ外でレジスタ確保することで内ループの命令数を減らす

// 戻り値: 1=エッジ検出, 0=タイムアウト
static inline __attribute__((always_inline))
int wait_th_state(volatile uint32_t* indr, uint32_t mask, uint32_t target, uint32_t timeout) {
    uint32_t v;
    do {
        v = (*indr) & mask;
        if ((target ? v : !v)) return 1;
    } while (--timeout);
    return 0;
}

void EXTI7_0_IRQHandler(void) __attribute__((interrupt, optimize("O3")));
void EXTI7_0_IRQHandler(void) {
    EXTI->INTFR = (1 << PIN_TH);

    volatile uint32_t* const indr_p = &GPIOA->INDR;
    volatile uint32_t* const bshr_p = &GPIOA->BSHR;
    const uint32_t mask = (1 << PIN_TH);
    const uint32_t timeout = 1000;

    // 最初のエッジ (TH=LOW = Step 1): すぐ出力
    *bshr_p = md6_lut[0];

    if (!wait_th_state(indr_p, mask, 1, timeout)) goto done;
    *bshr_p = md6_lut[1];
    if (!wait_th_state(indr_p, mask, 0, timeout)) goto done;
    *bshr_p = md6_lut[2];
    if (!wait_th_state(indr_p, mask, 1, timeout)) goto done;
    *bshr_p = md6_lut[3];
    if (!wait_th_state(indr_p, mask, 0, timeout)) goto done;
    *bshr_p = md6_lut[4];
    if (!wait_th_state(indr_p, mask, 1, timeout)) goto done;
    *bshr_p = md6_lut[5];
    if (!wait_th_state(indr_p, mask, 0, timeout)) goto done;
    *bshr_p = md6_lut[6];
    if (!wait_th_state(indr_p, mask, 1, timeout)) goto done;
    *bshr_p = md6_lut[7];

done:
    // アイドル時 (TH=HIGH) の Step 2 状態に戻す
    *bshr_p = md6_lut[1];

    // ポーリング中に発生した falling edge による pending を全クリア
    // EXTI フラグ (W1C) と NVIC pending の両方
    EXTI->INTFR = (1 << PIN_TH);
    NVIC->IPRR[EXTI7_0_IRQn / 32] = (1 << (EXTI7_0_IRQn % 32));
}

// ---------------------------------------------------------------------------
// TIM2 タイムアウト割り込み (1.8ms でサイクルカウンタリセット)
// ---------------------------------------------------------------------------

// (TIM2 タイマー割り込みは ISR 内ポーリング方式では不要)

// ---------------------------------------------------------------------------
// 初期化
// ---------------------------------------------------------------------------

static void gpio_init_output_pins(void) {
    // D0-D5 をオープンドレイン出力 (3.3V/5V 共用、レトロ PC のプルアップ前提)
    const uint8_t out_pins[] = { PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5 };
    for (int i = 0; i < (int)(sizeof(out_pins) / sizeof(out_pins[0])); i++) {
        uint8_t p = out_pins[i];
        GPIOA->CFGLR &= ~(0xf << (4 * p));
        GPIOA->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_OD) << (4 * p);
        GPIOA->BSHR = (1 << p);  // Hi-Z (非アクティブ)
    }
}

static void gpio_init_th_input(void) {
    // PA0 (TH): 入力、プルダウン
    GPIOA->CFGLR &= ~(0xf << (4 * PIN_TH));
    GPIOA->CFGLR |= (GPIO_Speed_In | GPIO_CNF_IN_PUPD) << (4 * PIN_TH);
    GPIOA->BCR = (1 << PIN_TH);  // Pull-Down
}

static void exti_init(void) {
    // PA0 を EXTI0 に接続 (PA = 0)
    AFIO->EXTICR1 = (AFIO->EXTICR1 & ~(0x0F << (4 * PIN_TH))) | (0x00 << (4 * PIN_TH));

    // 立ち下がりエッジのみで割り込み (最初の TH=LOW を捕捉)
    // 残りの 7 ステップは ISR 内のポーリングで処理する
    EXTI->INTENR |= (1 << PIN_TH);
    EXTI->RTENR &= ~(1 << PIN_TH);
    EXTI->FTENR |= (1 << PIN_TH);

    NVIC_EnableIRQ(EXTI7_0_IRQn);
}

void joystick_init(void) {
    for (int i = 0; i < BTN_COUNT; i++) btn_state[i] = 0;
    th_cycle = 0;

    gpio_init_output_pins();
    gpio_init_th_input();
}

void joystick_set_mode(uint8_t mode) {
    pad_mode = mode;
    joystick_release_all();
    th_cycle = 0;

    if (mode == PAD_MODE_MD6) {
        md6_rebuild_lut();
        exti_init();
        // 初期出力: 現在の TH 状態に応じて step 0 または 1
        uint8_t th_high = (GPIOA->INDR >> PIN_TH) & 1;
        GPIOA->BSHR = md6_lut[th_high & 1];
    } else {
        // EXTI 無効化
        EXTI->INTENR &= ~(1 << PIN_TH);
        NVIC_DisableIRQ(EXTI7_0_IRQn);
        atari_update_gpio();
    }
}

uint8_t joystick_get_mode(void) {
    return pad_mode;
}

void joystick_set_button_by_note(uint8_t note, uint8_t pressed) {
    int btn_idx = note_to_btn(note);
    if (btn_idx < 0) return;
    btn_state[btn_idx] = pressed ? 1 : 0;

    if (pad_mode == PAD_MODE_ATARI) {
        atari_update_gpio();
    } else if (pad_mode == PAD_MODE_MD6) {
        // LUT 再構築 (ボタン状態が変わったため)
        md6_rebuild_lut();
        // 現在の TH 状態に応じて即座に出力 (cycle=0 として)
        uint8_t th_high = (GPIOA->INDR >> PIN_TH) & 1;
        GPIOA->BSHR = md6_lut[th_high & 1];
    }
}

void joystick_release_all(void) {
    for (int i = 0; i < BTN_COUNT; i++) btn_state[i] = 0;
    if (pad_mode == PAD_MODE_ATARI) {
        atari_update_gpio();
    }
}

void joystick_poll(void) {
    // ATARI モード: 特に何もしない (set_button_by_note で即座に更新済み)
    // MD6 モード: 割り込みハンドラが処理するので何もしない
}
