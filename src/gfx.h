// Camada de desenho: um shader unico com SDF de retangulo arredondado, capaz de
// desenhar card com textura, sombra difusa, retangulo de cor e o hero com
// gradiente. Um so programa GL evita troca de estado a cada primitiva, que e o
// que mais custa nesta GPU.
#ifndef NV_GFX_H
#define NV_GFX_H
#include "gl_compat.h"

typedef enum {
  GFX_CARD   = 0,  // textura com cantos, parallax e especular
  GFX_SOMBRA = 1,  // sombra difusa atras do card focado
  GFX_COR    = 2,  // retangulo/pill de cor solida
  GFX_HERO   = 3,  // arte com gradiente para o fundo (aceita alpha p/ crossfade)
  GFX_VEU    = 4,  // veu escuro na base do card, sob texto sobreposto
  GFX_TEXTO  = 5,  // glifo: a forma vem do alpha da textura, nao do RGB
  GFX_FUNDO  = 6,  // arte desfocada por mipmap; `foco` carrega o bias
  GFX_VEU_TOPO = 7,// degrade escuro do topo para baixo (barra de cabecalho)
  GFX_SNAP   = 8,  // imagem ja pronta: sem SDF, sem efeito, so o quad
  GFX_PLAY   = 9,  // triangulo de "reproduzir", apontando para a direita
  GFX_BLUR   = 10, // uma passada de desfoque gaussiano; uPar da a direcao
  GFX_NMODOS = 11
} GfxModo;

typedef struct {
  float x, y, w, h;
} GfxRect;

// Proporcao (w/h) da textura a desenhar. 0 = mapeia direto (texto, veu).
// Definir ANTES de gfx_rect para que a arte seja recortada, nunca esticada.
extern float gfx_tex_aspect_atual;

// Snapshot: renderiza uma tela inteira para textura, para poder redesenha-la
// como um unico quad. Existe porque a home continua visivel pela moldura da
// tela de detalhe, e redesenhar hero + ~20 cards a cada quadro so para preencher
// uma borda de 120px derrubava o app para 30fps. A home nao muda enquanto o
// detalhe esta aberto, entao basta guardar a imagem dela.
//
// A meia resolucao e deliberada: o snapshot aparece escurecido e so nas bordas,
// e ninguem distingue — mas o custo de preenchimento cai a um quarto.
// Recorte de tesoura: limita o desenho a um retangulo da tela. Serve para
// pintar so a parte que aparece — no detalhe, o cartao cobre o centro e a home
// atras so e vista pela moldura, entao pintar a tela inteira por baixo dele e
// trabalho jogado fora.
void gfx_recorte(float x, float y, float w, float h);
void gfx_sem_recorte(void);

int  gfx_snap_iniciar(int w, int h);

// Desfoque por REDUCAO, no lugar do mipmap. O mipmap parecia a saida barata,
// mas as texturas de arte nao tem lado potencia de dois, e em GLES2 a piramide
// de uma textura NPOT e mal definida — o resultado era um padrao de listras
// verticais no fundo da pagina, bem visivel contra a referencia. Aqui a arte e
// desenhada num alvo minusculo e depois esticada com filtro linear: o borrao
// sai liso e custa uma leitura por pixel.
// `via` escolhe o alvo: 0 = pagina de detalhe, 1 = fundo da home. Dois alvos
// porque as duas telas coexistem — o detalhe cobre a home, mas a home continua
// desenhada por tras dele, e um alvo so faria as duas brigarem pela mesma
// textura a cada quadro.
int  gfx_borrao_iniciar(int w, int h);
void gfx_borrao_gerar(int via, unsigned int tex, float texAspecto);
void gfx_borrao_desenhar(int via, GfxRect r, float alpha);
void gfx_borrao_encerrar(void);
void gfx_snap_comecar(void);   // redireciona o desenho para o snapshot
void gfx_snap_terminar(void);  // volta para a tela
void gfx_snap_desenhar(void);  // pinta o snapshot ocupando a tela toda
void gfx_snap_encerrar(void);

void gfx_tamanho_alvo(int w, int h);   // drawable real, para restaurar viewport
int  gfx_iniciar(void);
void gfx_encerrar(void);

// Desenha um retangulo. `foco` 0..1 controla especular/sombra; `parx/pary`
// deslocam a arte dentro do card (parallax); `raio` em fracao do menor lado.
void gfx_rect(GfxRect r, GLuint tex, GfxModo modo, float foco,
              float parx, float pary, float raio,
              float cr, float cg, float cb, float ca);

// Atalhos legiveis para os casos comuns.
void gfx_cor(GfxRect r, float raio, float cr, float cg, float cb, float ca);
// Zera cor E alpha do retangulo, com blend desligado, abrindo a superficie para
// o plano de video que fica atras dela. Ver video.h.
void gfx_furo(GfxRect r);
void gfx_textura(GfxRect r, GLuint tex);

#endif
