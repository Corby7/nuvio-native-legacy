// Texto: SDL_ttf rasteriza para textura, com cache por (fonte,tamanho,string).
// Sem cache, cada quadro rasterizaria os mesmos titulos de fileira de novo —
// rasterizacao de texto e cara e o conteudo aqui muda pouco.
#ifndef NV_TEXT_H
#define NV_TEXT_H
#include "gl_compat.h"

// Escala do tvOS. Cada estilo carrega tamanho E peso: no aparelho a diferenca
// entre um titulo e um subtitulo vem tanto do peso quanto do corpo, e usar um
// peso so achata a hierarquia inteira — foi o que deixava a tela com cara de
// "tudo do mesmo tamanho, uns maiores".
typedef enum {
  TXT_TITULO1, TXT_TITULO2, TXT_TITULO3, TXT_HEADLINE,
  TXT_BODY, TXT_CALLOUT, TXT_CAPTION, TXT_CAPTION2, TXT_MINI,
  // Os dois do player vem do app web, nao da escala do tvOS. Ficam no FIM do
  // enum de proposito: a tabela ESTILOS em text.c e indexada por esta ordem, e
  // inserir no meio desloca todos os estilos seguintes em silencio.
  TXT_PLR_TITULO, TXT_PLR_CORPO, TXT_ROW_TITULO, TXT_HERO_SEC,
  // Tela de DETALHE, medidos no app web. Nao reaproveitam nenhum estilo do
  // tvOS porque nenhum bate: a sinopse la e 26/400 e o TXT_CAPTION daqui e
  // 22/400 — quatro pixels que mudam quantas linhas cabem no bloco.
  TXT_DET_BOTAO,   // .series-primary-btn      25 / 600
  TXT_DET_META,    // .series-detail-support   25 / 400
  TXT_DET_SIN,     // .series-detail-description 26 / 400
  TXT_DET_META2,   // .detail-meta-row.secondary 23 / 400
  // Linha de meta do HERO: 21 / 500, rgb(179,179,179). Nao e o TXT_CAPTION
  // (22/400) nem o TXT_CALLOUT (28/500) — um erra o peso, o outro o corpo, e a
  // linha ficava ou apagada demais ou grossa demais contra a arte.
  TXT_HERO_META,
  // Sinopse do hero: .home-hero-description, 22/400 branco cheio. O TXT_CAPTION
  // tem o mesmo corpo mas e cinza — a cor vem de quem desenha, o estilo nao.
  TXT_HERO_SIN,
  // Canto superior do PLAYER, do bloco #playerUiRoot do web:
  //   .player-clock          26 / 600
  //   .player-ends-at        20 / 400
  //   .player-parental-label 22 / 600
  //   .player-parental-severity e .player-parental-separator 22 / 400
  TXT_PG_RELOGIO, TXT_PG_FIM, TXT_PG_ROTULO, TXT_PG_GRAV,
  TXT_NFONTES
} TxtEstilo;

typedef struct { GLuint tex; int w, h; } TxtLinha;

// Instrumentacao: quantas linhas foram RASTERIZADAS (nao vieram do cache) no
// quadro e quanto tempo isso custou. Rasterizar texto e a operacao mais cara
// que acontece dentro de um quadro, e sem contador nao da para saber se um
// jank veio dai ou do upload de textura.
extern int    txt_rasterizadas;
extern double txt_ms;

// `dirRecursos` e a pasta que contem fonts/. No aparelho e a pasta do app; no
// Mac, a pasta do pacote — sem esse parametro a fonte so era procurada ao lado
// do executavel, e rodar local caia direto no fallback.
// `escala` e a razao entre o buffer e o canvas de layout (2 numa TV 4K, 1 em
// 1080p). As fontes sao abertas nesse tamanho e a linha devolvida continua
// medindo em unidades de layout — ver text.c.
int  txt_iniciar(const char *dirRecursos, float escala);
void txt_encerrar(void);

// Devolve linha cacheada. Cor em 0..255. Nunca devolve NULL; em falha, w/h = 0.
// Zera o orcamento de rasterizacao do quadro. Chamar uma vez por quadro, antes
// de desenhar; sem isso o orcamento se esgota e o texto some.
void txt_novo_quadro(void);

TxtLinha txt_linha(TxtEstilo estilo, const char *s, int r, int g, int b, int a);

// Linha que NUNCA passa de `maxW`: corta por palavra (ou por caractere, se uma
// palavra so ja estourar) e fecha com "…". Conteudo que vem de fora (nome de
// addon, genero do TMDB) nao tem comprimento garantido, e sem corte ele invade
// a coluna vizinha — foi o que apareceu no Top 10 e na folha de faixas.
TxtLinha txt_linha_corta(TxtEstilo estilo, const char *s, int r, int g, int b,
                         int a, float maxW);

// Desenha no canto superior esquerdo (x,y).
void txt_desenhar(TxtLinha l, float x, float y);
void txt_desenhar_alpha(TxtLinha l, float x, float y, float alpha);

// Desenha com ESPACAMENTO entre letras (tracking) e devolve a largura total.
// SDL_ttf nao tem tracking, e o titulo da pagina do tvOS depende dele: sem o
// espacamento largo o mesmo texto em maiusculas fica com cara de grito, nao de
// cabecalho. Passe x = -1 para so medir, sem desenhar.
float txt_tracking(TxtEstilo estilo, const char *s, int r, int g, int b,
                   float x, float y, float alpha, float tracking);

// Desenha texto QUEBRADO em linhas que cabem em `larg`, devolvendo a altura
// usada. Sem isso, qualquer texto de tamanho variavel (sinopse de episodio,
// nome de titulo) vaza para a coluna vizinha — nao existe "escrever curto o
// suficiente" quando o conteudo vem de fora.
float txt_bloco(TxtEstilo estilo, const char *s, int r, int g, int b,
                float x, float y, float larg, float leading, float alpha, int maxLinhas);

// Mesmo bloco, mas ALINHADO A DIREITA: cada linha termina em `xDir`. Os
// creditos do canto inferior direito precisam disso — alinhados a esquerda,
// eles ficam com a borda picotada contra a margem do cartao.
float txt_bloco_dir(TxtEstilo estilo, const char *s, int r, int g, int b,
                    float xDir, float y, float larg, float leading,
                    float alpha, int maxLinhas);

#endif
