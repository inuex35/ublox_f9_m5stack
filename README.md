# NTRIP Client for M5Stack with u-blox GNSS Module

This code implements an NTRIP client on the M5Stack, designed to work with a u-blox GNSS module. It connects to an NTRIP server, receives GNSS correction data, and forwards it to the u-blox module. It also has the capability to save the GNSS data received from the u-blox module to an SD card.

このコードは、M5Stack上でNTRIPクライアントを実装し、u-bloxのGNSSモジュールと連携させるためのものです。NTRIPサーバーに接続し、GNSS補正データを受信して、u-bloxモジュールに転送します。また、u-bloxモジュールから受信したGNSSデータをSDカードに保存する機能も備えています。

## Features / 機能

- WiFi network selection and connection / WiFiネットワークの選択と接続
- Reference station selection and connection / 参照局の選択と接続
- Reception of GNSS correction data from NTRIP server / NTRIPサーバーからのGNSS補正データの受信
- Forwarding of received GNSS correction data to u-blox module / 受信したGNSS補正データのu-bloxモジュールへの転送
- Saving GNSS data received from u-blox module to SD card / u-bloxモジュールから受信したGNSSデータのSDカードへの保存
- Downloading the logs over WiFi from a phone or PC: open `http://<M5Stack IP>/` (shown on the screen) / スマホや PC のブラウザで `http://<M5Stack の IP>/` を開いて SD のログをダウンロード (IP は画面に表示)
- Sending the receiver's GGA position to the caster every 10 s (needed by VRS / nearest-base mountpoints) / 受信機の GGA を 10 秒ごとにキャスターへ送信 (VRS などで必要)
- Automatic reconnection when the NTRIP stream or WiFi drops / NTRIP や WiFi が切れたときの自動再接続

## Required Hardware / 必要なハードウェア

- M5Stack Core2 (M5Stack Basic / Gray also build with M5Unified) / M5Stack Core2 (Basic / Gray も M5Unified でビルド可)
- u-blox GNSS receiver (tested with ZED-F9R) / u-blox GNSS 受信機 (ZED-F9R で確認)
- SD card / SDカード
- Optional: CAN transceiver (e.g. M5Stack CAN Unit) for the vehicle speed from OBD-II / 任意: OBD-II の車速用 CAN トランシーバ (M5Stack CAN Unit など)

## Wiring / 配線

| | M5Stack Core2 | Receiver / car |
|---|---|---|
| `GNSS:I2C` (default) | Port A (red), GPIO32 SDA / GPIO33 SCL | Qwiic / I2C (0x42) |
| `GNSS:UART` | Port C (blue), GPIO13 RX / GPIO14 TX | UART1 TX / RX (swap!) |
| `CAN:ON` | Port B (black), GPIO36 RX / GPIO26 TX | CAN transceiver -> OBD pin 6 CAN-H, pin 14 CAN-L, pins 4/5 GND |

**Power:** Grove ports carry 5 V on the red wire, Qwiic expects 3.3 V. Do not connect the red wire of a Grove-to-Qwiic cable to a board whose Qwiic 3.3 V goes straight to the receiver; power the receiver from its own USB / 5 V input and connect only GND / SDA / SCL. / Grove の赤線は 5 V、Qwiic は 3.3 V です。Qwiic の 3.3 V が受信機に直結のボードには、Grove-Qwiic ケーブルの赤線をつながず、受信機は自身の USB / 5 V 入力から給電し、GND / SDA / SCL だけをつないでください。

In the car, OBD pin 16 has +12 V at all times; use an automotive 12 V -> 5 V converter to the M5Stack's USB-C. / 車では OBD の 16 番ピンが常時 +12 V です。車載用の 12 V -> 5 V 変換器から M5Stack の USB-C に給電します。

## Setup / 設定

Create `config.txt` in the root of the SD card. / SD カードのルートに `config.txt` を作ります。

```
WIFI:SSID,PASSWORD
STATION:NAME,SERVER_ADDRESS,PORT,MOUNTPOINT,USERNAME,PASSWORD
GNSS:I2C
RATE_HZ:2
CAN:ON
ENGINE_OFF_S:60
```

