# MimicX-firmware

**製品名: Mimic X**

スマートフォンからUSB-MIDI経由で受信した指令をもとに、レトロPCのHIDデバイス（キーボード・マウス・ジョイスティック）を模倣するマイコンファームウェア。

「Mimic X」は様々な HID デバイスに変身する (Mimic = 模倣する) ことから命名。

## 対応デバイス

- ATARI仕様ジョイスティック
- メガドライブ 6 ボタンファイティングパッド
- X68000キーボード
- （今後追加予定）

## 関連リポジトリ

- [MimicX-protocol](https://github.com/kunichiko/MimicX-protocol) - MIDI通信プロトコルライブラリ
- [MimicX-app](https://github.com/kunichiko/MimicX-app) - Flutterアプリ
- [MimicX-hardware](https://github.com/kunichiko/MimicX-hardware) - 基板設計データ

## ファームウェア更新

CH32X035 のファームウェアは Web ブラウザ (Chrome / Edge) から更新できます:

<https://kunichiko.github.io/MimicX-firmware/firmware/>

詳細: [docs/firmware/README.md](docs/firmware/README.md)
