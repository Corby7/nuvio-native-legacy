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

Abrir em 1920×1080 e medir pelo console.

> ## ⚠️ Duas sessões, dois conjuntos de números
>
> Tudo que este arquivo trazia até 2026-08-31 foi medido **sem login**
> ("Continuar sem conta"). Depois o dono logou (perfil "Henrique") e várias
> telas mudaram — algumas por terem mais dados, **outras por preferência de
> perfil**. Cada seção abaixo diz de qual sessão veio.
>
> Como reproduzir a sessão logada: o `localStorage` é por ORIGEM, então
> qualquer aba em `http://localhost:4173` herda a sessão. **Não limpe
> localStorage nesse endereço** — isso derruba o pareamento e obriga o dono a
> refazer o QR no celular.


## Home — sessão DESLOGADA (o que está aplicado hoje)


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

## Home — sessão LOGADA (medida 2026-08-31, **divergente**)

Com o perfil do dono a home é **outra tela**, e a diferença não é "mais dados":
é preferência de perfil. `localStorage.layoutPreferences`, perfil 1:

```json
{ "homeLayout": "modern", "continueWatchingCardStyle": "card",
  "heroSectionEnabled": true, "modernLandscapePostersEnabled": true,
  "modernHeroFullScreenBackdropEnabled": true, "posterLabelsEnabled": true }
```

É `modernHeroFullScreenBackdropEnabled: true` que troca o hero de faixa por
tela cheia, e `continueWatchingCardStyle: "card"` que dá os cards landscape.

| elemento | deslogado (aplicado) | logado (dono) |
|---|---|---|
| rail | 144 de largura | `.home-nav-list` com **largura 0** — não ocupa fluxo |
| coluna de conteúdo | x=248 | x=**104** |
| arte do hero | x=555, 1421×670 | **x=0, 1920×1062** (full-bleed) |
| bloco de texto do hero | x=248, largura 640 | x=104, largura 640, y 40…500 |
| logo do hero | 440×160 em (248,135) | 640×160 em (104,**65**) |
| linha de meta | y=327, fonte 21/500 | y=**257**, h 52, fonte 21/500 `rgb(179,179,179)` |
| linha secundária | *não existe* | y=**341**, h 38, fonte **18/600**, `rgba(255,255,255,.88)` — "2H RESTANTES • 6.3 • EN" |
| sinopse | y=411, 640, fonte 22 | y=411, 640×89, fonte 22/400 |
| título de fileira | y=518, h 31, fonte 26/600, ls −0.52 | idêntico, em x=104 |
| cards da fileira | y=564 | y=**563.6** (topo do cabeçalho + 45.2) |
| poster | 212×322, raio 24, passo 272 | **idêntico** |
| card "Continuar assistindo" | — | **432×247**, raio 24, passo **492** |
| passo entre fileiras | 416 | **415.2**; a de "Continuar assistindo" usa **358.2** |

**PENDÊNCIA PARA O DONO:** o port deve seguir o layout do perfil dele
(full-bleed, sem rail, x=104) ou o padrão (faixa, rail, x=248)? Não mudei a
home por conta disso — é escolha, não medida.

## Foco — MEDIDO, e corrige um defeito real

O app web **quase não escala o item em foco**, e **nunca o levanta**:

```
.home-screen-shell .home-poster-card.focused   { transform: none }
.home-screen-shell .home-content-card.focused  { transform: scale(1.01) }
.series-primary-btn.focused, .series-circle-btn.focused,
.series-secondary-btn.focused, .series-season-btn.focused,
.series-episode-card.focused, .series-insight-tab.focused { transform: none }
.movie-cast-card.focused, .series-insight-tab.focused     { transform: scale(1.03) }
```

O foco se marca por **cor de borda e box-shadow**, não por geometria:

| elemento | anel |
|---|---|
| card da home | `box-shadow` 2px `#f5f5f5`, inset **e** outset, em `.home-continue-media` / `.home-poster-frame`; transição `border-color .14s, box-shadow .14s` |
| botão do detalhe | `box-shadow 0 0 0 4px #fff`; o circular focado vira `#f5f5f5` com ícone `#111`; o primário **não muda de cor** |

Isto explica o defeito que o dono relatou (card em foco encostando no título da
fileira): o port usava escala 1.09 + levantamento de 8px, das tabelas de Top
Shelf do **tvOS**. Um poster de 322 crescendo 9% e subindo 8 tem o topo em
541.5, e o título da fileira termina em 549 — 7.5px de sobreposição, por
construção. Corrigido: escala 0, levantamento 0, anel `#f5f5f5` de 2px (era
azul `#339f5`, cor que não existe em lugar nenhum da interface).

## Movimento — MEDIDO na folha (o arquivo não tinha esta seção)

Nada de voo de cartão. As transições são de opacidade e deslocamento pequeno:

| o que | regra |
|---|---|
| entrada da home | `.home-route-content-enter` → `homeRouteEnter` **0.24s ease-out**, `opacity 0→1` + `translateY(2% → 0)` |
| entrada de busca / biblioteca / ajustes / addons | `searchRouteEnter` **0.35s ease**, só `opacity 0→1` |
| troca de tela (`.screen`) | `transition: 0.14s` |
| **detalhe rolado para as seções** | `.detail-scrolled .series-detail-backdrop { opacity: .15 }` e a vinheta e a sombra de base a 0 — **0.8s cubic-bezier(.4, 0, .2, 1)** |
| conteúdo do detalhe | `.series-detail-content { transition: opacity .4s cubic-bezier(.4,0,.2,1) }` |
| corpo do hero do detalhe | `max-height .6s`, `opacity .4s`, mesma curva |
| foco de botão/card do detalhe | `transform/background/border/box-shadow .22s cubic-bezier(0.22, 1, 0.36, 1)` |
| foco de card da home | `transform .12s ease-out, border-color .12s`; moldura `.14s` |
| episódio | `.series-episode-card { transition: transform .18s }` |