| Line | Meaning |
|---|---|
| `WIFI:` | WiFi networks (up to 10). For NTRIP, NTP (file names) and downloads; logging also works without WiFi. / NTRIP・時刻・ダウンロード用。WiFi なしでも記録します |
| `STATION:` | NTRIP casters (up to 9). USERNAME / PASSWORD may be empty. A "(no NTRIP, log only)" entry is always added. / ユーザー名・パスワードは空でも可。「NTRIP なし (記録のみ)」が常に追加されます |
| `GNSS:` | `I2C` (Port A) or `UART` (Port C, 921600 bps; the receiver's baud rate is detected and switched) / 受信機との接続 |
| `RATE_HZ:` | RAWX / navigation rate, 1–10. Default 5 on UART, 2 on I2C. / 測位レート |
| `CAN:` | `ON` (Port B) or `CAN:<rx gpio>,<tx gpio>`: read the speed from OBD-II and feed it to the receiver. / OBD-II の車速を受信機に入力 |
| `ENGINE_OFF_S:` | Seconds without an OBD answer before closing the log and sleeping (default 60). / OBD の応答がないまま何秒で記録を閉じて眠るか |

The receiver is configured by the logger at every start (RAM and battery-backed RAM, not flash): measurement rate; RXM-RAWX, RXM-SFRBX, ESF-MEAS, ESF-STATUS, ESF-INS, NAV-PVT, NAV-PVAT and NMEA GGA on the link port only; all output on the other ports off (the receiver's transmit buffer is shared, and an undrained USB port makes it drop messages); ESF-RAW off (ESF-MEAS has the same IMU data and also the speed input); with `CAN:` the speed input (CFG-SFODO). / 受信機は起動のたびにロガーが設定します (RAM とバックアップ RAM、フラッシュには書きません)。

`tools/f9r_config.py` sets or backs up the receiver over its own USB (`pip install pyubx2 pyserial`). / 受信機の USB から設定・バックアップするスクリプトです。

## Usage / 使い方

1. Power on. The saved WiFi network and station are taken after 10 s unless a button is touched (A up, C down, B select). / 起動します。ボタンに触れなければ、前回の WiFi と基準局が 10 秒で選ばれます (A 上、C 下、B 決定)。
2. The receiver data is logged to `data_YYYYMMDD_HHMMSS.ubx` (or `log_NNNNN.ubx` without NTP time). / 受信機のデータを記録します (時刻がとれないときは連番のファイル名)。
3. Download: open `http://<M5Stack IP>/` on a phone or PC on the same WiFi. / 同じ WiFi のスマホや PC で `http://<IP>/` を開いてダウンロード。
4. Hold B for 2 s to close the file before removing the SD card (hold again for a new file). / SD を抜く前に B を 2 秒長押しでファイルを閉じます (もう一度で新しいファイル)。
5. With `CAN:`: when the car stops answering OBD requests (engine off) the log is closed and the M5Stack sleeps, checking the car every 30 s; it starts logging again by itself when the engine runs. Touching the screen also wakes it. / エンジンを切ると記録を閉じて眠り、30 秒ごとに車を確認し、エンジンがかかると自動で記録を再開します。画面に触れても起きます。
6. When the external 5 V disappears (Core2), the file is closed on the internal battery and the M5Stack powers off. / 外部 5 V が切れると、内蔵バッテリーでファイルを閉じてから電源を切ります (Core2)。

## Notes / 注意事項

- I2C runs at 100 kHz, about 10 KB/s: the receiver stretches the I2C clock and above 100 kHz the ESP32 misreads it. This is enough for 2 Hz; for 5 Hz RAWX (about 15–17 KB/s outdoors) use `GNSS:UART`. The status screen shows damaged frames. / I2C は 100 kHz (毎秒約 10 KB) です。受信機のクロックストレッチを ESP32 が 100 kHz より速いと読み違えるためです。2 Hz なら足りますが、RAWX 5 Hz (屋外で毎秒 15〜17 KB) には `GNSS:UART` を使ってください。画面に壊れたメッセージの数が出ます。
- OBD-II speed has 1 km/h steps. / OBD-II の車速は 1 km/h 刻みです。
- While the M5Stack sleeps the receiver keeps its power (about 0.3–0.5 W together); for long parking, unplug or use a switched supply. / 眠っている間も受信機には電源が来ています (合わせて 0.3〜0.5 W 程度)。長く駐車するときは抜くか、スイッチ付きの電源にしてください。

## License / ライセンス

This code is released under the MIT License. See the `LICENSE` file for details. / このコードはMITライセンスの下で公開されています。詳細は LICENSE ファイルをご確認ください。
