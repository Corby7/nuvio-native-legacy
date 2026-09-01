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

**RESOLVIDO NO FONTE, não era pergunta.** `js/data/local/layoutPreferences.js`
tem os padrões de fábrica, e o perfil do dono só difere neles:

```
collapseSidebar: true                        <- a rail não some, ela é RECOLHIDA
modernHeroFullScreenBackdropEnabled: true    <- troca a faixa por tela cheia
continueWatchingCardStyle: "card"            <- os cards landscape
modernLandscapePostersEnabled: true
```

Não são dois layouts: é **uma** tela dirigida por preferência. E a regra do
recuo fica óbvia quando se olha assim — o conteúdo tem **sempre 104** de recuo,
e a rail acrescenta os 144 dela quando está fixa (104 recolhida, 248 fixa). O
port passou a ler as preferências e a expô-las em Ajustes.

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

## A saída da home quando o detalhe abre — **MEDIDO, e a resposta é "não há"**

Ficava aqui um "AINDA NÃO MEDIDO" sobre o escalonamento da saída. Medido em
2026-09-01 com `MutationObserver` + `document.getAnimations()` + `getTiming()`
amostrado por `setTimeout` (o rAF do painel é estrangulado; `setTimeout` não é,
com a aba visível). Método reprodutível: instrumentar, clicar num
`.home-poster-card[data-action=openDetail]`, amostrar em 0/20/40/…/1300ms.

**Não existe animação de saída.** O que a instrumentação viu:

| t (ms) | o que aconteceu |
|---|---|
| 1 | `#home` recebe `style="display:none"` **e** `#detail` recebe `style="display:block"`, no mesmo lote de mutação |
| 2…1300 | nenhuma animação nem transição em nenhum nó da home ou do detalhe |

As três perguntas em aberto, respondidas:

1. **Juntos ou em cascata?** Nem um nem outro — a home é escondida por
   `display`, num quadro só. `display` não é animável, então a
   `transition: 0.14s` de `.screen` nunca chega a rodar.
2. **O conteúdo do detalhe entra durante a saída ou depois?** No **mesmo
   quadro**. As duas trocas de `display` saem no mesmo lote.
3. **`homeRouteEnter` 0.24s `translateY(2%)`?** **Não roda neste runtime.**

Por que: o app carrega com `<body class="performance-constrained legacy-webos
no-flex-gap no-aspect-ratio no-css-math no-backdrop-filter">`, e a folha tem

```css
/* components.css:18190 */
.performance-constrained * {
  animation-duration: 0.01ms !important;
  animation-iteration-count: 1 !important;
  transition-duration: 140ms !important;
  transition-delay: 0ms !important;
}
/* components.css:18245 */
.performance-constrained .home-route-content-enter,
.performance-constrained .search-route-enter,
.performance-constrained .library-route-enter,
.performance-constrained .settings-route-enter { animation: none !important }
```

Consequências que valem para o port inteiro, não só para esta transição:

- **Não pode haver escalonamento em lugar nenhum**: `transition-delay: 0ms
  !important` vale para `*`. Qualquer cascata que o port inventar é invenção.
- `homeRouteEnter` (0.24s, `translateY(2%)`) e `searchRouteEnter` (0.35s) estão
  na folha mas **não rodam**. As linhas da tabela de "Movimento" acima que os
  citam descrevem o CSS, não o comportamento nesta TV.
- Transições sem `!important` próprio caem para **140ms**. Confirmado medindo
  `getComputedStyle` na tela de detalhe aberta: `.series-primary-btn` computa
  `0.14s` (a folha declara .22s) e `.detail-bottom-shadow` computa `0.14s`.
- As que sobrevivem (têm `!important` próprio), medidas na mesma tela:
  `.series-detail-backdrop` **0.8s** `cubic-bezier(.4,0,.2,1)` — confirma o
  `NV_MOLA_PAGINA` 3.8; `.detail-hero-body` **0.6s**; `.series-detail-content`
  **0.25s** (a tabela acima dizia .4s — **corrigir**).
- Foco de card da home neste runtime é **200ms
  `cubic-bezier(0.22, 0.61, 0.36, 1)`** (components.css:18258), e não os .12s/.14s
  que a folha declara.

