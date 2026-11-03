# 98TAB5 — ライセンスと第三者著作物の表示

98TAB5 は **M5Stack Tab5**（ESP32-P4）用の PC-9801 エミュレータです。

- 作者: **mochimochi-man**
- 連絡先: **X [@MochimochimanEX](https://x.com/MochimochimanEX)**

**本書は `LICENSE.md`（英語）の参考訳です。両者に相違がある場合は英語版が優先します。**

本書で言及するライセンスの全文は [`licenses/`](licenses/) に収録しています。

---

## 1. 要約

| | |
|---|---|
| 本プロジェクトが書いたコード | **MIT** — §2 |
| それ以外 | 各々のライセンス。**すべて寛容型（permissive）** — §4 |
| コピーレフト（GPL/LGPL）の混入 | **無し**。イメージにもこのアーカイブにも — §5 |
| NEC の PC-9801 ROM / BIOS / フォントデータ | **無し** — §3 |

ファームウェアイメージは結合著作物です。作者自身のコードは MIT ですが、それ以外は
各著作者のライセンスのままです。イメージやアーカイブを再配布するときは、それら全部を
同時に満たす必要があります。すべて寛容型なので負担は軽いものの、**このプロジェクト
全体が単に「MIT」なのではありません。** 実務上必要なことは §6 にまとめました。

---

## 2. 本プロジェクトが書いたコード

**MIT License / Copyright (c) 2026 mochimochi-man**（全文は英語版 §2 および
`LICENSE.md` を参照）

**対象**: `main/` と `rom_src/` の全体、ルートの `CMakeLists.txt`、
`sdkconfig.defaults`、`partitions.csv`、ビルドスクリプト。すなわち Tab5 移植の
本体 — MIPI-DSI パネルの立ち上げ、PPA によるブリット、メニュー、タッチをマウスに
する処理、USB マスストレージの各モード、BLE HID ホスト、
スクリーンショット出力、RTC 対応です。

あわせて `components/pc98core/`（エミュレートする機械の全体: CPU の `cpu/`、デバイスの
`dev/`、表示の `video/`、音源の `sound/`、ディスク媒体の `media/`）も対象です。

**対象外 — `main/` の中の 2 ファイル:**

- `main/st7123_init_data.h`
- `main/ili9881c_init_data.h`

これらは M5Stack Tab5 BSP のパネル初期化データを**逐語複製**したもので、
**Apache-2.0** のままです（§4.4）。各ファイルの冒頭に出典を明記しています。

---

## 3. 意図的に含めていないもの

- **NEC の PC-9801 BIOS / ITF / CGROM データは一切含みません。**
  - イメージに内蔵した BIOS（`main/rom/PC98N.ROM`）は独立実装で、本アーカイブの
    `rom_src/pc98_bios_native/pc98n.asm` からバイト単位で再現できます。NEC のコード
    は含みません。他社 BIOS のコードも含みません — EPSON 実機の BIOS.ROM と照合して、
    16 バイト以上一致する箇所は 1 か所もなく、他社の著作権表示も持っていません。
  - 内蔵フォント（`main/rom/FONT_ESP.ROM`）はパブリックドメインの東雲ビットマップ
    フォントから `rom_src/mkfont_esp.py` で生成したものです（§4.5）。実機の
    キャラクタジェネレータのダンプではありません。
  - 両方を内蔵しているのは、**空の SD カードでも起動できるようにするため**です。
    メニューから SD 上の実機吸い出し ROM を指定すればそちらが優先されますが、
    その入手は利用者の責任です。
- **エミュレート対象機のディスクイメージ・ゲーム・その他ソフトウェアは含みません。**

---

## 4. 第三者著作物

### 4.1 ESP-IDF v5.5 — Espressif Systems

**Apache-2.0。** ESP-IDF に `NOTICE` ファイルはありません。同ライセンス §4(a) が
再配布者に求めるライセンス全文の頒布のため、`licenses/Apache-2.0.txt` を同梱して
います。

ESP-IDF の中で独自条項を持ち、かつ本イメージに入るもの:

- **FatFs** — Copyright (C) 2022, ChaN。条件は 1 つだけで、
  **ソース再配布時に著作権表示を保持すること**。`licenses/FatFs.txt`
- **TinyUSB** — **MIT**, Copyright (c) Ha Thach (tinyusb.org)
- **miniz** — ESP32-P4 のブート ROM に入っており、スクリーンショット出力で使用。
  上流 miniz は **MIT**（Copyright 2013-2014 RAD Game Tools and Valve Software /
  Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC）。
  宣言に使う ESP-IDF の `miniz.h` は Apache-2.0（Espressif）
- **Bluedroid** — ESP-IDF 同梱のもの、Apache-2.0
- **Newlib** および RISC-V ツールチェーンのランタイム — BSD 系

### 4.2 Espressif マネージドコンポーネント

ビルド時に ESP Component Registry から取得されます。版は `dependencies.lock` で
固定。**本アーカイブには含まれません**（ビルド時にダウンロードされます）。

**Apache-2.0**: `bmi270`, `cmake_utilities`, `eppp_link`, `esp_cam_sensor`,
`esp_codec_dev`, `esp_h264`, `esp_hosted`, `esp_io_expander`,
`esp_io_expander_pi4ioe5v6408`, `esp_lcd_ili9881c`, `esp_lcd_st7121`,
`esp_lcd_st7123`, `esp_lcd_touch`, `esp_lcd_touch_gt911`,
`esp_lcd_touch_st7123`, `esp_lvgl_port`, `esp_sccb_intf`,
`esp_serial_slave_link`, `esp_wifi_remote`, `i2c_bus`, `m5stack_tab5`,
`sensor_hub`, `usb`, `usb_host_hid`, `usb_host_uvc`,
`wifi_remote_over_eppp`

**MIT**: `tinyusb`, `esp_ipa`, `esp_video`

### 4.3 LVGL

**MIT**, Copyright (c) 2025 LVGL Kft。本プロジェクトが直接使うのではなく、
Tab5 BSP 経由で入ります。同梱の VGLite カーネルドライバ（Vivante Corporation,
2014-2022）は MIT/GPL-2.0 のデュアルライセンスで、ここでは **MIT** を選択します。

### 4.4 M5Stack Tab5 BSP — Espressif Systems

**Apache-2.0。** リンクされるほか、2 ファイルを**逐語複製**しており、出典表示を
保持する必要があります。

| 本プロジェクト内 | 複製元 |
|---|---|
| `main/st7123_init_data.h` | `espressif/m5stack_tab5` → `priv_include/disp_init_data_1.h` |
| `main/ili9881c_init_data.h` | `espressif/m5stack_tab5` → `priv_include/disp_init_data.h` |

### 4.5 東雲フォント — パブリックドメイン

内蔵 `FONT_ESP.ROM` の生成に `rom_src/mkfont_esp.py` が使用します。

- 作者: Yasuyuki Furukawa / 2001-2004 /efont/ Project
- 上流: <http://openlab.ring.gr.jp/efont/shinonome/>
- ライセンス: **パブリックドメイン。** 配布物には「すべてのフォントデータ、
  ドキュメント、スクリプト類はすべて Public Domain で提供」、ただし
  「日本に於いては現時点で著作権を放棄することは法律上不可能」であるため
  「AUTHORS に列挙されている作者がその権利を行使しないと宣言することで実質的な
  Public Domain とする」と明記されています。自由な改造・フォーマット変換・組込み・
  再配布が可能で、完全に無保証です。`licenses/Shinonome.txt`

同フォントの Debian パッケージング部分は GPL-2+ ですが、それはパッケージング
スクリプトのみに掛かるものです。本プロジェクトでは一切使用も配布もしていません。

### 4.6 VSCO 2 Community Edition — CC0-1.0

サウンドエフェクト Orchestra が鳴らす楽器録音
`components/pc98core/sound/samples/orchestra_bank.bin` は、VSCO 2 Community
Edition の一部の録音を加工したものです。

- 録音: Sam Gossner / Simon Dalzell、元のサンプル編集: Elan Hickler / Soundemote
- 上流: <https://github.com/sgossner/VSCO-2-CE>
- ライセンス: **CC0-1.0**（パブリックドメイン提供）。改変・再配布・商用利用に
  許諾も表示も不要です。本文は `components/pc98core/sound/samples/LICENSE`、
  固定リビジョン・各ファイルの SHA-256・加工内容は同じ場所の `SOURCES.json` にあります。

## 5. コピーレフト

コピーレフトのコードは、**ファームウェアにも、このアーカイブにも一切ありません。**
本アーカイブに対する `grep -ril "GNU General Public"` は何も返しません。

---

## 6. 再配布するとき

関係するライセンスはすべて寛容型なので、必要なことは多くありません。

1. **本書と `licenses/` を一緒に渡してください。**
2. **Apache-2.0 のライセンス全文を同梱してください。** ESP-IDF・Tab5 BSP・
   Espressif コンポーネントを含むバイナリ、つまり本プロジェクトのあらゆるビルドが
   対象です。`licenses/Apache-2.0.txt` がそのために置いてあります。
3. **著作権表示を保持してください。** §2 に挙げた複製 2 ファイルを含みます。
4. Apache-2.0 の成果物を改変した場合は**変更した旨を明示**してください。

**独自の追加部分を公開する義務はありません。** ソース公開を強制する条項は
どこにもありません。

---

## 7. 商標

PC-9801、PC-98、NEC は日本電気株式会社の商標です。M5Stack、Tab5 は
M5Stack Technology Co., Ltd. の商標です。ESP32、ESP32-P4、ESP-IDF は
Espressif Systems の商標です。Windows は Microsoft Corporation の商標です。
本書ではいずれも、このソフトウェアが何の上で動き何をエミュレートするかを
述べるためだけに使用しています。本プロジェクトはこれらのいずれとも提携・
承認・後援の関係にありません。

## 8. 無保証

上記のすべてのライセンスが一切の保証を否認しており、本プロジェクトも同様です。
これはハードウェアを直接制御する趣味のエミュレータです。**自己責任でご利用ください。**
