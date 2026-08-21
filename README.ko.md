<img src="icon.png" width="64" align="left" alt="">

# R One-Seg

Haiku OS용 ISDB-T 원세그(One-Seg) 수신기. 일본 내수판 Sony VAIO P(VGN-P70H)에
내장된 튜너 모듈을 대상으로 합니다.

[日本語](README.md)

## 이 앱이 할 수 없는 것

**한국 DMB는 수신할 수 없습니다.** 작업을 더 해서 되는 문제도, 드라이버를 바꿔서
되는 문제도 아닙니다. 물리계층이 다른 규격이고, 원세그 모듈은 소프트웨어 라디오가
아니라 고정 기능 복조기입니다.

| | 원세그 (ISDB-T 1seg) | 한국 T-DMB |
|---|---|---|
| 기반 규격 | ISDB-T | Eureka-147 DAB |
| 대역 | UHF 470-710 MHz | VHF Band III 174-216 MHz |
| 채널 폭 | 약 429 kHz (6 MHz의 1/13) | 1.536 MHz |
| 다중화 | MPEG-2 TS | DAB 앙상블, MPEG-4 SL/FlexMux |
| 오디오 | HE-AAC | BSAC |

RF 프론트엔드가 Band III에 닿지 않고, 복조기는 ISDB-T OFDM을 실리콘으로 구현하고
있습니다. 둘 사이를 잇는 펌웨어 경로는 없습니다.

더 단순한 이유가 하나 더 있습니다. 한국에서는 애초에 수신할 것이 없습니다. 한국
지상파는 ATSC 1.0(8VSB)이고 ISDB-T 송출은 존재하지 않습니다. 이 앱은 ISDB-T가
송출되는 곳 — 일본과 남미 대부분 — 에서만 쓸모가 있습니다.

## 펌웨어가 필요합니다

모듈은 전원을 넣은 시점에 펌웨어가 없습니다. `oneseg_fw.rec`가 없으면 USB에는
보이지만 스트리밍 엔드포인트가 나오지 않아 **스캔이 한 채널도 찾지 못합니다**.
이미지는 소니 것이라 동봉하지 않으며, 보유하신 기기의 드라이버에서 직접
생성합니다:

```
python3 recovery/extract_fw.py vscd.sys ~/config/settings/roneseg/oneseg_fw.rec
```

절차와, 추출이 안 될 때의 대처는 **[FIRMWARE.md](FIRMWARE.md)**에 정리해
두었습니다. 이미지가 제자리에 있는지는 `ROneSeg --list-usb`의 `Firmware:` 줄로
확인할 수 있습니다.

## 빌드

필요한 개발 패키지 (gcc2 하이브리드에서는 iconv을 두 아키텍처 모두):

```
pkgman install libiconv_devel libiconv_x86_devel
```

기기에서 Haiku SDK로:

```
./install.sh
```

대상인 32비트 이미지는 gcc2 하이브리드라 기본 `g++`가 GCC 2.95이고 이 코드를
빌드하지 못합니다. `install.sh`가 이를 감지하면 `setarch x86`으로 모던 보조
컴파일러로 전환합니다. 직접 하려면 `setarch x86 make`입니다.

1.33 GHz 단일 스레드 Atom이라 전체 빌드는 오래 걸리고 기기가 뜨거워집니다.

## 단축키

| | |
|---|---|
| Up / Down | 채널 선택 이동 (재튜닝하지 않음) |
| Enter | 선택한 채널로 튜닝 |
| Command-F | 확대 전환 |
| Command-U | USB 장치 보고 |
| Command-. | 정지 |

