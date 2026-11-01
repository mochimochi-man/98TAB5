# 公開用ファームウェア

対象: M5Stack Tab5 / ESP32-P4。ESP-IDF 5.5.1、版番号1.0.0（分離元の設定を継承）。
FDCの1バイト転送タイミングと、SD終了時のファイル同期・閉鎖を修正しています。
詳細はプロジェクト直下の `CHANGELOG_FDC.md` を参照してください。

## 新規書き込み

`98TAB5-public-1.0.0-20261101.bin` はブートローダー・パーティション・OTA初期データ・
アプリの結合イメージです。開始位置は **0x0**。COM番号を実機に合わせます。

```sh
python -m esptool --chip esp32p4 --port COM5 --baud 460800 write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 98TAB5-public-1.0.0-20261101.bin
```

## 同じパーティション構成からの更新

既存の保存設定を維持する更新では、アプリ単体を0x10000へ書き込みます。

```sh
python -m esptool --chip esp32p4 --port COM5 --baud 460800 write_flash 0x10000 pc98tab5-app.bin
```

## 検証範囲

- ビルド成功、バイナリのサイズ検査、削除済みバックエンドのELFシンボル監査を通過。
- FDC、通常媒体の読み書き・交換、HDMの再フォーマットと再マウント、SDキャッシュ終了処理の回帰を通過。
- 公開版のFDC変更後、FreeDOSのHDMとMS-DOSのNHDの起動を実機で確認。
- 全ソフトの完全動作は未検証。

実機の詳細記録と画像は開発プロジェクトに保管し、この配布物には含めていません。
`BUILD_INFO.json`、`VALIDATION.json`、`SHA256SUMS.txt` とビルドログを収録しています。
配布時にはプロジェクトの `LICENSE.md`、`LICENSE.jp.md`、`licenses/` を同梱してください。
