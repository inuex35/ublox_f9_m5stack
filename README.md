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

- M5Stack
- u-blox GNSS module
- SD card / SDカード

## Setup / 設定

1. Connect the u-blox GNSS module to the M5Stack's Grove Port A with a Qwiic-to-Grove cable (I2C, address 0x42, 400 kHz). / u-blox GNSSモジュールを Qwiic-Grove ケーブルで M5Stack の Grove ポート A に接続します (I2C、アドレス 0x42、400 kHz)。
2. Configure the receiver once (over its USB): `python tools/f9r_config.py COMx apply-i2c` enables on I2C the raw GNSS (RXM-RAWX, RXM-SFRBX), sensor (ESF-RAW, ESF-MEAS, ESF-STATUS, ESF-INS), solution (NAV-PVT, NAV-PVAT) and NMEA GGA messages and RTCM3 input, saved to flash. `backup FILE.json` saves the current settings first. Needs `pip install pyubx2 pyserial`. / 受信機を一度だけ設定します (受信機の USB から)。`python tools/f9r_config.py COMx apply-i2c` で、I2C に生データ・センサ・測位解・GGA を出し、RTCM3 を受け付ける設定をフラッシュに保存します。`backup FILE.json` で事前に今の設定を保存できます。
3. Create a `config.txt` file in the root directory of the SD card. / SDカードのルートディレクトリに `config.txt` ファイルを作成します。
4. In the `config.txt` file, list the WiFi networks and reference stations in the following format: / `config.txt` ファイルに、WiFiネットワークと参照局の情報を以下の形式で記述します：

config.txt:

`WIFI:SSID,PASSWORD`

`STATION:NAME,SERVER_ADDRESS,PORT,MOUNTPOINT,USERNAME,PASSWORD`

USERNAME and PASSWORD may be left empty for open casters (`STATION:NAME,SERVER,2101,MOUNT,,`). / 認証のないキャスターではユーザー名とパスワードを空にできます。

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
- The receiver is read over I2C (Qwiic / Grove Port A). At 400 kHz the link carries roughly 20–30 KB/s; the message set above is about 15 KB/s at the receiver's 2 Hz rate. If the log shows gaps after raising the rate, reduce messages or use UART at 921600 bps instead. / 受信機とは I2C (Qwiic / Grove ポート A) でやり取りします。400 kHz で毎秒 20〜30 KB 程度で、上の設定は 2 Hz で毎秒約 15 KB です。測位レートを上げてデータが欠ける場合は、メッセージを減らすか UART (921600 bps) を使ってください。

## License / ライセンス

This code is released under the MIT License. See the `LICENSE` file for details. / このコードはMITライセンスの下で公開されています。詳細は LICENSE ファイルをご確認ください。
