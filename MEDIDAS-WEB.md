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

## Detalhe — hero portado (sessão DESLOGADA)

> **Aviso de método.** Tudo abaixo foi medido **sem login** ("Continuar sem
> conta"). Deslogado o app web esconde parte da tela de título: não há lista de
> episódios, abas de temporada nem progresso. A **geometria do hero** medida
> aqui não depende disso, mas a tela de série logada tem seções a mais que
> ainda **não foram medidas** — quando forem, este arquivo tem de crescer.

A tela é **full-bleed**: backdrop 1920×1080 cobrindo tudo, vinheta por cima,
sem rail e **sem o cartão arredondado** que o port tinha. Medido com o título
"The Whisper Man" aberto.

| elemento | valor |
|---|---|
| shell / backdrop / vinheta | 1920×1080, x=0, y=0; fundo `#0d0d0d` |
| backdrop | `background-size: cover`, `position: 100% 0` |
| seção do hero | `padding: 0 96 32 72`, `justify-content: flex-end` |
| logo | 261×104 em (72, 445); altura fixa 104, max-width 710 |
| linha de ações | 1752×108 em (72, 589), padding 6, gap 24 |
| botão primário | 298×96 em (78, 595), raio 64, fonte **25/600**, texto PRETO em fundo branco, padding lateral 48, ícone 36, gap 16 |
| botões circulares | 84×84 em x=439, 586, 734 (passo 147), y=601, raio 999, `#222` |
| foco | **anel** `box-shadow 0 0 0 4px #fff`; `transform: none` — não há escala. O circular focado vira `#f5f5f5` com ícone `#111`; o primário **não muda de cor** |
| "Diretor: …" | 1040×36 em (72, 727), fonte 25/400, `rgb(179,179,179)`, lh 36.25 |
| sinopse | 1040×117 em (72, 787), fonte **26**/400, branco, lh 39, 3 linhas |
| pilha de meta | 1752×120 em (72, 928), gap 16 |
| meta linha 1 | y=928, caixa h=49, lh 35, fonte 25/400 `rgb(179,179,179)`: **gêneros à esquerda, ANO empurrado à direita** (termina em 1824), com um ponto de 1×14 a 24px de folga |
| meta linha 2 | y=1003, caixa h=45, lh 31, fonte **23**/400 **BRANCO**: duração • país |

**Correções ao que este arquivo dizia antes:** a linha de meta não é uma só nem
é toda 25/`rgb(179,179,179)` — são **duas**, e a segunda é 23px e **branca**. E
o ano fica na **primeira** linha, à direita, não na última.

### Vinheta

`linear-gradient(90deg, …)` de `#0d0d0d` a transparente, com nove paradas —
0%:1.00 · 7.8%:0.95 · 17.16%:0.84 · 28.08%:0.70 · 40.56%:0.52 · 51.48%:0.34 ·
60.84%:0.18 · 70.2%:0.07 · 78%:0. Rampas **lineares por partes**, como as do
hero da home. Implementada no modo `GFX_DETALHE` de `gfx.c`, que já desenha a
arte e a vinheta **numa passada só** — duas camadas de tela cheia custam caro
nesta GPU.

Não há pseudo-elementos: `.detail-bottom-shadow` existe no DOM mas com
`opacity: 0`.

### O que ficou diferente, e por quê

- **Dois botões circulares em vez de três.** O terceiro do web abre o trailer
  no YouTube, e este app não tem reprodutor de trailer. As posições x dos dois
  primeiros são as medidas.
- **A linha de duração não traz o país.** O `CatItem` do catálogo não tem esse
  campo.
- **Peso 600 vira Medium.** A Inter embarcada só tem Regular, Medium e Bold.

