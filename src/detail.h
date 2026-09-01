// Tela de detalhe do titulo.
//
// A transicao e o ponto: no Apple TV o card NAO some para dar lugar a uma tela
// nova — ele cresce ate virar o hero do detalhe, e o resto entra depois. Por
// isso detail_abrir recebe o retangulo de origem real do card na tela, e nao
// apenas o titulo: e esse rect que da continuidade ao movimento.
#ifndef NV_DETAIL_H
#define NV_DETAIL_H
#include <SDL2/SDL.h>
#include "home.h"

void detail_abrir(const HomeItem *item);
int  detail_aberto(void);        // 1 enquanto a tela existe, inclusive saindo
// 1 quando o cartao ja cobre a tela inteira e desenhar a home por baixo e
// trabalho jogado fora. Medido: a home custa o hero em tela cheia mais ~20
// cards, e sem este corte a pagina de detalhe rodava a 20fps.
int  detail_cobre_tela(void);
// 1 quando o cartao ja parou no lugar e nao esta esticado: nesse estado a home
// atras so aparece pela moldura.
int  detail_assentado(void);

// Qual titulo do catalogo esta em cena, e os pedidos que a tela nao resolve
// sozinha: reproduzir e marcar na lista. O detalhe nao chama o player nem a
// biblioteca direto — quem conhece as outras telas e o roteador.
int  detail_indice(void);
int  detail_pediu_reproduzir(void);   // consome o pedido
int  detail_pediu_marcar(void);       // botao "+"
int  detail_pediu_fontes(void);       // OK segurado, ou o botao "..."
// Botao secundario "Reproduzir desde o inicio", que so existe quando ha
// progresso. Hoje ele tambem marca `detail_pediu_reproduzir`, porque o roteador
// ainda nao sabe abrir o player ignorando o ponto salvo.
int  detail_pediu_do_inicio(void);
// --- Pagina do titulo (abaixo da dobra) -------------------------------------
// TUDO daqui para baixo foi medido no app web LOGADO, em 1920x1080, na serie
// "Silo" (getBoundingClientRect / getComputedStyle), em 2026-08-31. Os numeros
// nao vem do CSS: a folha declara `clamp(440px, 29vw, 540px)` para o card de
// episodio e o que a tela desenha e 640 — ler a folha erra por 100px.
//
// Vivem aqui e nao em layout.h porque layout.h esta sendo mexido por outros
// agentes nesta mesma sessao.
//
// O modelo e o do web: UM documento rolavel de 2144px de altura, do qual a tela
// mostra 1080. O hero ocupa 0..1080 e ROLA junto — nao ha cabecalho fixo, nem
// logo centralizado no topo (isso era do app da Apple TV). As secoes ficam em
// coordenadas ABSOLUTAS de documento, e rolar e so subtrair scrollY.
#define NV_DETP_X             96.0f   // gutter das fileiras (--tv-safe-gutter-wide)
#define NV_DETP_FIM         2144.0f   // fim do conteudo: elenco (2024) + 120 de padding
// Regra de rolagem do web, achada no fonte e conferida com quatro medidas:
// o topo do GRUPO focado vai para 33% da altura util (40% nas abas). Constantes
// DETAIL_ROW_FOCUS_TARGET / DETAIL_TAB_FOCUS_TARGET de metaDetailsScreen.js.
// Conferido: temporadas -> scrollTop 724, episodios -> 838, abas -> 1248,
// elenco -> 1393. Os quatro batem na casa do pixel.
#define NV_DETP_ALVO_FILEIRA  0.33f
#define NV_DETP_ALVO_ABAS     0.40f
// Vao entre itens de uma linha de meta. O flex declara 24 e o que se mede e 38
// (borda a borda) nas SEIS ocorrencias: genero->ponto, ponto->ano, ano->IMDb,
// selo->duracao, duracao->pais, pais->idioma. Mede-se, nao se le.
#define NV_DETP_SEP           38.0f

// Topo de cada GRUPO focavel, em coordenada de documento.
#define NV_DETP_G_TEMP      1080.0f
#define NV_DETP_G_EP        1194.0f
#define NV_DETP_G_ABAS      1680.0f
#define NV_DETP_G_ELENCO    1749.0f

