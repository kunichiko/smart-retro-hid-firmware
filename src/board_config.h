// ===================================================================================
// board_config.h
// ===================================================================================
// ビルド時にどのボード (どの HID 機能を有効化するか) を選ぶか定義する。
// platformio.ini の env 内で `-D BOARD_<NAME>` を指定する。
//
// BOARD_FUNCTIONS マクロは、有効な hid_function_t* の配列初期化子を定義する。
// 同時に複数機能を有効にすることも可能 (各機能を異なる MIDI チャンネルに割り当てる)。
//
// 新しいボード/機能の追加方法:
//   1. functions/<name>/ にモジュールを作成
//   2. extern 宣言を下のブロックに追加
//   3. BOARD_FUNCTIONS マクロを定義
//   4. platformio.ini に env を追加 (BOARD_<NAME> 定義 + src_filter 設定)
// ===================================================================================
#ifndef _BOARD_CONFIG_H
#define _BOARD_CONFIG_H

#include "hid_function.h"

// ---------------------------------------------------------------------------
// BOARD_JOYSTICK: ATARI/MD6 ジョイスティックのみ
// ---------------------------------------------------------------------------
#if defined(BOARD_JOYSTICK)
    extern const hid_function_t joystick_function;
    #define BOARD_NAME "mimic-x-joy"
    #define BOARD_FUNCTIONS { &joystick_function }

// ---------------------------------------------------------------------------
// BOARD_X68K_KEYBOARD: X68000 キーボード単体
// ---------------------------------------------------------------------------
#elif defined(BOARD_X68K_KEYBOARD)
    extern const hid_function_t x68k_keyboard_function;
    #define BOARD_NAME "mimic-x-x68k-kb"
    #define BOARD_FUNCTIONS { &x68k_keyboard_function }

// ---------------------------------------------------------------------------
// BOARD_X68K_FULL: X68000 キーボード + マウス
// ---------------------------------------------------------------------------
#elif defined(BOARD_X68K_FULL)
    extern const hid_function_t x68k_keyboard_function;
    extern const hid_function_t x68k_mouse_function;
    #define BOARD_NAME "mimic-x-x68k"
    #define BOARD_FUNCTIONS { &x68k_keyboard_function, &x68k_mouse_function }

#else
    #error "No BOARD_* macro defined. Set one of BOARD_JOYSTICK, BOARD_X68K_KEYBOARD, BOARD_X68K_FULL via build_flags."
#endif

#endif
