# Plano — legendas customizáveis e desempenho percebido

Data: 2026-09-02. TCL de referência (com.nuvio.tv, 1920x1080) medida por
`adb screenrecord`. A TV LG estava fora da rede durante a investigação: o que
vem dela é o que já estava anotado no código, com método.

Convenção: **[MEDIDO]** = número obtido com método declarado. **[SUPOSTO]** =
inferência de leitura de código, sem número. **[NÃO MEDIDO]** = precisa da TV.
Nada aqui é número inventado.

---

## FRENTE 1 — Customização de legenda

### 1.1 O uMS EXPÕE atributos de legenda (evidência)

Hoje a legenda não é nossa: `video.c` só liga/desliga e escolhe faixa
(`setSubtitleEnable`, `selectTrack type:text`, `setSubtitleSource`). A pergunta
era se dá para ESTILIZAR sem reescrever. Dá.

Fonte 1 — o próprio SDK que compila o app (`tools/Dockerfile`, sysroot
buildroot openlgtv). As strings de `usr/lib/libums_pipeline.so` registram estes
métodos LS2, com o nome do campo JSON ao lado de cada um:

| Método `luna://com.webos.media/…` | Campo | Tipo |
|---|---|---|
| `setSubtitlePosition` | `position` | int, −3..4 |
| `setSubtitleSync` | `sync` | int (ms) |
| `setSubtitleFontSize` | `fontSize` | int, 0..4 |
| `setSubtitleColor` | `color` | int, 0..5 |
| `setSubtitleEncoding` | `encoding` | string |
| `setSubtitlePresentationMode` | `presentationMode` | string |
| `setSubtitleCharacterColor` | `charColor` | string |
| `setSubtitleCharacterOpacity` | `charOpacity` | int 0..255 |
| `setSubtitleCharacterFontSize` | `charFontSize` | string |
| `setSubtitleCharacterFont` | `charFont` | string |
| `setSubtitleBackgroundColor` | `bgColor` | string |
| `setSubtitleBackgroundOpacity` | `bgOpacity` | int 0..255 |
| `setSubtitleCharacterEdge` | `charEdgeType` | string |
| `setSubtitleWindowColor` | `windowColor` | string |
| `setSubtitleWindowOpacity` | `windowOpacity` | int 0..255 |

`libplayerAPIs.so` tem os handlers correspondentes, mais quatro que o uMS deste
SDK NÃO expõe por LS2 (`setSubtitleCharacterEdgeColor`,
`setSubtitleCustomStyleEnable`, `setSubtitleCharacterBlinking`,
`setSubtitleBackgroundBlinking`).

Dois nomes que eu havia chutado NÃO existem: `setSubtitleAttribute` e
`setSubtitleEdgeType` — o certo é `setSubtitleCharacterEdge`, campo
`charEdgeType`.

Fonte 2 — o app web (`playerController.js:125-140,4358-4395`) já chama
`setSubtitleFontSize` no webOS, com o comentário "five discrete subtitle sizes
(0=tiny, 4=largest)". É a única chamada de estilo que ele usa.

Fonte 3 — gist comunitário (aabytt/bddbb1bcf031a050d89a89aeee3a6737), que bate
com os nomes do SDK e dá os valores: `color` 0 amarelo, 1 vermelho, 2 branco
(padrão), 3 verde, 4 azul, 5 cinza; cores nomeadas em
{black,white,yellow,red,green,blue}; `position` −3 (mais baixa) a 4.

NÃO ACHEI o vocabulário de `charEdgeType`, `charFont`, `charFontSize` e
`presentationMode`. A doc oficial do webOS OSE não documenta método de legenda
nenhum. Kodi não ajuda: renderiza legenda por conta própria.

RESSALVA: os `.so` lidos são os do SDK, não os da TV. Antes de qualquer código,
conferir no aparelho — `grep -a -o 'setSubtitle[A-Za-z]*' /usr/lib/libums_pipeline.so | sort -u`.

### 1.2 Decisão: usar o pipeline (a), não render próprio (b)

Render próprio daria controle contínuo de tudo, e custa: parser SRT/VTT,
sincronização com `video_pos()`, e o texto passa a competir com
`TXT_POR_QUADRO=2`. Pior: **só cobre legenda EXTERNA** — as embutidas em MKV
exigiriam demux, que não fazemos (`mkv.c` lê só o cabeçalho). E PGS/VobSub são
IMAGEM: não há como re-renderizar, em nenhum dos dois caminhos.

