// Tokens visuais do port nativo. A referencia e o Nuvio 1.0.1 legacy (webOS):
// hero moderno, rail fixa e fileiras de posters. O prototipo Apple TV continua
// em outro build e nao define estes valores.
//
// Escala de tipografia: FATO da HIG do tvOS. Em 1080p, 1pt = 1px, entao os
// valores sao literais. Title1 76 / Title2 57 / Title3 48 / Headline 38 /
// Body 29 / Caption 25. Corpo nunca abaixo de 29.
#ifndef NV_LAYOUT_H
#define NV_LAYOUT_H

#define NV_TELA_W        1920.0f
#define NV_TELA_H        1080.0f

// O shell legacy usa uma rail de 72dp (144px no canvas 1080p) e inicia o
// conteúdo 104px depois dela, como no CSS .home-main + --home-content-start.
#define NV_LEGACY_RAIL_W        144.0f
#define NV_LEGACY_CONTENT_X     248.0f
#define NV_LEGACY_CONTENT_RIGHT 104.0f

// Scancode do BACK no SDL da LG (SDL_SCANCODE_WEBOS_BACK). Nao esta no
// SDL_scancode.h padrao, por isso vem como numero.
#define NV_SCANCODE_BACK 482
// Quanto tempo o OK precisa ficar pressionado para valer como pressao longa.
// 500ms e o limiar classico: mais curto dispara sem querer, mais longo parece
// que o botao nao respondeu.
#define NV_HOLD_MS       500
// fatia da fileira vizinha que fica visivel acima/abaixo da fileira em foco
#define NV_ESPIA_VIZINHA 0.10f

// Safe area: HIG pede >=60pt das bordas; overscan classico usa 80-90 nas
// laterais. Medido nas fotos de referencia: bate com 90.
// Safe area OFICIAL do tvOS: 80px nas laterais, 60px em cima e embaixo (HIG
// Layout). O valor 90 que estava aqui era chute meu.
#define NV_MARGEM_X      80.0f
#define NV_MARGEM_Y      60.0f

// No layout moderno legacy a arte ocupa a faixa superior (72% da largura e
// ~650px de altura); o viewport de fileiras permanece fixo nos 52% inferiores.
#define NV_HERO_H       650.0f
// Medido na foto do aparelho: a linha de botoes do hero termina a ~65% da
// altura, e a primeira fileira comeca logo abaixo, aparecendo cortada. Com a
// base em 150 os botoes desciam ate onde o cabecalho da fileira comeca, e os
// dois se sobrepunham.
// Medido na foto do aparelho: a linha de botoes do hero termina a ~69% da
// altura e o cabecalho da fileira vem ~100px depois. Com a base em 380 sobrava
// margem demais entre o bloco e os cards.
#define NV_HERO_BASE     570.0f
// A partir de quanta rolagem o bloco do hero comeca a sumir, e em quantos px
// ele desaparece por completo.
#define NV_HERO_FADE_INI 420.0f
#define NV_HERO_FADE_EXT 260.0f
#define NV_FUNDO_ESCURO   0.40f   // quanto o fundo da home escurece a arte
// A partir de quanta rolagem a ARTE do hero comeca a sumir, e em quantos px
// ela desaparece. Depois disso o que se ve e o fundo desfocado.
#define NV_HERO_ARTE_INI 300.0f
#define NV_HERO_ARTE_EXT 520.0f
#define NV_HERO_BOTAO_H   68.0f
#define NV_HERO_NBOTOES      3
// MEDIDO no app web: .home-modern-hero-media em x=555, y=0, 1421x670, com a
// arte em object-fit:cover. Os degrades que dissolvem a borda esquerda e a base
// estao no shader GFX_HERO, com as paradas anotadas la.
#define NV_HERO_ARTE_X   555.0f
#define NV_HERO_ARTE_W  1421.0f
#define NV_HERO_ARTE_H   670.0f
// MEDIDO no app web: .home-hero-logo ocupa 440x160 em x=248, y=135.
#define NV_LOGO_HERO_H   160.0f
#define NV_LOGO_HERO_MAX_W 440.0f
// Posicoes ABSOLUTAS do bloco de texto do hero, medidas no app web com a home
// no topo. Antes isto era ancorado na BASE (base = 1080 - NV_HERO_BASE) e
// empilhado para cima, que e como o app da Apple faz — o efeito colateral era o
// texto descer conforme a sinopse crescia e encostar no titulo da primeira
// fileira. No web cada linha tem lugar fixo:
//   logo      y=135  (h 160)
//   meta      y=327  (fonte 21, peso 500, rgb(179,179,179))
//   sinopse   y=411  (largura 640, fonte 22, h 89 em 2 linhas)
// e a fileira 0 comeca em 518, logo abaixo dos 500 onde a sinopse termina.
#define NV_HERO_LOGO_Y   135.0f
#define NV_HERO_META_Y   327.0f
#define NV_HERO_SIN_Y    411.0f
#define NV_HERO_SIN_W    640.0f
// MEDIDO no app web: o titulo da primeira fileira fica em y=518 e os cards em
// y=564. A regra dos "2/3 da tela" que estava aqui e do productTemplate do
// tvOS, e nao e a nossa: no web as fileiras sobem por cima da parte de baixo da
// arte do hero (que vai ate 670), em vez de comecarem depois dela.
#define NV_SHELF_TOP     518.0f   // topo do cabecalho da primeira fileira
#define NV_LEGACY_ROW_HEAD_H 46.0f // titulo + margem ate os cards (564 - 518)