Ou seja: a descrição do dono ("a arte do background PERMANECE e os componentes
saem como se descessem") corresponde ao que a folha *pretende*, não ao que a TV
*executa*. Na TV a arte permanece porque o detalhe desenha o próprio backdrop, e
os componentes não descem: eles somem no mesmo quadro.


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


## Catálogos da home — de onde sai a lista (MEDIDO, **não portado**)

O port declara quatro fileiras num array estático em `src/home.c`
(`"Continue Assistindo"`, `"Popular - Movie"`, `"Popular - Series"`,
`"Em alta"`). No web nada disso é fixo. A fonte de verdade é
`localStorage.homeCatalogPrefs`, escopado por perfil:

```json
{ "__profileScoped": true, "version": 1,
  "profiles": { "1": { "order": [...], "disabled": [...], "customTitles": {...} } } }
```

- **`order`** — a ordem das fileiras, por chave. Duas formas de chave:
  `<addonId>_<tipo>_<catalogoId>` (ex.
  `app.xperience.<uuid>_movie_recs_movies_for_you`) e `collection_<uuid>`
  para as coleções do próprio usuário. No perfil do dono são **mais de 30**.
- **`disabled`** — as que o perfil desligou (vazio hoje). É isto que responde
  "o que acontece com uma que o perfil desativou": ela sai da home.
- **`customTitles`** — renomeação por fileira; sem entrada, o título vem do
  manifesto do addon.
- **`installedAddonEnabledStates`** — 3 addons instalados; um addon desligado
  leva junto os catálogos dele.

"Continuar assistindo" **não** está em `order`: é uma fileira sintética, sempre
primeira quando há itens, montada de `continueWatchingItems` /
`watchProgressItems`.

Do lado nativo já existem `addons.c` (lê `addons.txt`, com coluna dizendo se o
addon fornece catálogo) e `descoberta.c` (monta o acervo pela rede). O que
falta é a home parar de cravar a lista e passar a perguntar.

**PENDÊNCIA PARA O DONO:** o nativo deve ler `homeCatalogPrefs` do app web
(mesmo aparelho, outro processo — e o app web mantém o arquivo aberto, como já
aconteceu com o progresso), ou receber a lista pela rede junto com o acervo?
Não inventei uma ordem.


## Preferências de layout — a fonte, e o que o port faz com elas

`js/data/local/layoutPreferences.js`, `DEFAULTS` (padrão de fábrica):

| chave | padrão | perfil do dono | port |
|---|---|---|---|
| `homeLayout` | `"modern"` | `"modern"` | só o moderno existe; não exposto |
| `collapseSidebar` | `false` | **`true`** | ✅ "Barra lateral" |
| `heroSectionEnabled` | `true` | `true` | ✅ "Destaque na home" |
| `modernHeroFullScreenBackdropEnabled` | `false` | **`true`** | ✅ "Destaque em tela cheia" |
| `continueWatchingCardStyle` | `"card"` | `"card"` | ✅ "Estilo do Continuar assistindo" (card/largo/pôster) |
| `posterLabelsEnabled` | `true` | `true` | ✅ "Rótulos nos pôsteres" |
| `modernLandscapePostersEnabled` | `false` | **`true`** | ✅ exposto; o desenho landscape ainda não |
| `posterCardWidthDp` / `posterCardCornerRadiusDp` | 126 / 12 | 120 / 12 | ver abaixo |
| `cardDepth*`, `focusedPosterBackdropExpand*` | — | ligados | **não portados** |

### O tamanho do pôster NÃO vem de `posterCardWidthDp`

Vale corrigir uma suposição razoável mas errada. `buildModernHomeSizingStyle`
(homeScreen.js:521) calcula, com `dpToPx = 2`:

```
portraitWidth  = round(dp * 0.84 * 1.08 * 2)   // 120 -> 218
portraitHeight = round(dp * 1.5 * 0.84 * 1.08 * 2)  // 120 -> 327
radius         = round(radiusDp * 2)           // 12  -> 24
```

E de fato `--home-poster-width: 218px` / `--home-poster-height: 327px`. **Mas o
card medido é 212×322.** A razão está no CSS do layout moderno, que ignora a
variável:

```css
.home-screen-shell.home-layout-modern .home-poster-card:not(.is-landscape)
  { min-width: 212px; max-width: 212px; flex-basis: 212px }
.home-screen-shell.home-layout-modern .home-poster-card:not(.is-landscape)
  .home-poster-frame { height: 318px }
```

212 de largura, moldura de 318 mais 2px de borda em cima e embaixo = **322**. As
variáveis `--home-poster-*` são do layout **clássico**. Ou seja: 212×322 é
constante do layout moderno e **não** muda com `posterCardWidthDp` — só o raio
(24) sai da preferência. O `layout.h` pode manter os números, desde que diga
isso.

**CONFIRMADO POR EXPERIMENTO (2026-09-01), não mais por leitura.** Na home
aberta, troquei a variável inline no shell:

```
--home-modern-portrait-poster-width: 218px -> 300px   card continuou 212
--home-modern-portrait-poster-height: 327px -> 400px  moldura continuou 318
```

Ou seja `posterCardWidthDp` **não dimensiona nada** no layout moderno. Da
preferência sai só o raio.

### Três armadilhas a mais, descobertas no fonte

**1. `posterLabelsEnabled` não tem efeito no layout moderno — por decisão da
folha.**

```css
/* components.css:7334 */
.home-screen-shell.home-layout-modern .home-poster-copy { display: none }
```

E é por isso que a tela de Ajustes do web **esconde a opção** quando o layout é
moderno: `settingsScreen.js:4050` embrulha a linha em `!isModernLayout`. O port
desenha o rótulo quando a preferência está ligada (foi o pedido explícito do
dono); para ficar pixel a pixel com o web hoje, basta desligá-la.

**2. `modernLandscapePostersEnabled` está QUEBRADO no web, e a quebra é uma
chave errada.** O perfil do dono tem a preferência em `true`, o shell recebe
`.home-modern-landscape-posters` — e mesmo assim **todos os pôsteres de catálogo
medem 212×322 em pé**. Os únicos `.is-landscape` na tela são cards de coleção
(`is-collection-landscape`), cuja forma vem do `tileShape` da coleção.

A causa está no reconciliador:

```js
// homeScreen.js:11922, reconcileHomeCatalogRows()
showPosterLabels: this.layoutPrefs?.showPosterLabels !== false,
showCatalogTypeSuffix: this.layoutPrefs?.showCatalogTypeSuffix !== false,
preferLandscapePosters: Boolean(this.layoutPrefs?.preferLandscapePosters),
```

`layoutPrefs` **não tem** `preferLandscapePosters` (a chave é
`modernLandscapePostersEnabled`), nem `showPosterLabels`/`showCatalogTypeSuffix`
(são `posterLabelsEnabled`/`catalogTypeSuffixEnabled`). O render completo em
`renderModernHomeLayout` passa as chaves certas, mas o reconciliador roda a cada
fileira que chega da rede e reescreve tudo com `false`. O card deitado aparece
por um instante no primeiro render e morre no primeiro reconcile.

O port implementa o efeito pretendido (o dono pediu "implemente o efeito das
duas"). **Se o objetivo for igualar a tela de hoje, a correção é do lado do web**:
trocar as três chaves em `homeScreen.js:11920-11922`.

**3. `focusedPosterBackdropExpandEnabled` está desligado no código, de
propósito.** `homeScreen.js:6812`:

```js
const HOME_POSTER_EXPAND_DISABLED = true;
const shouldExpand = HOME_POSTER_EXPAND_DISABLED ? false : ...;
```

com um comentário do próprio dono explicando a decisão ("o hero já mostra arte,
título e sinopse do item focado"). Portanto **o pôster focado NÃO cresce para
563.92 depois de 3s** no app que ele usa. O port guarda a preferência e o atraso,
mas não desenha o crescimento — desenhá-lo seria divergir da tela real.

Outras larguras da mesma folha, ainda não portadas:

- `.home-poster-card.is-landscape` — **318** de largura, moldura 178.875 (16:9).
  É o `modernLandscapePostersEnabled`.
- `.home-poster-card.is-expanded` — **563.92** de largura, moldura 318. É o
  `focusedPosterBackdropExpandEnabled`: o pôster em foco **cresce sozinho depois
  de 3s** (`focusedPosterBackdropExpandDelaySeconds`) e mostra o backdrop com um
  gradiente. É comportamento visível e o port não tem.

### Hero em tela cheia — as rampas

MEDIDO nos pseudo-elementos de `.home-modern-hero-media` com a preferência
ligada (1920×1062 em 0,0, imagem `cover` com `object-position: 100% 0`):

- `::before` — horizontal, cobre os **1248px esquerdos de 1920** (65%):
  `#0d0d0d` → 0.90 em 22% → 0.80 em 46% → 0.42 em 76% → 0
- `::after` — vertical, altura toda:
  0 até **64%** → 0.35 em 74.8% → 0.75 em 85.6% → sólido no fim

As paradas percentuais são **as mesmas** do hero em faixa; o que muda é a
cobertura (65% da largura contra 45%) e a profundidade. Faz sentido: com a arte
ocupando a tela toda, o texto precisa de mais fundo escuro sob ele.
Implementadas em `GFX_HERO_CHEIO`, ao lado de `GFX_HERO`.


## Busca — MEDIDA (sessão LOGADA, 2026-09-01). Nunca tinha sido comparada.

Fundo `#0d0d0d`. Rail recolhida (x=-48), conteúdo em 104, como a home.

| elemento | valor |
|---|---|
| `.search-header` | y=22, h=110, padding `0 104` |
| `.search-discover-btn` | 110×110 em (104,22), `#222`, borda 1px `#333`, raio 22, ícone 54 |
| `.search-voice-btn` | 110×110 em (262,22) — passo **158** (gap 48) |
| `.search-input-field` | 1396×110 em (420,22) → borda direita 1816; `#222`, raio 22, fonte **34/500**, padding `0 32`; placeholder "Buscar filmes e séries" |
| campo em foco | borda `#f5f5f5` + `box-shadow 0 0 0 2px rgba(245,245,245,.22)` |
| `.search-empty-state` | y=148, h=400, centrado; ícone 136×136 em y=220.5 |
| título do vazio | 56/600 branco, lh 58.24, y=378.5 |
| apoio do vazio | 24/400 `rgb(179,179,179)`, lh 28.8, y=446.7 |

Os resultados **não são uma grade**: são fileiras horizontais, uma por catálogo.

| elemento | valor |
|---|---|
| `.search-results-title` | 48/600 branco, lh 51.84, padding `0 104` |
| `.search-results-subtitle` | 20/400 `rgb(179,179,179)`, margin-top 4 → +55.8 do título |
| `.search-results-track` | +88.3 do título; cards +4 do trilho (→ +92.3) |
| `.search-result-card` | 248 de largura; x = 104, 384, 664 → passo **280** |
| `.search-result-poster-wrap` | 248×**372**, raio 22, `#222`, borda 2px |
| `.search-result-name` | 28/500 branco, lh 33.6, margin-top 8 |
| `.search-result-date` | 20/400 `rgb(179,179,179)`, margin-top 4 |
| passo entre fileiras | **562.4** (546.4 de altura quando há data; 523.9 sem) |
| `.search-seeall-card` | mesma caixa 248×440.1, no fim do trilho |

**O web não tem teclado na tela**: o `<input>` é servido pelo IME do sistema da
TV. O port é SDL puro e não tem IME — o teclado em grade fica, e é a única
divergência deliberada desta tela.


## Biblioteca — MEDIDA (sessão LOGADA, 2026-09-01). Nunca tinha sido comparada.

`.library-main` tem `padding: 48px 96px 64px` → conteúdo em x=**96**, y=48,
largura **1728**. (Note que é 96, e não os 104 da home e da busca.)

| elemento | valor |
|---|---|
| `.library-page-title` | "Biblioteca" 56/600, letter-spacing **1px**, em (96,48), h=56 |
| `.library-page-source` | selo "NUVIO" 28/500 `rgb(128,128,128)`, ls **4px**, padding-top 10, alinhado à direita (termina em 1824) |
| `.library-view-mode-row` | y=136, h=56, gap 16 |
| `.library-view-mode-button` | 150×56, raio 999, padding `14 24`, fonte 21/400; x = 96, 278 → passo **182** |
| — escolhida | `#303030`, borda 2px `#fff` |
| — as outras | `#222`, borda 2px `#333` |
| `.library-picker-row` | y=212, h=110 |
| `.library-picker-anchor` | **840×110** em x=96 e x=**984** (passo 888), raio 36, padding `18 28` |
| — em foco | `#303030`, borda 1px `#fff` |
| — fora de foco | `#222`, borda 1px `rgba(255,255,255,.1)` |
| `.library-picker-title` | 19/500 `rgb(128,128,128)`, ls 0.45, lh 24 |
| `.library-picker-value` | 30/500 branco, ls 0.3, lh 40, margin-top 4 |
| `.library-picker-icon` | 40×40 (svg 32) encostado à direita |
| `.library-empty-state` | y=354, padding-top 38, gap 18 |
| — título | 46/500 branco, lh 49.68 |
| — apoio | 28/400 `rgb(179,179,179)`, lh 35 |

Grade (`.library-grid`), lida da folha e conferida na largura útil:

```css
grid-template-columns: repeat(auto-fill, minmax(var(--library-poster-width, 252px), 1fr));
gap: 32px 24px;
```

1728 úteis com mínimo 252 e gutter 24 → **6 colunas de 268**. Pôster 2:3 →
268×**402**, raio 24, com `border: 4px solid transparent` (a borda de foco é
**por dentro**). Título 32/500, lh 1.18 (37.8), a **16** do pôster. Altura do
card 455.8; passo de linha **487.8**.

Foco: `transform: scale(1.02)` com `transform-origin: center top`, borda vai
para `--focus-color`, e `box-shadow: none` — a folha comenta: *"Android TV uses
the inside focus border, not an outer halo"*. É a **única** escala de foco que
sobrou em qualquer tela do web; as outras (9%, 14%) eram das tabelas de Top
Shelf do tvOS e não desta interface.

As três abas do port ("Minha Lista", "Comprados", "Gêneros") não existem no web.
O que existe são **duas** dimensões: o modo (Salvos/Nuvem) e os dois seletores
(Tipo, Ordenar).