O pipeline cobre embutida e externa com o mesmo código e custo zero de quadro.
Render próprio fica como plano B, só para externa, e só se o teste na TV mostrar
que o firmware 4.10 ignora `fontSize`/`charColor`.

### 1.3 Como a referência apresenta

APK da TCL: `SubtitleStyleSettings(preferredLanguage, textColor, outlineColor)`,
eventos `OnSetSubtitleSize/Bold/TextColor/OutlineEnabled/OutlineColor/VerticalOffset`,
UI em `SubtitleSelectionOverlay` com um "style rail" na MESMA folha de seleção.
Web: Delay, Font Size %, Bold, Text Color, Text Opacity, Outline, Outline Color,
Bottom Offset, Reset.

Não capturei o painel de estilo da TCL (exigiria abrir um título e gastar link
de debrid). Protocolo para capturar depois está em 2.6.

### 1.4 Opções a oferecer

| Opção | Valores | Chamada |
|---|---|---|
| Tamanho | Muito pequena / Pequena / **Padrão** / Grande / Enorme | `setSubtitleFontSize` 0..4 |
| Cor | **Branco** / Amarelo / Verde / Azul / Vermelho / Preto | `setSubtitleCharacterColor` |
| Fundo | **Nenhum** / Escuro 50% / Escuro 100% | `setSubtitleBackgroundColor "black"` + `setSubtitleBackgroundOpacity` 0/128/255 |
| Posição | 8 níveis | `setSubtitlePosition` −3..4 |
| Borda | Nenhuma / Contorno / Sombra — **depende do teste** | `setSubtitleCharacterEdge`; candidatos CEA-708: none, raised, depressed, uniform, dropShadow |
| Atraso | −5s..+5s passo 250ms | `setSubtitleSync` |

Não oferecer: opacidade do texto (o web marca indisponível em webOS nativo),
fonte (sem vocabulário conhecido), janela (redundante com fundo).

### 1.5 Itens, em ordem

**F1.0 — PROVA NA TV antes de codar UI.** Com filme tocando, por telnet:
`luna-send -n 1 luna://com.webos.media/setSubtitleFontSize '{"mediaId":"<id>","fontSize":4}'`
e assim por diante, inclusive cada candidato de `charEdgeType`. Verificação:
`returnValue:true` E foto da TV — a legenda é desenhada pelo pipeline, então
NÃO sai no `glReadPixels`, igual ao vídeo. Repetir com externa e com PGS.
O resultado decide a lista final.

**F1.1 — API em video.c/h.** `VideoLegendaEstilo {tamanho,cor,fundo,posicao,borda,atrasoMs}`
e `video_legenda_estilo()`, emitindo por `chamar()` (video.c:614). Reaplicar em
`video_escolher_legenda` (1129), `video_legenda_externa` (1148) e no
`loadCompleted` (558) — o pipeline nasce a cada load. Stub vazio no Mac (43-50).

**F1.2 — Persistência** em `player.c:277-304` (`art/player.txt`, formato
`chave valor`): `leg_tamanho`, `leg_cor`, `leg_fundo`, `leg_pos`, `leg_borda`,
`leg_atraso`. É preferência DO APARELHO, como `aspecto` — não vai em
`ajustes.txt`, que espelha as chaves de layout do web.

**F1.3 — UI: terceira coluna "Estilo" em faixas.c.** Linhas `rótulo ◂ valor ▸`;
OK cicla o valor e aplica na hora sem fechar; LEFT volta para Legendas.
`FX_LARG` 1180 → 1400 para caber 3 colunas.
PRÉ-VISUALIZAÇÃO: `faixas_desenhar` escurece a tela a 72% e a legenda do
pipeline fica embaixo — invisível. Com `coluna==2`, baixar o véu para ~0.25 e
ancorar o painel no TOPO, deixando o terço inferior livre. É o live preview que
o web tem.

**F1.4 — Legenda de imagem.** `mkv.c` já lê Tracks; acrescentar `CodecID`
(EBML 0x86) para distinguir `S_TEXT/UTF8`, `S_TEXT/ASS`, `S_HDMV/PGS`,
`S_VOBSUB`. Marcar na folha e avisar que estilo não se aplica.

**F1.5 — Plano B**, só se F1.0 falhar: `src/legenda.c` com parser SRT/VTT,
desenhado sobre o furo. Não cobre embutidas.

---

## FRENTE 2 — Desempenho

