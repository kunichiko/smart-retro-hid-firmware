// ===================================================================================
// Joystick / Gamepad emulation for ATARI and Mega Drive 6-button
// ===================================================================================
// ATARI mode:  GPIO direct output (open-drain)
// MD 6B mode:  TH (PA0) edge interrupt driven, 8-step cycle with 1.8ms timeout
// ===================================================================================

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
// MD 6B: TH エッジ割り込みで呼ばれる出力処理
// ---------------------------------------------------------------------------

static void md6_output_for_cycle(uint8_t cycle, uint8_t th_high) {
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

    if (th_high) {
        // TH = HIGH
        switch (cycle) {
        case 0: // Step 2
        case 1: // Step 4
        case 3: // Step 8
            // D0=Up, D1=Down, D2=Left, D3=Right, D4=B, D5=C
            set_d0_d5(up, down, left, right, b, c);
            break;
        case 2: // Step 6: 拡張ボタン
            // D0=Z, D1=Y, D2=X, D3=Mode, D4=1(Hi-Z), D5=1(Hi-Z)
            set_d0_d5(z, y, x, mode, 0, 0);
            break;
        }
    } else {
        // TH = LOW
        switch (cycle) {
        case 0: // Step 1
        case 1: // Step 3
            // D0=Up, D1=Down, D2=0(Low), D3=0(Low), D4=A, D5=Start
            set_d0_d5(up, down, 1, 1, a, start);
            break;
        case 2: // Step 5: 6B 識別 (D0-D3 全て Low)
            // D0=0, D1=0, D2=0, D3=0, D4=A, D5=Start
            set_d0_d5(1, 1, 1, 1, a, start);
            break;
        case 3: // Step 7: 確認 (D0-D3 全て High)
            // D0=1, D1=1, D2=1, D3=1, D4=A, D5=Start
            set_d0_d5(0, 0, 0, 0, a, start);
            break;
        }
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

void EXTI7_0_IRQHandler(void) __attribute__((interrupt));
void EXTI7_0_IRQHandler(void) {
    if (EXTI->INTFR & (1 << PIN_TH)) {
        EXTI->INTFR = (1 << PIN_TH);  // フラグクリア

        if (pad_mode != PAD_MODE_MD6) return;

        uint8_t th_high = (GPIOA->INDR >> PIN_TH) & 1;

        // TH の立ち上がりでサイクルを進める
        if (th_high) {
            // サイクル内の HIGH フェーズ
            md6_output_for_cycle(th_cycle, 1);
        } else {
            // サイクルの LOW フェーズ → 次のサイクルに進む（最初の LOW は cycle 0）
            if (th_cycle < 3) {
                // 立ち下がりでは cycle は進めない (HIGH→LOW が1ステップ)
            }
            md6_output_for_cycle(th_cycle, 0);
        }

        // TH の立ち上がりでサイクルカウンタを進める
        if (th_high && th_cycle < 3) {
            th_cycle++;
        }

        // タイマーリセット (1.8ms タイムアウト)
        TIM2->CNT = 0;
        TIM2->CTLR1 |= TIM_CEN;  // タイマー開始/再開
    }
}

// ---------------------------------------------------------------------------
// TIM2 タイムアウト割り込み (1.8ms でサイクルカウンタリセット)
// ---------------------------------------------------------------------------

void TIM2_IRQHandler(void) __attribute__((interrupt));
void TIM2_IRQHandler(void) {
    if (TIM2->INTFR & TIM_UIF) {
        TIM2->INTFR = ~TIM_UIF;  // フラグクリア
        TIM2->CTLR1 &= ~TIM_CEN;  // タイマー停止
        th_cycle = 0;

        // タイムアウト後は TH=HIGH のデフォルト状態を出力
        if (pad_mode == PAD_MODE_MD6) {
            md6_output_for_cycle(0, 1);
        }
    }
}

// ---------------------------------------------------------------------------
// 初期化
// ---------------------------------------------------------------------------

static void gpio_init_output_pins(void) {
    // D0-D5 をオープンドレイン出力
    const uint8_t out_pins[] = { PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5 };
    for (int i = 0; i < (int)(sizeof(out_pins) / sizeof(out_pins[0])); i++) {
        uint8_t p = out_pins[i];
        GPIOA->CFGLR &= ~(0xf << (4 * p));
        GPIOA->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_OD) << (4 * p);
        GPIOA->BSHR = (1 << p);  // Hi-Z
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

    // 両エッジで割り込み
    EXTI->INTENR |= (1 << PIN_TH);
    EXTI->RTENR |= (1 << PIN_TH);   // 立ち上がり
    EXTI->FTENR |= (1 << PIN_TH);   // 立ち下がり

    NVIC_EnableIRQ(EXTI7_0_IRQn);
}

static void timer_init(void) {
    // TIM2: 1.8ms ワンショットタイマー
    RCC->APB1PCENR |= RCC_TIM2EN;

    TIM2->CTLR1 = 0;
    TIM2->PSC = 48 - 1;          // 48MHz / 48 = 1MHz (1us per tick)
    TIM2->ATRLR = 1800 - 1;      // 1800us = 1.8ms
    TIM2->CTLR1 |= TIM_OPM;     // ワンショットモード
    TIM2->DMAINTENR |= TIM_UIE;  // 更新割り込み有効
    TIM2->INTFR = 0;             // フラグクリア

    NVIC_EnableIRQ(TIM2_IRQn);
}

void joystick_init(void) {
    for (int i = 0; i < BTN_COUNT; i++) btn_state[i] = 0;
    th_cycle = 0;

    gpio_init_output_pins();
    gpio_init_th_input();
    timer_init();
    // EXTI は MD6 モードに切り替えたときに有効化
}

void joystick_set_mode(uint8_t mode) {
    pad_mode = mode;
    joystick_release_all();
    th_cycle = 0;

    if (mode == PAD_MODE_MD6) {
        exti_init();
        // デフォルト: TH=HIGH の出力
        md6_output_for_cycle(0, 1);
    } else {
        // EXTI 無効化
        EXTI->INTENR &= ~(1 << PIN_TH);
        NVIC_DisableIRQ(EXTI7_0_IRQn);
        // ATARI モード: GPIO 直接出力
        atari_update_gpio();
    }
}

uint8_t joystick_get_mode(void) {
    return pad_mode;
}

void joystick_set_button_by_note(uint8_t note, uint8_t pressed) {
    int idx = note_to_btn(note);
    if (idx < 0) return;
    btn_state[idx] = pressed ? 1 : 0;

    // ATARI モードでは即座に GPIO 更新
    if (pad_mode == PAD_MODE_ATARI) {
        atari_update_gpio();
    }
    // MD6 モードでは割り込みハンドラが TH エッジ時に出力するので、
    // ここでは btn_state を更新するだけで良い
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