// Abas de temporada: 269x80 em x=96, passo 321 (gap 52), raio 40, fonte 32/500.
// A largura sai do texto + padding, e nao e constante: "Especiais" mede 219.
#define NV_DETP_TEMP_Y      1100.0f
#define NV_DETP_TEMP_H        80.0f
#define NV_DETP_TEMP_PADX     40.0f
#define NV_DETP_TEMP_GAP      52.0f

// Episodio: card 640x422 em x=96, passo 726, raio 32. A diferenca estrutural
// com o port anterior (que era do app da Apple TV) e que o TEXTO FICA DENTRO da
// miniatura, sobre um degrade vertical, e nao abaixo dela.
#define NV_DETP_EP_Y        1226.0f
#define NV_DETP_EP_W         640.0f
#define NV_DETP_EP_H         422.0f
#define NV_DETP_EP_PASSO     726.0f
#define NV_DETP_EP_THUMB_H   414.0f
#define NV_DETP_EP_RAIO       32.0f
#define NV_DETP_EP_PAD        32.0f   // margem do texto dentro da miniatura
#define NV_DETP_EP_TEXTO_W   576.0f
#define NV_DETP_EP_SELO_Y    126.0f   // "EPISODIO n", 163x44, raio 12
#define NV_DETP_EP_SELO_H     44.0f
#define NV_DETP_EP_SELO_PADX  16.0f
#define NV_DETP_EP_TIT_Y     190.0f   // 32/800, lh 44
#define NV_DETP_EP_SIN_Y     254.0f   // 28/400 rgba(255,255,255,.9), lh 36
#define NV_DETP_EP_LD_SIN     36.0f
#define NV_DETP_EP_META_Y    346.0f   // relogio + duracao + data, 20/400
#define NV_DETP_EP_ICONE      28.0f
#define NV_DETP_EP_BARRA_Y   390.0f   // barra 576x8, raio 999
#define NV_DETP_EP_BARRA_H     8.0f
#define NV_DETP_EP_STATUS     48.0f   // circulo tracejado de "nao assistido"
// Anel de foco. O web usa 2px na miniatura do episodio e 4px nos botoes do
// hero; aqui fica 4 nos dois, porque a folha declara pixels de CSS e a tela
// desta pagina roda com um fator de escala de ~1.6 sobre os `clamp` — ou seja,
// o anel de 2px do episodio chega perto de 3px reais, e 4 e o valor inteiro que
// mais se aproxima sem sumir na TV.
#define NV_DETP_ANEL           4.0f

// Abas "Criador e elenco | Avaliacoes | Mais como este | Trailer": fonte 32/500,
// selecionada branca, as outras #808080; o divisor "|" e 32/700 #808080, com 20
// de folga de cada lado.
#define NV_DETP_ABA_Y       1698.0f
#define NV_DETP_ABA_H         51.0f
#define NV_DETP_ABA_SEP       20.0f

// Elenco: card de 220 de largura, passo 270; avatar 140x140 ALINHADO A
// ESQUERDA do card (nao centralizado); nome 26/500 rgb(179,179,179) e papel
// 21/400 rgb(128,128,128) abaixo.
#define NV_DETP_EL_Y        1757.0f
#define NV_DETP_EL_W         220.0f
#define NV_DETP_EL_PASSO     270.0f
#define NV_DETP_EL_AVATAR    140.0f
#define NV_DETP_EL_NOME_DY    10.0f   // base do avatar -> topo do nome
#define NV_DETP_EL_PAPEL_DY   43.0f   // topo do nome -> topo do papel

// Selos da pilha de meta do HERO, medidos na mesma sessao.
#define NV_DETW_IMDB_W       109.0f   // logo 60x60 + folga + nota 20.7/400
#define NV_DETW_IMDB_H        60.0f
#define NV_DETW_SELO_H        45.0f   // .detail-meta-badge, raio 8, borda 1px
#define NV_DETW_SELO_PADX     10.0f
// Botao secundario "Reproduzir desde o inicio": 345x96, raio 64, fundo #222,
// texto branco; focado vira #f5f5f5 com texto #111 e o anel de 4px.
#define NV_DETW_BTN2_PADX     34.0f

void detail_evento(const SDL_Event *e);
void detail_atualizar(float dt, Uint32 agora);
void detail_desenhar(Uint32 agora);   // desenhe DEPOIS da home: ele cobre

#endif
