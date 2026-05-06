# MimicX-firmware

**製品名: Mimic X**

スマートフォンからUSB-MIDI経由で受信した指令をもとに、レトロPCのHIDデバイス（キーボード・マウス・ジョイスティック）を模倣するマイコンファームウェア。

「Mimic X」は様々な HID デバイスに変身する (Mimic = 模倣する) ことから命名。

## 対応デバイス

- ATARI仕様ジョイスティック
- メガドライブ 6 ボタンファイティングパッド
- X68000キーボード
- （今後追加予定）

## ビルド方法

[PlatformIO](https://platformio.org/) ベースのプロジェクト。事前に PlatformIO
Core を導入してください (`pip install platformio` / `brew install platformio`
/ `uv tool install platformio` / VS Code の "PlatformIO IDE" 拡張のいずれか)。

`platformio.ini` にボード別の env が定義されています:

| env | 用途 |
|---|---|
| `joystick` | ATARI / MD6 ジョイスティック (atari-joystick 基板) |
| `x68k_keyboard` | X68000 キーボード単体 |
| `x68k_full` | X68000 キーボード + マウス |
| `joystick-debug` | デバッグビルド (-Og + WCH-LinkE GDB サーバ) |

### ビルド

```sh
pio run -e joystick           # ATARI/MD6 ジョイスティック
pio run -e x68k_keyboard      # X68000 キーボード単体
pio run -e x68k_full          # X68000 キーボード + マウス
```

初回は Community-PIO-CH32V プラットフォーム / ch32v003fun フレームワーク /
riscv toolchain が自動取得されます (数分かかります)。成果物は
`.pio/build/<env>/firmware.{bin,hex,elf}` に出力されます。

### 書き込み

**A. WCH-LinkE (SWD 経由)**

```sh
pio run -e joystick -t upload   # platformio.ini の upload_protocol=minichlink
```

**B. USB DFU bootloader 経由**

リセット時に UDP (PC17) を HIGH にすると WCH USB DFU bootloader で起動します
(atari-joystick 基板の **BOOT ボタン**を押しながら USB を挿す)。書き込みは
`wchisp` または下記 WebUSB フラッシャから:

```sh
wchisp flash .pio/build/joystick/firmware.bin
```

### デバッグ

```sh
pio debug -e joystick-debug
```

WCH-LinkE 接続時に minichlink GDB サーバ経由でブレーク・ステップ実行できます。

### クリーンビルド

```sh
pio run -e joystick -t clean   # env 単位
rm -rf .pio                    # 完全クリア (プラットフォーム再取得)
```

## 関連リポジトリ

- [MimicX-protocol](https://github.com/kunichiko/MimicX-protocol) - MIDI通信プロトコルライブラリ
- [MimicX-app](https://github.com/kunichiko/MimicX-app) - Flutterアプリ
- [MimicX-hardware](https://github.com/kunichiko/MimicX-hardware) - 基板設計データ

## ファームウェア更新

CH32X035 のファームウェアは Web ブラウザ (Chrome / Edge) から更新できます:

<https://kunichiko.github.io/MimicX-firmware/firmware/>

詳細: [docs/firmware/README.md](docs/firmware/README.md)
