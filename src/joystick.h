#ifndef _JOYSTICK_H
#define _JOYSTICK_H

#include <stdint.h>

// パッドモード
#define PAD_MODE_ATARI  0
#define PAD_MODE_MD6    1

// MIDI Note 番号 → ボタンインデックス
#define BTN_UP      0
#define BTN_DOWN    1
#define BTN_LEFT    2
#define BTN_RIGHT   3
#define BTN_A       4
#define BTN_B       5
#define BTN_C       6
#define BTN_START   7
#define BTN_X       8
#define BTN_Y       9
#define BTN_Z       10
#define BTN_MODE    11
#define BTN_COUNT   12

// GPIO ピン割り当て (ATARI D-SUB 9pin 配線済み)
// PA0: TH/SELECT (入力: コンソールからの SELECT 信号)
// PA1: D0 (Up)
// PA2: D1 (Down)
// PA3: D2 (Left)
// PA4: D3 (Right)
// PA6: D4 (TL / Button A or B)
// PA7: D5 (TR / Button B or C)
#define PIN_TH      0
#define PIN_D0      1
#define PIN_D1      2
#define PIN_D2      3
#define PIN_D3      4
#define PIN_D4      6
#define PIN_D5      7

// 初期化
void joystick_init(void);

// パッドモード設定
void joystick_set_mode(uint8_t mode);
uint8_t joystick_get_mode(void);

// ボタン状態設定 (MIDI Note 番号で指定)
// note: MIDI Note 番号, pressed: 1=押下, 0=解放
void joystick_set_button_by_note(uint8_t note, uint8_t pressed);

// 全ボタン解放
void joystick_release_all(void);

// ATARI モード: GPIO を直接更新 (メインループから呼ぶ)
void joystick_poll(void);

#endif
