<img src="icon.png" width="64" align="left" alt="">

# R One-Seg

Haiku OS 用の ISDB-T ワンセグ受信アプリです。日本国内向け Sony VAIO P (VGN-P70H) に内蔵されたチューナーモジュールを対象にしています。

[한국어](README.ko.md)

## ビルド

実機上で Haiku SDK を使って:

```
./install.sh
```

対象となる 32 ビットイメージは gcc2 ハイブリッドで、既定の `g++` は GCC 2.95 の
ためこのコードをビルドできません。`install.sh` はそれを検出すると
`setarch x86` でモダンなセカンダリコンパイラに切り替えます。手動なら
`setarch x86 make` です。

1.33 GHz シングルスレッドの Atom なので、フルビルドは時間がかかり本体が熱く
なります。

## キー操作

| | |
|---|---|
| Up / Down | チャンネル選択の移動 (再選局はしない) |
| Enter | 選択したチャンネルを選局 |
| Command-F | 拡大表示の切り替え |
| Command-U | USB デバイスの報告 |
| Command-. | 停止 |