Confirma a descrição do dono: **a arte de fundo permanece** e o que muda são os
componentes. Ao descer, o web NÃO desfoca a arte — ele a **apaga para 15%** em
800ms. O desfoque gaussiano com escurecimento que o port fazia é do app da
Apple TV, custa duas passadas de tela cheia por quadro e mata a cor da arte.

`anim_mola` reproduz isto se a rigidez casar com o tempo medido:
`exp(-k·t)`, 95% do caminho em 800ms → **k ≈ 3.8** (`NV_MOLA_PAGINA`). Com a
rigidez de troca de tela (9.0) a arte apagava em ~330ms, menos da metade.

**AINDA NÃO MEDIDO:** a ordem exata da saída dos componentes da home quando o
detalhe abre (todos juntos ou escalonados; se o conteúdo do detalhe entra
durante a saída ou depois). Tentei amostrar por `requestAnimationFrame` e o
painel do navegador estrangula o rAF — só chegaram 10 amostras em 2s. Precisa
de outro método (gravação de vídeo, ou `element.getAnimations()`).


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


## Detalhe — sessão LOGADA (medida 2026-08-31, série "Silo" em progresso)

A geometria do hero se mantém, mas **o conteúdo muda e desloca a pilha**. E há
duas correções ao que este arquivo dizia:

**1. Os botões estão em FLUXO, com 63px entre vizinhos.** As posições
x=439/586/734 não são constantes: são o resultado da conta com o rótulo
"Reproduzir". Conferido em duas telas — Whisper Man (deslogado) e Silo
(logado) — o vão entre botões vizinhos é 63 nas duas.

**2. O vão entre o ícone e o rótulo do botão primário é 34, não os 16 que o
`gap` do flex declara.** Ícone começa em 126, rótulo em 196, ícone tem 36 de
largura. Largura do primário = `48 + 36 + 34 + textoW + 48` — dá 298 para
"Reproduzir" (texto 132) e 334 para "Retomar T2E3" (texto 168). Confere.

O que a sessão logada acrescenta:

| elemento | valor |
|---|---|
| rótulo do primário | "**Retomar T2E3**" em vez de "Reproduzir" quando há progresso |
| botão secundário | `.series-secondary-btn` **345×96** — "Reproduzir desde o início", `#222`, texto branco, raio 64, peso 600 |
| linha de retomada | `.detail-resume-indicator` 1720×**37** em (72,633), fonte 22.66/400 `rgba(255,255,255,.82)`: "Retomada disponível · 45% · Episódio S2E3 · 30m restantes" |
| logo | sobe para (72,**359**) — a pilha ficou mais alta |
| ações | (72,**503**) |
| "Roteirista: …" | (72,**688**) — em série é roteirista, em filme é diretor |
| sinopse | (72,748), 1040×117, fonte 26/400 — **inalterada** |
| pilha de meta | (72,**889**), altura **159** (era 120) |
| meta linha 1 | y=889, caixa h=**74** (cresce por causa do selo IMDb), texto em y=901; ano "2023-" à direita; **selo IMDb 109×60**, raio 999, logo 60×60 + nota fonte 20.7 |
| meta linha 2 | y=**989**, caixa h=**59**; **selo `.detail-meta-badge.strong`** "RETURNING SERIES" 249×45 raio 8; depois duração e país |
| largura do conteúdo | `.series-detail-content` = **1888** (não 1920): a borda direita útil vira 1792 |

### Seções abaixo da dobra (série) — **NÃO PORTADAS**

O nível 1 do port nativo ainda é a página do app da Apple TV (pílulas de
temporada 236×63, episódio 212 com texto abaixo, seções "Trailers", "Elenco e
equipe", "Você também pode gostar", "Como assistir", "Sobre"). **Nada disso
existe no web.** O que existe, medido:

| elemento | valor |
|---|---|
| `.series-season-row` | y=1080, altura 114; botões **269×80** em x=96, 417, 738 (passo **321**), raio 40, fonte 32/500; escolhido `#2d2d2d` texto branco, os outros `#222` texto `rgb(179,179,179)` |
| `.series-episode-track` | y=1194; cards **640×422** em x=96, passo **726**, raio 32 |
| miniatura do episódio | 640×**414** — e **o texto fica DENTRO dela**, sobreposto na base, não abaixo. É a diferença estrutural com o port. |
| selo do episódio | 163×44, fonte 20/600, fundo `rgba(0,0,0,.42)`, raio 12 |
| título do episódio | fonte **32/800**, lh 44 |
| sinopse do episódio | fonte **28**/400 `rgba(255,255,255,.9)`, lh 36, 2–3 linhas |
| meta do episódio | fonte 20/400 `rgb(179,179,179)`: ícone de relógio + duração + data por extenso |
| progresso do episódio | barra 576×**8**, raio 999; trilho `rgba(0,0,0,.45)`, preenchimento `rgb(158,158,158)` |
| `.series-insight-tabs` | y=1680; "Criador e elenco \| Avaliações \| Trailer", fonte 32/500; escolhida branca, as outras `#808080`; divisor "\|" fonte 32/700 `#808080` |
| elenco | avatar **140×140** raio 999, nome fonte **26/500** `rgb(179,179,179)`, card 220 de largura, passo **270**, a partir de x=96 |

