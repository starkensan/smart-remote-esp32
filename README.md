# ESP32 Smart Remote

Seeed Studio XIAO ESP32S3で作るスマートリモコンのPlatformIOプロジェクトです。赤外線LEDで家電に信号を送信し、赤外線受信モジュールでリモコン信号を学習できます。

このREADMEではセットアップ方法と使用方法を説明します。内部構成は
[docs/architecture.md](docs/architecture.md)、v1要件は
[docs/requirements.md](docs/requirements.md) を参照してください。

## 機能

- Seeed Studio XIAO ESP32S3向けArduino firmware
- HomeSpanによるWi-Fi設定とHomeKit連携
- IR送信: 学習済みRAW信号
- IR受信: 既存リモコンの「点灯」「常夜灯」信号を学習
- HomeKitのONで「点灯」、OFFで「常夜灯」を送信
- HomeKitからSHARP製エアコンの電源、冷暖房、除湿、温度、風量、風向を操作
- LAN内HTTP APIからエアコン、照明を操作し、状態をJSONで取得
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
| ステータスLED | `include/config.h` で指定。不要なら `-1` |

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

ポートを自動検出できない場合は、接続されているポートを確認します。

```sh
pio device list
```

ポートが分かっている場合は明示して書き込みます。

```sh
pio run --target upload --upload-port /dev/ttyACM0
```

XIAO ESP32S3が見つからない場合は、USBケーブルを確認し、必要に応じてBOOTを押しながらRESETしてブートローダーモードに入れてから再実行します。WSLや仮想環境を使っている場合は、USBデバイスがLinux側へ渡っていることも確認してください。

4. シリアルモニタを開きます。

```sh
pio device monitor
```

ポートを明示する場合:

```sh
pio device monitor --port /dev/ttyACM0
```

5. HomeSpan CLIまたは起動する設定APでWi-Fiを設定します。

初期AP:

- SSID: `SmartRemote-Setup`
- Password: デバイスごとに初回起動時に生成

6. Apple HomeアプリでHomeSpanのペアリングコードを使って追加します。

設定APのパスワード、HomeKitペアリングコード、APIトークンはESP32のNVSにだけ保存されます。シリアルモニタからHomeSpan CLIの `@z` を入力すると確認できます。これらの値をログ、Issue、スクリーンショットへ掲載しないでください。

## HomeKit操作

HomeKit上では照明用のLightbulbサービスと、エアコン送信用のSwitchサービスが表示されます。

- ON: 学習済みの `light_on`、「点灯」信号を送信
- OFF: 学習済みの `night_light`、「常夜灯」信号を送信
- Air Conditioner: 電源、冷房/暖房、設定温度、風量を操作
- Air Conditioner Dry: 除湿運転をON/OFF
- Air Conditioner Direction: 風向自動、固定1～5、全方向スイングを操作

未学習の状態でON/OFFすると、シリアルログに未学習であることを出力し、操作は失敗として扱います。

風量は自動と1～4、風向は自動、固定1～5、全方向スイングに対応します。

室温センサーは搭載していないため、HomeKitの現在温度には選択中の設定温度を表示します。エアコン状態は最後に送信した値を表示する推定状態であり、実機からの状態フィードバックではありません。

## HTTP API

Wi-Fi接続後、ポート`8080`でHTTP APIを起動します。ベースURLは `http://<DEVICE_IP>:8080` です。すべてのリクエストに、シリアルCLIの `@z` で確認したAPIトークンをBearer認証として付けます。

| メソッド | パス | 動作 |
| --- | --- | --- |
| `GET` | `/api/status` | エアコン、照明、HomeSpan、ネットワーク状態を取得 |
| `GET` | `/api/ac` | 状態全体を取得 |
| `GET` | `/api/light` | 状態全体を取得 |
| `POST` | `/api/ac/off` | エアコンを停止 |
| `POST` | `/api/ac/cool` | 冷房を開始 |
| `POST` | `/api/ac/heat` | 暖房を開始 |
| `POST` | `/api/light/on` | 照明の点灯信号を送信 |
| `POST` | `/api/light/off` | 照明の常夜灯信号を送信 |

冷房・暖房は、任意で`temperature=17..32`と`fan=0..4`をクエリ指定できます。`fan=0`は自動です。

```sh
curl -H "Authorization: Bearer <API_TOKEN>" http://<DEVICE_IP>:8080/api/status
curl -X POST -H "Authorization: Bearer <API_TOKEN>" "http://<DEVICE_IP>:8080/api/ac/cool?temperature=25&fan=0"
curl -X POST -H "Authorization: Bearer <API_TOKEN>" "http://<DEVICE_IP>:8080/api/ac/heat?temperature=24&fan=2"
curl -X POST -H "Authorization: Bearer <API_TOKEN>" http://<DEVICE_IP>:8080/api/ac/off
curl -X POST -H "Authorization: Bearer <API_TOKEN>" http://<DEVICE_IP>:8080/api/light/on
curl -X POST -H "Authorization: Bearer <API_TOKEN>" http://<DEVICE_IP>:8080/api/light/off
```

APIとHomeSpanは同じ状態を共有します。API操作後はHomeアプリの表示も更新され、Homeアプリからの操作も`GET /api/status`へ反映されます。APIはBearer認証必須ですがHTTP通信自体は暗号化されないため、信頼できるローカルネットワーク内だけで使用してください。ブラウザ向けのCORSは有効にしていません。

## HomeSpan CLI

シリアルモニタからHomeSpan CLIを使えます。`?` を入力するとHomeSpanのコマンド一覧が表示されます。

このプロジェクト固有のIR学習コマンド:

- `@o`: 「点灯」信号を学習
- `@n`: 「常夜灯」信号を学習
- `@a`: 現在保存されているSHARP_AC状態を送信
- `@q`: 学習済み信号の保存状態を表示
- `@k`: 学習中の操作をキャンセル
- `@y`: 学習済みIR信号を削除
- `@z`: 設定APパスワード、HomeKitペアリングコード、APIトークンを表示

## ボタン操作

- D3短押し: 未学習のIR信号を順に学習
- D3を約7秒長押し: HomeKitペアリング情報だけを削除して再起動

D3長押しでは、Wi-Fi設定、学習済みIR信号、エアコン設定は削除しません。

## リセット

学習済みIR信号だけを消す場合:

- HomeSpan CLIで `@y` を入力する

HomeKitペアリング情報だけを消す場合:

- D3を約7秒長押しする
- またはHomeSpan CLIで `U` を入力する

Wi-Fi設定も消す場合はHomeSpan CLIの `X` や `F` などを使います。

## 配線メモ

IR LEDはESP32のGPIOから直接大電流を流さず、トランジスタまたはMOSFETで駆動してください。赤外線受信モジュールの出力は `IR_RECV_PIN` に接続し、電源電圧は使用するモジュールの仕様に合わせます。
