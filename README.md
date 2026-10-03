<img src="icon.png" width="64" align="left" alt="">

# R One-Seg

Haiku OS 用の ISDB-T ワンセグ受信アプリです。日本国内向け Sony VAIO P (VGN-P70H) に内蔵されたチューナーモジュールを対象にしています。日本やブラジルなど、ISDB-T を採用している国でのみ使用できます。

[日本語](README.md) · [한국어](README.ko.md) · [English](README.en.md) · [Português (Brasil)](README.pt-BR.md)

![VAIO P の R One-Seg で実放送を受信・再生している画面](captures/vaio-oneseg-2026-10-03.png)

## VAIO 内での受信と再生

受信・USB リンクの復号・映像と音声の再生はすべて VAIO 内で処理します。
起動時の受信機準備が終わってからアンテナを接続してスキャンし、チャンネルを
クリックすると再生します。音量スライダーと全画面ボタンは画面下部にあります。

## インストール

VAIO P の 32 ビット Haiku では、次のコマンドでまとめてインストールできます:

```sh
curl -fsSL https://pkgman.rainygirl.com/install-all.sh | sh -s -- roneseg_x86
```

ソースからインストールする場合:

```sh
git clone https://github.com/rainygirl/haiku-roneseg.git
cd haiku-roneseg
./install.sh
```

必要なパッケージと受信データを自動でインストールします。完了後、Deskbar の
**R One-Seg** を起動してください。

## キー操作

| | |
|---|---|
| Up / Down | チャンネル選択の移動 (再選局はしない) |
| Enter | 選択したチャンネルを選局 |
| Command-F | 拡大表示の切り替え |
| Command-Shift-F | 全画面表示の切り替え |
| Esc | 全画面表示から戻る |
| Command-U | USB デバイスの報告 |
| Command-. | 停止 |
