# 98TAB5 1.3.0 ファームウェア

対象: M5Stack Tab5 / ESP32-P4。ESP-IDF 5.5.0 でビルドしています。

## 変更点

- テキスト画面の縦線を修正
- 再起動を繰り返すソフトを修正
- CPU 高速時のキー入力の遅れを修正
- ヘッドホンが認識されない問題を修正
- スクリーンキーボードを追加
- ステートセーブを追加
- タッチマウスの速度設定を追加
- Orchestra のサンプルを変更

## 新規書き込み（M5Burner 向けの結合版）

`98TAB5-public-1.3.0-20261114.bin` を **0x0** に書き込みます。全体を書き込むため、設定が失われる場合があります。

```sh
python -m esptool --chip esp32p4 --port COM15 --baud 460800 write_flash 0x0 98TAB5-public-1.3.0-20261114.bin
```

## 設定を維持する更新

`pc98tab5-app.bin` を **0x10000** に書き込みます。

```sh
python -m esptool --chip esp32p4 --port COM15 --baud 460800 write_flash 0x10000 pc98tab5-app.bin
```

COM 番号は実機に合わせてください。
