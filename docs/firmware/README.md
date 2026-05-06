# Mimic X Firmware Update

CH32X035 のファームウェアを Web ブラウザから書き込むためのページです。

公開 URL: <https://kunichiko.github.io/MimicX-firmware/firmware/>

## 使い方

1. CH32X035 の BOOT ボタンを押しながら USB ケーブルを接続する（または BOOT を押しながら RESET を押して離す）
2. Chromium 系ブラウザ（Chrome, Edge）で [ファームウェア更新ページ](https://kunichiko.github.io/MimicX-firmware/firmware/) を開く
3. 「デバイスに接続」を押し、「QinHeng Electronics 製のデバイス」を選択する
4. 基板に合うバリアント（joystick / x68k_keyboard）とファームウェアバージョンを選択して「書き込み」を押す
5. 書き込み完了後、USB ケーブルを抜き差しして通常モードで起動する

## バリアント

| バリアント | 用途 | ビルド env |
|-----------|------|-----------|
| `joystick` | ATARI / メガドライブ 6 ボタン | `pio run -e joystick` |
| `x68k_keyboard` | X68000 キーボード (キーボード + マウス) | `pio run -e x68k_keyboard` |

## 動作環境

- **ブラウザ**: Chrome, Edge（WebUSB 対応ブラウザが必要。Safari / Firefox は非対応）
- **OS**: Windows, macOS, Linux, ChromeOS, Android

## 新しいファームウェアを追加する手順

1. `pio run -e <env>` で `.pio/build/<env>/firmware.bin` を生成する
2. `docs/firmware/firmwares/mimicx_<env>_v<X.Y.Z>.bin` としてコピーする
3. `docs/firmware/index.html` の `VARIANTS` テーブルに新しいリリースエントリを追加する

## 技術情報

- [WebUSB API](https://developer.mozilla.org/en-US/docs/Web/API/WebUSB_API) を使用して WCH ISP プロトコルで CH32X035 に書き込みます
- `ch32flasher.js` は [chprog](https://github.com/wagiminator/MCU-Flash-Tools)（MIT License）のプロトコル実装を JavaScript で再実装したものです（[MinyasX](https://github.com/kunichiko/X68k-MinyasX/) の実装を流用）