### 2.0 Referência TCL **[MEDIDO]**

`adb screenrecord` + `ffmpeg tblend=difference,signalstats`. As DURAÇÕES são
confiáveis; as latências tecla→pixel são TETOS, porque `input keyevent`
acrescenta 50–300 ms próprios.

| Evento | Medida |
|---|---|
| `am start -W` frio | 1417 ms |
| Arranque → tela de perfis | ~1,8 s |
| Perfil → home com TEXTO e layout (esqueleto cinza) | **1,7 s** |
| → pôsteres preenchendo | 2,0 a 3,7 s |
| → arte do hero | 3,8 s |
| → quieto | 4,8 s |
| RIGHT numa fileira | 0,59 s, com crossfade do hero junto |
| DOWN (troca fileira) | 1,26 s, hero troca ~1,1 s |
| OK → detalhe | escurece 0,48 s, corte para esqueleto, preenche +0,15 a +0,55 s → **1,0–1,1 s** |
| BACK | 0,35 s |

Qualitativo: a TCL MOSTRA ESQUELETO IMEDIATAMENTE e preenche depois — nunca
fica parada. O hero segue o foco com crossfade dentro dos 0,6 s. O card focado
fica encostado à esquerda e a fileira desliza sob ele.

### 2.1 Arranque — a causa real da lentidão

`descoberta.c:montar` (773-905) roda **um fio, tudo em série, e publica só no
fim** (`cat_definir_tudo`, 896). Contagem exata pelo código com os addons do
dono:

1. `trakt_continuar` — 1 GET `/sync/playback` + 1 GET Cinemeta `/meta` POR ITEM
   (`trakt.c:51-88`), até 8 → **até 9 pedidos em série**, 20–25 s de timeout cada
2. `lerManifesto` × 3 addons → 3 pedidos
3. `lerCatalogo` por fileira (853-877) → **até 16 pedidos em série**, 25 s cada
4. `trakt_lista` watchlist + collection → 4 pedidos

**≈32 pedidos HTTP em série antes de a home mostrar UMA fileira de rede.**
E não há cache em disco do catálogo montado: só as imagens (`art/cache`). Cada
abertura refaz tudo. O Cinemeta e o Trakt não são mais rápidos na TCL — a
diferença é que ela mostra o que já tem.

**P1 — Cache do catálogo montado em disco.** `montar` grava
`art/catalogo-rede.txt` no fim; `cat_carregar` lê antes do estático do pacote.
A segunda abertura nasce com as fileiras de ontem em <100 ms. Verificação:
pedidos antes do primeiro quadro útil caem de ~32 para 0.

**P2 — Publicação incremental.** `cat_definir_tudo` após "Continuar assistindo"
e a cada fileira lida (a troca de bloco já é segura, catalogo.c:438-486).
CUIDADO: `home.c:sincronizarFileiras` chama `focus_iniciar` e ZERA O FOCO a cada
mudança de contagem — preservar `foco.fileira/coluna` comparando por `chave`,
senão o foco pula enquanto carrega. Verificação: foco não se move durante o
carregamento.

**P3 — Paralelizar.** Os 16 `lerCatalogo` são independentes; o padrão de 3 fios
já existe em `fioBusca` (descoberta.c:354-415). Os 8 `enfeitar` do Trakt idem.
Watchlist/collection depois da primeira publicação.

**P4 — Timeouts.** 25 s por catálogo e 20 s por meta, em série, viram minutos
com um addon fora do ar. Baixar para 8 s, como já foi feito para imagens.
Verificação: apontar um addon para host morto e medir ≤ 10 s.

### 2.2 Capas **[SUPOSTO]**

`tex_cache.c`: **o download acontece DENTRO do fio de decode** (`garantirLocal`
291, chamado de `threadDecode` 377). Com cache frio cada um dos 2 fios fica
bloqueado na rede até 8 s em vez de decodificar.

**P5 — Separar download de decode.** Fios só de rede alimentando a fila de
decode. Rede é I/O, não CPU: dá para ter 4 downloads sem competir com o desenho.

**P6 — Fila por visibilidade.** Hoje é FIFO. Prioridade = distância da fileira
ao foco.

**P7 — Esqueleto na home.** Garantir bloco cinza + rótulo quando `tex_obter`
devolve 0. É o que faz a TCL parecer pronta aos 1,7 s.

### 2.3 Resposta ao D-pad