// As quatro secoes visuais que a home do Apple TV usa, cada uma com proporcao
// propria — OBSERVADO nas fotos de referencia:
//  1. HERO      arte 16:9 full-bleed que TROCA sozinha (carrossel + dots)
//  2. CARD      landscape 16:9 comum, a fileira padrao
//  3. POSTER    retrato 2:3, usado no Top 10 ao lado do numeral
//  4. DESTAQUE  card grande com bloco de metadados embaixo ("Assista em seguida")
// Larguras OFICIAIS da tabela de grid do tvOS (somam 1760 = area util):
//   4 colunas -> 410   |   5 -> 320   |   6 -> 260   |   8 -> 184
// MEDIDO no app web rodando em 1920x1080 (getBoundingClientRect, nao leitura de
// CSS): .home-content-card = 212 x 322, primeiro card em x=248, segundo em
// x=520. Os valores anteriores vinham do grid do tvOS e erravam nos dois:
// altura 318 (o 212 x 1.5 "certinho" que o web nao usa) e gap 24.
#define NV_CARD_W        212.0f
#define NV_CARD_H        322.0f
// 520 - 248 = 272 de passo; menos os 212 do card, o gap e 60. Era 24 — menos da
// metade —, e e a diferenca que mais salta ao olhar as duas telas lado a lado:
// as fileiras do nativo pareciam apertadas.
#define NV_CARD_GAP      60.0f
// Passo entre fileiras MEDIDO: titulo da fileira 0 em y=518, da fileira 1 em
// y=934 -> 416. Desses, 46 sao do cabecalho ate os cards (518 -> 564) e 322 do
// card, sobrando 48 de respiro entre uma fileira e a proxima.
#define NV_FILEIRA_GAP  48.0f

#define NV_POSTER_W      212.0f
#define NV_POSTER_H      322.0f
// O Top 10 reserva espaco abaixo do poster para o rotulo de genero.
#define NV_TOP10_ROTULO   38.0f
#define NV_POSTER_NUM_W  118.0f   // faixa do numeral gigante do Top 10

