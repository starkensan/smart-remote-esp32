# ESP32 Smart Remote

Seeed Studio XIAO ESP32S3で作るスマートリモコンのPlatformIOプロジェクトです。赤外線LEDで家電に信号を送信し、赤外線受信モジュールでリモコン信号を学習できます。

現在のv1要件は [docs/requirements.md](docs/requirements.md) にまとめています。

## 機能

- Seeed Studio XIAO ESP32S3向けArduino firmware
- IR送信: NEC、SONY、PANASONIC、RAW
- IR受信: 最後に受信した信号をWeb APIで確認
- LittleFS配信の簡易Web UI
- JSON APIによる外部連携

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
2. `include/secrets.example.h` を `include/secrets.h` にコピーし、`WIFI_SSID` と `WIFI_PASSWORD` を書き換えます。
3. ファームウェアをビルドします。

```sh
pio run
```

4. Web UIをLittleFSへ書き込みます。

```sh
pio run --target uploadfs
```

5. ESP32へ書き込みます。

```sh
pio run --target upload
```

6. シリアルモニタでIPアドレスを確認します。

```sh
pio device monitor
```

## API

### 状態確認

```sh
curl http://<esp32-ip>/api/status
```

### NEC信号送信

```sh
curl -X POST http://<esp32-ip>/api/send \
  -H 'Content-Type: application/json' \
  -d '{"protocol":"NEC","value":"0x20DF10EF","bits":32}'
```

### RAW信号送信

```sh
curl -X POST http://<esp32-ip>/api/send \
  -H 'Content-Type: application/json' \
  -d '{"protocol":"RAW","frequency":38,"raw":[9000,4500,560,560]}'
```

## 配線メモ

IR LEDはESP32のGPIOから直接大電流を流さず、トランジスタまたはMOSFETで駆動してください。赤外線受信モジュールの出力は `IR_RECV_PIN` に接続し、電源電圧は使用するモジュールの仕様に合わせます。
