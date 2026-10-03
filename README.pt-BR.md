<img src="icon.png" width="64" align="left" alt="">

# R One-Seg

Receptor de TV digital ISDB-T One-Seg para o Haiku OS, desenvolvido para o
sintonizador integrado ao Sony VAIO P japonês (VGN-P70H). Só pode ser usado
em países que adotam o ISDB-T, como o Japão e o Brasil.

[日本語](README.md) · [한국어](README.ko.md) · [English](README.en.md) · [Português (Brasil)](README.pt-BR.md)

![Transmissão de TV ao vivo no R One-Seg em um VAIO P](captures/vaio-oneseg-2026-10-03.png)

## Recepção e reprodução no VAIO

A recepção, a descriptografia do enlace USB e a reprodução de vídeo e áudio
são realizadas no próprio VAIO. Aguarde a preparação do receptor ao abrir o
aplicativo, conecte a antena e faça a busca de canais. Clique em um canal da
lista para assistir. O controle de volume e o botão de tela cheia ficam na
parte inferior da janela.

## Instalação

No VAIO P com Haiku de 32 bits, instale o pacote com um único comando:

```sh
curl -fsSL https://pkgman.rainygirl.com/install-all.sh | sh -s -- roneseg_x86
```

Para instalar a partir do código-fonte:

```sh
git clone https://github.com/rainygirl/haiku-roneseg.git
cd haiku-roneseg
./install.sh
```

Os pacotes necessários e os dados do receptor são instalados automaticamente.
Após a instalação, abra o **R One-Seg** pelo Deskbar.

## Atalhos de teclado

| Tecla | Ação |
|---|---|
| Cima / Baixo | Selecionar um canal sem sintonizá-lo |
| Enter | Sintonizar o canal selecionado |
| Command-F | Alternar o dimensionamento do vídeo |
| Command-Shift-F | Alternar o modo de tela cheia |
| Esc | Sair da tela cheia |
| Command-U | Exibir o relatório de dispositivos USB |
| Command-. | Parar |

## Uso de IA

Este programa foi desenvolvido com o auxílio do Claude Code e do Codex.
