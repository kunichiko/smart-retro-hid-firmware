# Mimic X Firmware Update

CH32X035 のファームウェアを Web ブラウザから書き込むためのページです。

公開 URL: <https://kunichiko.github.io/MimicX-firmware/firmware/>

## 使い方

1. Chromium 系ブラウザ（Chrome, Edge）で [ファームウェア更新ページ](https://kunichiko.github.io/MimicX-firmware/firmware/) を**先に**開いておく
2. CH32X035 の BOOT ボタンを押しながら USB ケーブルを接続する
3. **すぐに**「デバイスに接続」を押し、「QinHeng Electronics 製のデバイス」を選択する
4. 基板に合うバリアント（joystick / x68k_keyboard / combined）とファームウェアバージョンを選択して「書き込み」を押す
5. 書き込み完了後、USB ケーブルを抜き差しして通常モードで起動する

> **BOOT モードの 5 秒タイムアウトについて**
>
> CH32X035 内蔵の USB DFU ブートローダーは、ホストから ISP コマンドを受信しないまま約 5 秒経過すると自動的に通常起動へ抜ける仕様です。ブラウザのページを開く前に USB を挿してしまうと、「デバイスに接続」を押す頃には既に通常モードに移行していてデバイスが見つかりません。必ず先にページを開き、USB 接続直後に「デバイスに接続」を押してください。間に合わなかった場合は USB を一度抜き、BOOT ボタンを押しながら挿し直してリトライしてください（CH32X035G8U6 には RESET 端子が出ていないため、RESET ボタンによる復帰はできません）。

> **Windows をお使いの方へ**
>
> CH32X035 を BOOT モードで Chrome / Edge に認識させるには、事前に **WinUSB ドライバ**を手動でインストールしておく必要があります。手順は [WinUSB ドライバのインストール手順](https://kunichiko.ohnaka.jp/products/mimicx/install-winusb) を参照してください。macOS / Linux / ChromeOS / Android では追加のドライバインストールは不要です。

## バリアント

| バリアント | 用途 | ビルド env |
|-----------|------|-----------|
| `joystick` | ATARI / メガドライブ 6 ボタン | `pio run -e joystick` |
| `x68k_keyboard` | X68000 キーボード (キーボード + マウス) | `pio run -e x68k_keyboard` |
| `combined` | 全機能 (joystick + x68k_keyboard + x68k_mouse) を 1 MCU に同居 | `pio run -e combined` |

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