// CORRIGIDO apos foto de referencia: no Apple TV os metadados do card grande
// ficam DENTRO da arte, sobrepostos na base sobre um veu escuro — nao abaixo
// dela. O logo do titulo aparece embutido na propria arte-chave.
// Medido por proporcao nas fotos: o card grande ocupa ~40% da largura da tela
// (~760px em 1920). E NAO e 16:9 — comparando a altura na foto do Apple TV, a
// proporcao fica perto de 3:2. Como nossa arte de origem e backdrop 16:9, o
// shader recorta (cover) em vez de esticar; sem isso a imagem deforma, que foi
// exatamente o defeito que apareceu na primeira tentativa.
#define NV_DESTAQUE_W    419.0f
#define NV_DESTAQUE_H    236.0f   // continue watching: 419 x 236
                                  // (16:9 -> 3:2 -> 4:3 -> +20%: cada passo foi
                                  //  comparado lado a lado na TV)

// Hero-carrossel: tempo em cada arte e duracao do crossfade.
#define NV_HERO_INTERVALO_MS  7000
#define NV_HERO_FADE_MS       900.0f
#define NV_HERO_DOT           9.0f
#define NV_HERO_DOT_GAP      14.0f

// Tipografia (px em 1080p)
// Escala tipografica do tvOS em 1080p (1pt = 1px). Os pesos vem do arquivo:
// a TV so tem Light e Regular da fonte LG, entao o negrito e sintetico.
#define NV_FT_TITULO1    76
#define NV_FT_TITULO2    57
#define NV_FT_TITULO3    48
#define NV_FT_HEADLINE   38
// Os corpos pequenos ficam ABAIXO da tabela do tvOS de proposito. A escala
// oficial pressupoe a SF Pro, e a Inter — que e a substituta possivel aqui — e
// visivelmente mais larga: no mesmo corpo, a mesma frase ocupou 336px contra
// 225px na captura do aparelho. Manter os numeros oficiais deixaria a mancha de
// texto de cada card metade maior que a do original. Os titulos ficam nos
// valores oficiais, onde a medida bateu (cap 40 contra 41).
#define NV_FT_BODY       25
#define NV_FT_CALLOUT    28
#define NV_FT_CAPTION    22
#define NV_FT_CAPTION2   21
#define NV_FT_MINI       15   // selo de classificacao (icone, nao texto)
// Corpos do PLAYER. Nao saem da escala do tvOS: saem do app web, que e a
// referencia desta variante. Os valores estao resolvidos para 1920x1080, que e
// onde o app roda — no CSS eles sao min(2.92vw,56px) e min(1.67vw,32px), e a
// TV cai sempre no teto. 56 nao vira TITULO2 (57) nem 32 vira HEADLINE (38)
// porque a diferenca aparece: o subtitulo em 38 empurra a barra de progresso
// para fora do lugar que o web reserva para ela.
// .home-row-title do web: 26px, peso 600. O nativo usava HEADLINE (38), e era
// isso que fazia o titulo da fileira invadir o card logo abaixo dele.
#define NV_FT_ROW_TITULO 26
#define NV_FT_PLR_TITULO 56   // .player-title
#define NV_FT_PLR_CORPO  32   // .player-subtitle e .player-time-label
// Entrelinha (leading) OFICIAL de cada estilo. Usar a altura que o SDL_ttf
// devolve nao e a mesma coisa: ela varia com os acentos da linha, entao um
// paragrafo fica com espacamento irregular linha a linha.
#define NV_LD_TITULO1    96
#define NV_LD_TITULO3    56
#define NV_LD_HEADLINE   46
#define NV_LD_BODY       32
#define NV_LD_CAPTION    29
#define NV_LD_CAPTION2   30

// Raios, em fracao do menor lado (o shader usa SDF normalizado)
#define NV_RAIO_CARD     0.055f
#define NV_RAIO_PILL     0.5f
#define NV_RAIO_BADGE    0.18f

// Cores (0..1). Fundo cinza-escuro, nunca preto puro — OBSERVADO nas fotos.
// Cinza neutro, levemente frio — o tom que a home do aparelho usa atras das
// fileiras. O quase-preto que estava aqui fazia os cards flutuarem no vazio.
#define NV_COR_FUNDO_R   0.145f
#define NV_COR_FUNDO_G   0.149f
#define NV_COR_FUNDO_B   0.161f

