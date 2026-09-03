#!/usr/bin/env python3
"""Confere o gerador de QR DECODIFICANDO o que ele desenha.

Comparar com outra implementacao nao serve como prova: dois simbolos podem
diferir modulo a modulo (enchimento diferente, outra mascara escolhida) e os
dois serem validos. O que importa e uma so pergunta — um leitor de verdade le?
Por isso a conferencia decodifica com o OpenCV em vez de comparar matrizes.

Isto NAO e paranoia: um QR errado nao da erro. Ele desenha, fica bonito na
tela, os localizadores estao no lugar, e nenhum celular decodifica. Foi
exatamente o que aconteceu aqui — a ordem dos bits do formato estava invertida
e so o decodificador acusou.

Preparo (uma vez):
    python3 -m venv /tmp/qrvenv && /tmp/qrvenv/bin/pip install opencv-python-headless numpy

Uso:
    cc tools/qr_despejo.c src/qr.c -o /tmp/qr_despejo -Isrc
    /tmp/qrvenv/bin/python tools/qr_conferir.py /tmp/qr_despejo
"""
import subprocess
import sys

import cv2
import numpy as np

# Um caso por motivo, nao por capricho:
CASOS = [
    "https://nuvio.tv/tv-login?code=fa0010cad8b5d2f512e58646ab82ca6b",  # o caso real
    "https://nuvio.tv/tv-login?code=00000000000000000000000000000000",  # dados quase todos iguais
    "A",                    # a menor entrada possivel
    "HELLO WORLD",
    "12345678901234567",
    "x" * 53,               # ultimo que cabe na versao 3
    "y" * 54,               # o primeiro que forca a versao 4
    "z" * 78,               # ultimo da versao 4
    "w" * 106,              # versao 5
    "k" * 134,              # versao 6, o limite do modulo
]
# Acima do limite o app TEM de recusar em vez de desenhar algo truncado.
CASOS_QUE_DEVEM_FALHAR = ["q" * 135, ""]


def matriz(binario, texto):
    saida = subprocess.run([binario, texto], capture_output=True, text=True)
    linhas = saida.stdout.strip().splitlines()
    if not linhas or linhas[0].startswith("nao coube"):
        return None
    return [l.strip() for l in linhas[1:]]


def decodificar(m, escala=8, quieta=4):
    lado = len(m)
    img = np.full((lado + 2 * quieta, lado + 2 * quieta), 255, dtype=np.uint8)
    img[quieta:quieta + lado, quieta:quieta + lado] = np.array(
        [[0 if c == "1" else 255 for c in linha] for linha in m], dtype=np.uint8
    )
    img = cv2.resize(img, None, fx=escala, fy=escala, interpolation=cv2.INTER_NEAREST)
    texto, _, _ = cv2.QRCodeDetector().detectAndDecode(img)
    return texto


def main():
    binario = sys.argv[1] if len(sys.argv) > 1 else "/tmp/qr_despejo"
    falhas = 0
    for texto in CASOS:
        m = matriz(binario, texto)
        rot = texto if len(texto) <= 34 else f"{texto[:10]}…({len(texto)}B)"
        if m is None:
            print(f"FALHA  {rot}: o app recusou, mas isto cabe")
            falhas += 1
            continue
        lido = decodificar(m)
        if lido == texto:
            print(f"ok     {rot}: {len(m)}x{len(m)} decodificado igual")
        else:
            print(f"FALHA  {rot}: {len(m)}x{len(m)} decodificou como {lido[:34]!r}")
            falhas += 1
    for texto in CASOS_QUE_DEVEM_FALHAR:
        if matriz(binario, texto) is None:
            print(f"ok     ({len(texto)}B): recusado, como esperado")
        else:
            print(f"FALHA  ({len(texto)}B): devia ter sido recusado")
            falhas += 1
    print("\n" + ("TODOS LEGIVEIS" if not falhas else f"{falhas} FALHA(S)"))
    return 1 if falhas else 0


if __name__ == "__main__":
    sys.exit(main())
