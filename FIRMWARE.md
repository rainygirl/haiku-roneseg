<img src="icon.png" width="64" align="left" alt="">

# ファームウェアの入手

内蔵チューナーモジュール (`054c:0279`) は Cypress EZ-USB で、**電源投入時に
ファームウェアを持っていません**。EEPROM から Sony の VID/PID だけを名乗って
列挙され、コントロール転送には答えますが、ストリーム用のエンドポイントを
一切出しません。

つまり `oneseg_fw.rec` が無い状態では:

- `listusb` には `054c:0279` が見える
- `tools/probe_all.cpp` は「control works, streaming does not」と報告する
- **スキャンは 1 チャンネルも見つけない**

これはハードウェアの故障ではなく、ファームウェア未投入そのものの症状です。
`ROneSeg --list-usb` の `Firmware:` 行で、イメージが所定の場所にあるかどうかを
確認できます。

このリポジトリはイメージを同梱していません (ソニーのものです)。**お手元の実機の
ドライバから、ご自分で生成する**のが前提です。

## 取り出し方 — `install.sh` に任せる

イメージは Windows 用ドライバ `vscd.sys` の `.data` セクションに、固定 22 バイト
のレコード表として埋め込まれています。**手で取り出す必要はありません。**
`vscd.sys` をこのスクリプトの隣 (またはデスクトップ) に置いて:

```
./install.sh
```

抽出・検証・設置まで済ませて、こう表示します:

```
==> extracting firmware from ./vscd.sys
==> firmware: 666 records, 8597 bytes, loading to 0x0000-0x2207, reset vector LJMP 0x1aae
```

ドライバが別の場所にあるなら:

```
./install.sh --driver /path/to/vscd.sys
```

探す場所は、スクリプトのあるディレクトリ、ホーム、`~/Desktop`、`~/Downloads`、
`~/driver`、`~/drivers` の各 3 階層までです。ファイル名は `VSCD.SYS` でも
かまいません。

以降の実行では、すでに置かれているイメージを毎回検証して同じ 1 行を出します。
イメージが壊れていれば設置せずに理由を表示します。

手で実行する場合 (`install.sh` がドライバを見つけられないときなど):

```
python3 recovery/extract_fw.py vscd.sys ~/config/settings/roneseg/oneseg_fw.rec
python3 recovery/extract_fw.py --check ~/config/settings/roneseg/oneseg_fw.rec
```

VGN-P70H のドライバなら `666 records, 8597 bytes` と
`reset vector LJMP 0x1aae` になります。リセットベクタが `LJMP` (先頭バイト
`0x02`) であることが、解析が正しいことの最も安価な確認です。抽出器は、
リセットベクタが無い場合や `0x43` (INT2/USB) のベクタを欠く場合は
**書き出しを拒否します** — そのイメージは「ファームウェアは走るのに EP0 に
一切応答しないモジュール」を作るからです。

`vscd.sys` の入手元:

- 実機のリカバリディスク。中身は `.MOD` パッケージ群で、それぞれ先頭 16 バイトが
  ASCII 文字列 `Sony Corporation` と XOR された WIM イメージです。そこを戻せば
  任意の WIM ツールで開けます
- VGN-P70H の `C:\Windows\Drivers` から取ったドライバアーカイブの
  `INF/sonycxd/` 以下
- 「1seg Tuner Driver」パッケージ (`vscd.sys` / `vscd.inf` / `vscd.cat`)

## レコード形式 — 手作業で探す場合の注意

自力で探すときに一度はまる形です。**長さが先で、ストライドは 22 バイト**です。

```
u16 length    1..16
u16 address   8051 のメモリ上のロード先
u8  valid     0。非ゼロで表の終わり
u8  data[16]  ペイロード (使うのは length バイトだけ)
              ... レコード全体で 22 バイト
```

4 バイトのヘッダだけを頼りに探すと、x86 コードの偶然の一致が大量に釣れます。
「アドレスが 0x1000 単位で並ぶ規則的な表」が見えたなら、それは誤検出です。
本物は上の 3 条件 (valid が 0、length が 1..16、アドレスが 16 ビット空間に収まる)
を 22 バイト刻みで何百回も連続して満たします。

表の開始位置も重要です。ドライバのローダは自身のポインタを `.data+0x15` に置き、
-5 から length、-3 から address を読み、22 バイトずつ進めます。ここから表の先頭は
`.data+0x10` と決まります。**先頭 14 レコードは 8051 の割り込みベクタと
`0x1700` の USB ディスパッチテーブル**なので、開始位置を間違えると「ファームウェアは
走るが EP0 に一切応答しない」モジュールが出来上がり、電源を切るまで戻りません。

`extract_fw.py` は `.data+0x10` で見つからなければファイル全体を走査し、上記の
条件を満たす最長の連続領域を採用します。ドライバのビルドが違って表が移動していても、
セクションが `.data` でなくても、そのまま取り出せます。

## それでも見つからない場合

1. `--image` で平坦なバイナリを書き出し、中身を見る:
   `python3 recovery/extract_fw.py vscd.sys --image oneseg_fw.bin`
2. `recovery/dis8051.py` で逆アセンブルし、`0x0ec5` 付近にベンダコマンドの
   ディスパッチ (`bRequest - 0x20` の範囲チェック) があるか確認する
3. どうしても表が存在しないビルドなら、Windows 実機で USB キャプチャ
   (Wireshark + USBPcap) を取り、`bRequest = 0xA0` のベンダリクエスト列から
   再構成する。`0xA0` の `wValue` がロード先アドレス、データステージが中身です

**他機種の CXD9192 モジュール用に確認済みのイメージがあるなら、それも試す価値が
あります。** モジュール側の I2C の並び (デモジュレータ `0x6E`、フロントエンド
`0x60`/`0x6C`) が同じなら動く見込みがあります。

## 置き場所

`ROneSeg` が探す順に:

```
~/config/settings/roneseg/oneseg_fw.rec      <- 既定。install.sh もここに置く
~/config/non-packaged/data/roneseg/oneseg_fw.rec
~/fwtool/oneseg_fw.rec
./oneseg_fw.rec                              <- 実行時のカレントディレクトリ
```

投入は最初の選局時に自動で行われ、成功するとモジュールは
`CXD9192 Controller` として再列挙されます (`bcdDevice 0x1018`)。

**壊す心配はありません。** ファームウェアは内部 RAM に載るだけで、フラッシュには
書きません。電源を落とせば空のブートローダに戻ります。
