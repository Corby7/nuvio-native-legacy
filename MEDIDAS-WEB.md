# Medidas do app web — referência do port 1:1

Tudo aqui foi **medido no app web rodando** em 1920×1080, com
`getBoundingClientRect` e `getComputedStyle`, não lido da folha de estilo.

Por que isso importa: ler o CSS engana. `.player-title` declara `font-size: 28px`
em `components.css:12582` e é **sobrescrito** por `#playerUiRoot` na 14632 para
`min(2.92vw, 56px)` = 56px. Quem lê só o primeiro bloco erra por metade.

Como reproduzir:

```bash
cd ../NuvioWeb-0.3.38-beta && npm run serve     # porta 4173
```

Abrir em 1920×1080, "Continuar sem conta", e medir pelo console.

## Home

| elemento | valor |
|---|---|
| rail | 144 de largura, altura cheia |
| itens de nav | 96×104 em x=24; primeiro em y=340; passo 144 |
| conteúdo | começa em x=248 (rail 144 + 104) |
| arte do hero | x=555, y=0, 1421×670, `object-fit: cover` |
| logo do hero | 440×160 em (248, 135) |
| título sem logo | 76px, peso 600, letter-spacing −2.28 |
| linha de meta | y=327, h=52, fonte 21, peso 500, `rgb(179,179,179)` |
| sinopse | y=411, largura 640, h 89, fonte 22, peso 400, ls 0.5 |
| título da fileira | y=518, h=31, fonte 26, peso 600, ls −0.52 |
| cards da fileira 0 | y=564 |
| card | 212×322, raio 24 |
| gap entre cards | 60 (passo 272) |
| passo entre fileiras | 416 |
| título do poster | 16, peso 500 |
| subtítulo do poster | 13, peso 400, `rgba(255,255,255,0.7)` |

### Degradês do hero

Nos pseudo-elementos de `.home-modern-hero-media`:

- `::before` — horizontal, cobre os **639px esquerdos** de 1421 (45% da UV):
  `#0d0d0d` → 0.86 em 22% → 0.56 em 46% → 0.16 em 76% → 0
- `::after` — vertical, altura toda:
  0 até 82% → 0.25 em 89.2% → 0.65 em 95.5% → sólido no fim

São rampas **lineares por partes**. Um `smoothstep` único não passa pelos pontos
intermediários (em 89.2% dá 0.35 no lugar de 0.25), e é o miolo da rampa que se
enxerga. Implementadas com `clamp` no modo `GFX_HERO` de `gfx.c`.

## Player

| elemento | valor |
|---|---|
| margens | x 64, y 48 |
| botão | 96, gap 4; ícone 48 |
| barra | altura 6 (10 com foco), raio 3 |
| trilho | `rgba(255,255,255,0.30)` (0.45 com foco) |
| preenchimento | `#f5f5f5` (`--secondary-color`); buffer a 0.35 |
| barra → topo | margin-top 12 (a partir da meta) |
| linha de botões | margin-top 16 |
| gradientes | topo 150 (0.7→0), base 200 (0→0.8) |
| título | 56, peso 700 |
| subtítulo e tempo | 32, peso 400, `rgba(255,255,255,0.9)` |
| rótulo de tempo | **um só**, `decorrido / total`, empurrado à direita |

## Detalhe — **ainda não portado**

A tela é **full-bleed**: backdrop 1920×1080 cobrindo tudo, vinheta por cima,
sem rail e **sem o cartão arredondado** que o port tem hoje. O nativo desenha o
cartão que voa a partir do pôster, que é o padrão do app da Apple TV.

| elemento | valor |
|---|---|
| shell / backdrop / vinheta | 1920×1080, x=0, y=0 |
| coluna de conteúdo | x=**72** (não 248) |
| logo | 261×104 em (72, 445) |
| botão primário | 298×96 em (78, 595), raio 64, fonte 25 peso 600, texto preto |
| botões circulares | 84×84, raio 999, em x=439, 586, 734 (passo ~147), y=601 |
| pilha de meta | x=72, y=928, 1752×120 |
| linha de meta | fonte 25, peso 400, `rgb(179,179,179)` |

Ordem vertical observada: logo → botões → "Diretor: …" → sinopse → gêneros →
linha final com duração e país à esquerda e o ano à direita.
