<img src="icon.png" width="64" align="left" alt="">

# 受信データ

受信に必要なデータは `data/` に同梱し、ソースからの `./install.sh` と
配布パッケージの両方で自動的に設置します。

| ファイル | 用途 |
|---|---|
| `oneseg_fw.rec` | USB コントローラーのファームウェア |
| `oneseg_demod.bin` | 復調器の DSP プログラム |
| `link-auth.bin` | チューナーとの認証設定 |

パッケージ内の設置先は `data/roneseg/` です。アプリは自身の設置場所に対応する
データを優先し、以前の `~/config/settings/roneseg/` は互換用に参照します。

`packaging/check_payload.py` はファームウェアの SHA-256、認証設定の形式、
ネイティブ AES の既知ベクトルを確認します。パッケージ作成時に自動実行します。

抽出・逆アセンブルなどの解析用ツールはローカルの `recovery/` に保管し、
通常のインストールには使用しません。
