# ESP32 Smart Remote

Seeed Studio XIAO ESP32S3で作るスマートリモコンのPlatformIOプロジェクトです。赤外線LEDで家電に信号を送信し、赤外線受信モジュールでリモコン信号を学習できます。

現在のv1要件は [docs/requirements.md](docs/requirements.md) にまとめています。

## 機能

- Seeed Studio XIAO ESP32S3向けArduino firmware
- HomeSpanによるWi-Fi設定とHomeKit連携
- IR送信: 学習済みRAW信号
- IR受信: 既存リモコンの「点灯」「常夜灯」信号を学習
- HomeKitのONで「点灯」、OFFで「常夜灯」を送信
- v1では独自Web UIなし

## 想定部品

- Seeed Studio XIAO ESP32S3
- 赤外線LED
- NPNトランジスタまたはMOSFET
- 赤外線受信モジュール 38kHz品
- 抵抗: LED電流制限用、ベース/ゲート用

## 初期ピン

| 用途 | 初期ピン |
| --- | --- |
| IR送信 | D1 |
| IR受信 | D2 |
| 設定/リセットボタン | D3 |
| ステータスLED | 未使用 |

ピンは `include/config.h` で変更できます。

## セットアップ

1. PlatformIOをインストールします。
2. ファームウェアをビルドします。

```sh
pio run
```

3. ESP32へ書き込みます。

```sh
pio run --target upload
```

4. シリアルモニタを開きます。

```sh
pio device monitor
```

5. HomeSpan CLIまたは起動する設定APでWi-Fiを設定します。

初期AP:

- SSID: `SmartRemote-Setup`
- Password: `homespan`

6. Apple HomeアプリでHomeSpanのペアリングコードを使って追加します。

ペアリングコードやQR情報はシリアルモニタに表示されます。必要に応じてHomeSpan CLIの `S` コマンドでセットアップコードを設定してください。

## HomeKit操作

HomeKit上では1つのLightbulbアクセサリとして表示されます。

- ON: 学習済みの `light_on`、「点灯」信号を送信
- OFF: 学習済みの `night_light`、「常夜灯」信号を送信

未学習の状態でON/OFFすると、シリアルログに未学習であることを出力し、操作は失敗として扱います。

## HomeSpan CLI

シリアルモニタからHomeSpan CLIを使えます。`?` を入力するとHomeSpanのコマンド一覧が表示されます。

このプロジェクト固有のIR学習コマンド:

- `o`: 「点灯」信号を学習
- `n`: 「常夜灯」信号を学習
- `q`: 学習済み信号の保存状態を表示
- `k`: 学習中の操作をキャンセル
- `y`: 学習済みIR信号を削除

## ボタン操作

- D3短押し: 未学習のIR信号を順に学習
- D3を約7秒長押し: 学習済みIR信号を削除
- D3をさらに長押し: HomeSpanのコントロールボタンとして、HomeSpan標準のコマンドモード/Factory Resetに入る

## リセット

学習済みIR信号だけを消す場合:

- HomeSpan CLIで `y` を入力する
- またはD3を約7秒長押しする

Wi-Fi設定やHomeKitペアリング情報を消す場合:

- HomeSpan CLIの `X` や `F` など、HomeSpan標準コマンドを使う
- またはD3をHomeSpanのFactory Reset時間まで長押しする

## 配線メモ

IR LEDはESP32のGPIOから直接大電流を流さず、トランジスタまたはMOSFETで駆動してください。赤外線受信モジュールの出力は `IR_RECV_PIN` に接続し、電源電圧は使用するモジュールの仕様に合わせます。