// Foco: escala 1.05-1.10x na HIG. Usamos 1.09 no card.
// Escala do foco DERIVADA das tabelas oficiais de Top Shelf, que publicam
// tamanho focado e nao focado: poster 2:3 e quadrado crescem ~14%, card 16:9
// cresce ~9%. Eu usava 9% para tudo, o que deixava o poster subdimensionado.
#define NV_FOCO_ESCALA   0.09f    // cards 16:9
#define NV_FOCO_ESCALA_P 0.14f    // posters 2:3 e circulos
// O item em foco tambem SOBE, nao so cresce: no tvOS ele se levanta em direcao
// ao espectador e a sombra cai por baixo. Sem o deslocamento, escala e sombra
// juntas leem como "a imagem inchou", nao como "esta item veio para frente".
#define NV_FOCO_LIFT      8.0f
// Sombra do item em foco. Numeros de reimplementacoes de terceiros do efeito
// do tvOS (a Apple nao publica os dela): raio 25px, deslocada 16px para baixo,
// preto a 30%. O deslocamento vertical importa mais do que parece — sombra
// centrada le como halo, sombra caida le como objeto levantado.
#define NV_FOCO_SOMBRA   25.0f
#define NV_SOMBRA_DY     16.0f
#define NV_SOMBRA_ALFA   0.30f

// Molas: rigidez usada em anim_mola(). ~250-350ms de assentamento, sem overshoot.
// Ganhar foco e mais rapido que perder: assimetria que a Apple declara no HIG
// ("focusing animations should be prominent, unfocusing subtler"). Com a mesma
// rigidez nos dois sentidos a navegacao fica com um peso uniforme que nao
// existe no aparelho.
#define NV_MOLA_FOCO     13.0f    // entrando no foco
#define NV_MOLA_DESFOCO   8.5f    // saindo dele
#define NV_MOLA_SCROLL    8.0f
#define NV_MOLA_TELA      9.0f

