<img src="icon.png" width="64" align="left" alt="">

# R One-Seg

ISDB-T One-Seg receiver para sa Haiku OS, na ginawa para sa tuner na nakapaloob
sa Japanese Sony VAIO P (VGN-P70H). Magagamit lamang ito sa mga bansang
gumagamit ng ISDB-T, gaya ng Japan at Brazil.

Sa mga OneSeg broadcast sa Japan lamang isinagawa ang mga pagsubok sa pagtanggap ng signal.

[日本語](README.md) · [한국어](README.ko.md) · [English](README.en.md) · [Português (Brasil)](README.pt-BR.md) · [Filipino](README.fil.md)

![Live na palabas sa R One-Seg sa isang VAIO P](captures/vaio-oneseg-2026-10-03.png)

## Pagtanggap ng signal at pag-play sa VAIO

Sa VAIO mismo ginagawa ang pagtanggap ng signal, pag-decrypt ng USB link,
at pag-play ng video at audio. Pagkabukas ng app, hintaying matapos ang
paghahanda ng receiver, ikonekta ang antena, at mag-scan ng mga channel.
I-click ang isang channel sa listahan upang manood. Nasa ibaba ng window
ang kontrol ng volume at ang button para sa fullscreen.

## Pag-install

Sa VAIO P na may 32-bit Haiku, i-install ang package gamit ang isang command:

```sh
curl -fsSL https://pkgman.rainygirl.com/install-all.sh | sh -s -- roneseg_x86
```

Upang mag-install mula sa source code:

```sh
git clone https://github.com/rainygirl/haiku-roneseg.git
cd haiku-roneseg
./install.sh
```

Awtomatikong ini-install ang mga kinakailangang package at data ng receiver.
Pagkatapos, buksan ang **R One-Seg** mula sa Deskbar.

## Mga keyboard shortcut

| Key | Gawain |
|---|---|
| Up / Down | Pumili ng channel nang hindi pa tumutune |
| Enter | I-tune ang napiling channel |
| Command-F | Baguhin ang pag-scale ng video |
| Command-Shift-F | I-on o i-off ang fullscreen |
| Esc | Lumabas sa fullscreen |
| Command-U | Ipakita ang ulat tungkol sa mga USB device |
| Command-. | Ihinto ang pag-play |

## Paggamit ng AI

Binuo ang programang ito sa tulong ng Claude Code at Codex.