Latência estrutural = 1 quadro de espera + 1 de apresentação ≈ 40 ms. Não é
"lerdo". O que é:

**P8 — Texto conta-gotas.** `TXT_POR_QUADRO 2`. `detail.c` tem 58 pontos de
chamada `txt_*`. Se o detalhe rasteriza 30 linhas novas, são 15 quadros =
**300 ms de texto aparecendo aos poucos** depois do OK. Pré-rasterizar as linhas
fixas na abertura com orçamento maior por 1 quadro, quando t≈0 e não há mais
nada custoso. Verificação: captura 100 ms após o OK já tem todo o texto.

**P9 — Hero com atraso composto.** `NV_HERO_REPOUSO_MS 220` + fade 330 + decode.
Na TCL o crossfade termina em 0,6 s contados da tecla. Pior: `home.c:75-80`
documenta "APAGAR → VAZIO → CORTE" — há um quadro VAZIO entre uma arte e outra;
a TCL faz crossfade sem vazio. Pedir a textura quando o item vira CANDIDATO, não
quando vira `heroAtual`.

**P10 — `txt_despejos` não sai no relatório.** Existe em `text.c` mas
`main.c:508-537` não imprime. Acrescentar. Deve ser 0 com a tela parada.

### 2.4 Animações — dt e molas **[MEDIDO por leitura completa]**

dt: TODAS as telas recebem o dt de `app_atualizar`, que vem de
`SDL_GetPerformanceCounter`. Nenhuma calcula dt próprio. Os `SDL_GetTicks`
restantes são temporizadores (hold, repouso do hero, toast) — uso correto.
**Nada a corrigir aqui.**

**P11 — Incoerência real de mola.** A home rola com `anim_mola2` (2ª ordem,
w=11,5, medido na referência). Detalhe, busca, biblioteca, ajustes e streams
rolam com `anim_mola` 1ª ordem k=8 — forma diferente, parte em velocidade
máxima. Migrar as cinco para `anim_mola2`, guardando velocidade ao lado de cada
`scroll*`.

Troca de tela (330 ms) é comparável à TCL (0,48 s abrindo, 0,35 s fechando).
**A lentidão não está na transição — está no conteúdo que chega depois.**

### 2.5 Abrir e tocar

**P12 — Fontes em série.** `addons.c:buscar` consulta os addons um por vez com
25 s de timeout; depois `stream_primeira_boa(8)` faz até 8 `rede_url_final` em
série com 20 s cada. Paralelizar os addons e verificar as 3 melhores juntas.

**P13 — Layout do detalhe pula** quando os dados chegam (`detail.c:1013-1017`
recalcula por quadro). Reservar altura das seções conhecidas.

### 2.6 Protocolo de medição na LG

1. Marcas `printf("[t] %u <evento>", SDL_GetTicks())` em: entrada do main,
   primeiro SwapWindow, `[desc] fileira 0`, catálogo montado, primeira fileira
   de texturas completa, `detail_abrir`, primeiro quadro sem texto pendente,
   `loadCompleted`.
2. Percurso fixo, com cache frio (apagar `art/cache`) e quente.
3. Rajada de capturas: `touch nuvio-shot-req` a cada 100 ms por 1 s após a
   tecla. É a única forma de medir tecla→pixel na LG.
4. Lado a lado com a TCL, mesma sequência da tabela 2.0.

### 2.7 Premissas conferidas

- "Com vídeo tocando a UI não desenha nada": confirmado (`player.c:811-812`).
- Cache de linhas em 512 e `txt_despejos`: confirmados, mas o contador não é
  reportado (P10).
- Nenhuma premissa de FPS / fill rate / 1062px foi contrariada.
- `FERRAMENTAS.md` está desatualizado: fala em `/tmp/nuvio-shot.png`, o código
  grava `.bmp` (`main.c:168`).

---

## Ordem por impacto na percepção de lentidão

1. P10 telemetria — pré-requisito para medir o resto
2. P1 cache do catálogo + P2 publicação incremental
3. P3/P4 paralelizar e encurtar timeouts
4. P8 texto conta-gotas na abertura do detalhe
5. P9 hero sem vazio e sem 220+330 ms
6. P5/P6 download fora dos fios de decode + prioridade por visibilidade
7. P12 fontes em paralelo
8. P11 mesma mola em todas as rolagens
9. P7/P13 esqueletos
10. Frente 1: F1.0 (prova na TV) → F1.1 → F1.2 → F1.3 → F1.4