// Tela de detalhe: um CARTAO da arte cobrindo quase tudo, com a home aparecendo
// pela moldura. O voo do card usa NV_MOLA_TELA, mais lenta que a do foco de
// proposito: troca de tela e movimento maior, e na rigidez do foco viraria corte.
// Medido no video do app da Apple: o cartao central ocupa ~88% da largura e
// ~94% da altura. A margem LATERAL e grande de proposito — e por ela que os
// cartoes vizinhos aparecem, ~95px de cada lado. Com margem pequena o cartao
// vira tela cheia e o deslize deixa de parecer troca de cartao: parece troca de
// quadro de um filme, que foi exatamente o que o dono viu na primeira versao.
// MEDIDO por retificacao por homografia de um frame do aparelho (a tela da TV
// mapeada para 1920x1080 exatos; validacao: o centro do cartao caiu em 957 de
// 960 esperado). Cartao 1674x?? com margem lateral 120 e superior 38 — e ele
// NAO tem margem inferior: e cortado pela base da tela.
#define NV_DET_MARGEM_X  120.0f
#define NV_DET_MARGEM_Y   38.0f
#define NV_DET_PAD        44.0f   // medido: texto a 44px da borda do cartao
#define NV_DET_BOTAO_H    70.0f   // medido: pilula 254x70, raio = h/2
#define NV_DET_BASE      163.0f   // medido: fim do botao ate a base da tela
// Altura do logo do titulo. Bate com a altura de tinta medida na referencia
// (cap 83px), com folga para as letras que descem.
#define NV_LOGO_H        104.0f
#define NV_LOGO_MAX_W    620.0f
// No cabecalho da pagina o logo aparece menor que no cartao — ali ele e a
// etiqueta da tela, nao o protagonista.
#define NV_LOGO_CAB_H     62.0f
#define NV_LOGO_CAB_MAX_W 420.0f
// Logo dentro do card destaque da home, no tamanho do card sem foco. Ele cresce
// junto com o card, senao o titulo "descola" da arte ao ganhar foco.
#define NV_LOGO_CARD_H    54.0f
// Quanto a arte se ATRASA dentro da moldura durante o deslize, em fracao de
// textura. 0 = arte colada na moldura (parece um panorama unico passando);
// 0.12 = a janela corre por cima e a arte quase fica — o efeito do painel de
// feira em que a pessoa poe o rosto e o quadro troca.
#define NV_DET_PARALLAX  0.12f
// Quanto a arte cresce ao virar fundo da pagina esticada, e quanto escurece.
// Os dois juntos e que a transformam de foto em campo de cor.
#define NV_DET_ZOOM_FUNDO  1.35f
#define NV_DET_ESCURO_FUNDO 0.62f
// Nivel de mipmap amostrado no fundo da pagina: quanto maior, mais borrado.
// Medido: no fundo da pagina NENHUMA estrutura menor que ~250px sobrevive — e
// praticamente um gradiente de manchas. Bias 5.5 preservava detalhe demais.
#define NV_BLUR_PASSO       2.4f   // passo do gaussiano, em texels do alvo
// Teto de memoria das texturas. A TV tem cota, e a arte de verdade e grande:
// um backdrop 1920x1080 ocupa 8 MB depois de decodificado.
// 72 MB foi o teto posto depois de um "double free" — mas aquele estouro veio
// de o cache NAO TER teto nenhum (passava de 104 MB e crescia), nao de 96 ser
// demais. Com o catalogo dinamico sao ~48 titulos x 2 imagens, e a 72 o cache
// vivia encostado no limite (medido: 70,7 MB com 46 texturas), despejando e
// rebaixando sem parar — 12 a 15 janks por segundo durante a navegacao.
#define NV_TEX_ORCAMENTO_MB 96
// Secoes da pagina do titulo.
#define NV_ABA_W          236.0f   // medido
#define NV_ABA_H           63.0f   // medido; capsule (raio = h/2)
#define NV_ABA_PITCH      277.0f   // medido: texto a texto
// Medido: miniatura 410x228, texto ABAIXO dela, base da miniatura ao rotulo
// "EPISODIO n" 18px, e do fim do texto ao proximo cabecalho 143px.
#define NV_EP_H           512.0f   // miniatura + rotulo + titulo + 5 linhas + data
#define NV_EP_THUMB_GAP    18.0f
#define NV_AVATAR         168.0f
// Tracking: no tvOS ele e LEVEMENTE POSITIVO nos corpos pequenos (+0.4px) e
// praticamente zero nos titulos grandes — o oposto do reflexo de apertar
// titulos que vem do design web. O cabecalho da pagina e a excecao: ele e
// maiusculo e espacado de proposito, e da para ver isso na foto do aparelho.
#define NV_TRACKING_CAB     9.0f   // medido contra a captura do aparelho
// Espacamentos verticais MEDIDOS na pagina expandida.
// 64 e nao 84: o valor medido (84) e onde comeca a TINTA das maiusculas, e o
// desenho do texto parte do topo da caixa da linha, uns 20px acima disso.
#define NV_PG_TOPO         64.0f
#define NV_PG_TIT_ABAS     82.0f   // base do titulo ao topo das abas
#define NV_PG_SEC_CARDS    22.0f   // cabecalho de secao ao topo dos cards
#define NV_PG_ENTRE_SEC   143.0f   // fim de uma secao ao cabecalho da proxima
#define NV_ONDE_W         420.0f
#define NV_ONDE_H         106.0f
#define NV_SOBRE_H        150.0f
#define NV_DET_GAP        35.0f   // medido: gutter entre cartao e vizinho
// Quanto o cartao passa do tamanho final antes de assentar. Sem esse estouro a
// abertura parece "aparecer maior"; com ele, parece vir para a frente.
#define NV_DET_ESTOURO   0.035f

#endif
