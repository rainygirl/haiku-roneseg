<img src="icon.png" width="64" align="left" alt="">

# R One-Seg

Haiku OS용 ISDB-T 원세그(One-Seg) 수신기. 일본 내수판 Sony VAIO P(VGN-P70H)에
내장된 튜너 모듈을 대상으로 합니다. 일본과 브라질 등 ISDB-T를 사용하는 국가에서만
사용할 수 있습니다.

[日本語](README.md) · [한국어](README.ko.md) · [English](README.en.md) · [Português (Brasil)](README.pt-BR.md)

![VAIO P의 R One-Seg에서 실제 방송을 수신·재생하는 화면](captures/vaio-oneseg-2026-10-03.png)

## VAIO 내부 재생 경로

튜너 수신, USB 링크 복호화, 영상·음성 재생은 모두 VAIO에서 처리합니다.
앱을 열면 수신기를 먼저 준비하고,
준비가 끝난 뒤 안테나를 연결해 스캔합니다. 목록의 채널을 한 번 누르면 재생합니다.
볼륨 슬라이더와 전체화면 버튼은 창 아래에 있습니다.

## 설치

VAIO P의 32비트 Haiku에서는 패키지로 한 번에 설치할 수 있습니다:

```sh
curl -fsSL https://pkgman.rainygirl.com/install-all.sh | sh -s -- roneseg_x86
```

소스에서 설치하려면:

```sh
git clone https://github.com/rainygirl/haiku-roneseg.git
cd haiku-roneseg
./install.sh
```

필요한 패키지와 수신 데이터를 자동으로 설치합니다. 설치 후 Deskbar의
**R One-Seg**를 실행하세요.

## 단축키

| | |
|---|---|
| Up / Down | 채널 선택 이동 (재튜닝하지 않음) |
| Enter | 선택한 채널로 튜닝 |
| Command-F | 확대 전환 |
| Command-Shift-F | 전체화면 전환 |
| Esc | 전체화면에서 창 모드로 복귀 |
| Command-U | USB 장치 보고 |
| Command-. | 정지 |

## AI 활용 고지

이 프로그램은 Claude Code와 Codex를 활용해 개발했습니다.
