# NTRIP Client for M5Stack with u-blox GNSS Module

This code implements an NTRIP client on the M5Stack, designed to work with a u-blox GNSS module. It connects to an NTRIP server, receives GNSS correction data, and forwards it to the u-blox module. It also has the capability to save the GNSS data received from the u-blox module to an SD card.

このコードは、M5Stack上でNTRIPクライアントを実装し、u-bloxのGNSSモジュールと連携させるためのものです。NTRIPサーバーに接続し、GNSS補正データを受信して、u-bloxモジュールに転送します。また、u-bloxモジュールから受信したGNSSデータをSDカードに保存する機能も備えています。

## Features / 機能

- WiFi network selection and connection / WiFiネットワークの選択と接続
- Reference station selection and connection / 参照局の選択と接続
- Reception of GNSS correction data from NTRIP server / NTRIPサーバーからのGNSS補正データの受信
- Forwarding of received GNSS correction data to u-blox module / 受信したGNSS補正データのu-bloxモジュールへの転送
- Saving GNSS data received from u-blox module to SD card / u-bloxモジュールから受信したGNSSデータのSDカードへの保存

## Required Hardware / 必要なハードウェア

- M5Stack
- u-blox GNSS module
- SD card / SDカード

## Setup / 設定

1. Connect the u-blox GNSS module to the M5Stack. / u-blox GNSSモジュールをM5Stackに接続します。
2. Create a `config.txt` file in the root directory of the SD card. / SDカードのルートディレクトリに `config.txt` ファイルを作成します。
3. In the `config.txt` file, list the WiFi networks and reference stations in the following format: / `config.txt` ファイルに、WiFiネットワークと参照局の情報を以下の形式で記述します：

config.txt:

`WIFI:SSID,PASSWORD`

`STATION:NAME,SERVER_ADDRESS,PORT,MOUNTPOINT,USERNAME,PASSWORD`

Multiple WiFi networks and reference stations can be registered. / 複数のWiFiネットワークと参照局を登録できます。

## Usage / 使い方

1. Power on the M5Stack. / M5Stackの電源を入れます。
2. Select and connect to a WiFi network. / WiFiネットワークを選択し、接続します。
3. Select and connect to a reference station. / 参照局を選択し、接続します。
4. The received GNSS correction data from the NTRIP server will be forwarded to the u-blox module. / NTRIPサーバーから受信したGNSS補正データが、u-bloxモジュールに転送されます。
5. The GNSS data received from the u-blox module will be saved to the SD card. The file name will be in the format `data_YYYYMMDD_HHMMSS.ubx`. / u-bloxモジュールから受信したGNSSデータがSDカードに保存されます。ファイル名は `data_YYYYMMDD_HHMMSS.ubx` の形式です。

## Notes / 注意事項

- This code is designed to work with a u-blox GNSS module on the M5Stack. Operation on other platforms or with other GNSS modules is not guaranteed. / 本コードはM5Stack上でu-bloxのGNSSモジュールと連携するように設計されています。他のプラットフォームや他のGNSSモジュールでの動作は保証されません。
- Connection to the NTRIP server requires a valid username and password. / NTRIPサーバーへの接続には、適切なユーザー名とパスワードが必要です。
- Proper serial communication settings are required for communication with the u-blox module. This code uses serial port 2, with a baud rate of 230400bps, 8N1. / u-bloxモジュールとの通信には、適切なシリアル通信設定が必要です。このコードでは、シリアルポート2を使用し、ボーレート230400bps、8N1で通信しています。

## License / ライセンス

This code is released under the MIT License. See the `LICENSE` file for details. / このコードはMITライセンスの下で公開されています。詳細は `LICENS
