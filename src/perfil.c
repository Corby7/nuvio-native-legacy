// Perfil e Stats: relatorio editorial escuro, pensado para leitura a 3 metros.
//
// A composicao evita uma grade de cards identicos. O resumo e tipografico, o
// streak e um calendario, generos viram uma faixa proporcional e os titulos
// mais vistos ganham paineis largos com arte. O acento violeta pertence aos
// dados, enquanto o foco continua branco como no restante do native legacy.
#include "perfil.h"
#include "anim.h"
#include "gfx.h"
#include "layout.h"
#include "text.h"
#include "tex_cache.h"
#include "ajustes.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PF_X             ajustes_conteudo_x()
#define PF_W            (NV_TELA_W-PF_X-104.0f)
#define PF_TOPO           62.0f
#define PF_RESUMO_Y       92.0f
#define PF_ATIV_Y        500.0f
#define PF_GENEROS_Y    1050.0f
#define PF_DESTAQUES_Y  1370.0f
#define PF_DOC_H        1940.0f
#define PF_SECOES          4
#define PF_CARD_W        724.0f
#define PF_CARD_H        344.0f
#define PF_CARD_GAP       28.0f

static const float SECAO_Y[PF_SECOES] = {
  PF_RESUMO_Y, PF_ATIV_Y, PF_GENEROS_Y, PF_DESTAQUES_Y
};
static const uint32_t PALETA[PERFIL_MAX_GENEROS] = {
  0xA84BD6, 0x3C9FE8, 0xC57BE3, 0x5CB8F2,
  0x71338E, 0x235B79, 0xD6A1E8, 0x777780
};

static PerfilDados dados;
static int aberto, sair, carregando, temDados;
static int secao, item, escolhido = -1;
static int dia, pedirAtualizar;
static int lateral, completo, lateralFoco;
static char erro[160];
static float entrada, scroll, scrollAlvo, velScroll;
static float scrollX, velX;
static float focoSec[PF_SECOES], focoItem[PERFIL_MAX_DESTAQUES];

static int limitar(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float fmax0(float v) { return v > 0.0f ? v : 0.0f; }
static void rgb(uint32_t c, float *r, float *g, float *b) {
  *r = ((c >> 16) & 255) / 255.0f;
  *g = ((c >> 8) & 255) / 255.0f;
  *b = (c & 255) / 255.0f;
}
static void texto(TxtEstilo e, const char *s, int cor, float x, float y, float a) {
  txt_desenhar_alpha(txt_linha(e, s ? s : "", cor, cor, cor, 255), x, y - scroll, a);
}
static void textoC(TxtEstilo e, const char *s, int r, int g, int b,
                   float x, float y, float a) {
  txt_desenhar_alpha(txt_linha(e, s ? s : "", r, g, b, 255), x, y - scroll, a);
}
static void numero(char *s, size_t n, int v) { snprintf(s, n, "%d", v < 0 ? 0 : v); }
static void tempo(char *s, size_t n, int minutos) {
  if (minutos < 0) minutos = 0;
  if (minutos < 60) snprintf(s, n, "%d min", minutos);
  else snprintf(s, n, "%dh %02dmin", minutos / 60, minutos % 60);
}
static void anel(GfxRect r, float raio, float a) {
  float menor = r.w < r.h ? r.w : r.h;
  gfx_rect(r, 0, GFX_ANEL, 0, NV_ANEL_FOCO/menor, 0, raio, 0.96f, 0.96f, 0.98f, a);
}
static int visivel(float y, float h) { return y+h-scroll>=0 && y-scroll<NV_TELA_H; }
static void corta(TxtEstilo e,const char *s,int c,float x,float y,float w,float a) {
  txt_desenhar_alpha(txt_linha_corta(e,s,c,c,c,255,w),x,y-scroll,a);
}

int perfil_iniciar(void) {
  memset(&dados, 0, sizeof(dados));
  aberto = sair = temDados = carregando = 0;
  secao = item = 0; escolhido = -1;
  dia = pedirAtualizar = 0; erro[0] = 0; scrollX = velX = 0;
  entrada = scroll = scrollAlvo = velScroll = 0;
  memset(focoSec, 0, sizeof(focoSec));
  memset(focoItem, 0, sizeof(focoItem));
  return 1;
}
void perfil_encerrar(void) { perfil_iniciar(); }
void perfil_abrir(void) {
  lateral = completo = 0;
  aberto = 1; sair = 0; escolhido = -1; secao = item = 0;
  scroll = scrollAlvo = velScroll = 0;
  scrollX = velX = 0; pedirAtualizar = 0;
}
void perfil_abrir_lateral(void) { perfil_abrir(); lateral=1; lateralFoco=0; }
int perfil_lateral(void) { return lateral && (aberto || entrada>.002f); }
int perfil_pediu_completo(void) { int v=completo;completo=0;return v; }
void perfil_fechar(void) { aberto = 0; sair = 1; }
int perfil_aberto(void) { return aberto; }
int perfil_quer_sair(void) { int q = sair; sair = 0; return q; }
void perfil_definir_carregando(int v) { carregando = !!v; }
void perfil_definir_erro(const char *m) {
  snprintf(erro,sizeof(erro),"%s",m&&m[0]?m:"Não foi possível atualizar o histórico.");
  carregando=0;
}
int perfil_pediu_atualizar(void) { int p=pedirAtualizar; pedirAtualizar=0; return p; }

void perfil_definir_dados(const PerfilDados *d) {
  if (!d) {
    memset(&dados,0,sizeof(dados));temDados=carregando=0;
    secao=item=dia=0;scroll=scrollAlvo=velScroll=scrollX=velX=0;
    escolhido=-1;erro[0]=0;return;
  }
  dados = *d;
  // O produtor pode preencher buffers fixos ate o ultimo byte. Fechar todos
  // aqui mantem as chamadas de texto e de textura seguras mesmo com payload
  // truncado vindo da rede.
  dados.nome[sizeof(dados.nome)-1] = 0;
  dados.usuario[sizeof(dados.usuario)-1] = 0;
  dados.avatar[sizeof(dados.avatar)-1] = 0;
  dados.periodo[sizeof(dados.periodo)-1] = 0;
  dados.aviso[sizeof(dados.aviso)-1] = 0;
  if(dados.minutos<0)dados.minutos=0;
  if(dados.plays<0)dados.plays=0;
  if(dados.filmes<0)dados.filmes=0;
  if(dados.episodios<0)dados.episodios=0;
  // Calendario mensal: no maximo 31 dias, mesmo que o array tenha 42 slots.
  dados.nDias = limitar(dados.nDias, 0, 31);
  dados.primeiroDiaSemana = limitar(dados.primeiroDiaSemana, 0, 6);
  dados.nGeneros = limitar(dados.nGeneros, 0, PERFIL_MAX_GENEROS);
  dados.nDestaques = limitar(dados.nDestaques, 0, PERFIL_MAX_DESTAQUES);
  for (int i=0; i<dados.nGeneros; i++) {
    dados.generos[i].nome[sizeof(dados.generos[i].nome)-1] = 0;
    if(dados.generos[i].quantidade<0)dados.generos[i].quantidade=0;
  }
  for (int i=0; i<dados.nDestaques; i++) {
    PerfilDestaque *p=&dados.destaques[i];
    p->id[sizeof(p->id)-1]=0; p->titulo[sizeof(p->titulo)-1]=0;
    p->detalhe[sizeof(p->detalhe)-1]=0; p->poster[sizeof(p->poster)-1]=0;
    p->backdrop[sizeof(p->backdrop)-1]=0;
  }
  temDados = dados.minutos > 0 || dados.plays > 0 || dados.filmes > 0 ||
             dados.episodios > 0 || dados.nDestaques > 0;
  carregando = 0;
  erro[0]=0; escolhido=-1;
  dia=limitar(dia,0,dados.nDias?dados.nDias-1:0);
  if(!temDados){secao=0;scroll=scrollAlvo=velScroll=0;}
  if (item >= dados.nDestaques) item = dados.nDestaques ? dados.nDestaques - 1 : 0;
}

int perfil_item_selecionado(PerfilDestaque *saida) {
  if (escolhido < 0 || escolhido >= dados.nDestaques) return 0;
  if (saida) *saida = dados.destaques[escolhido];
  escolhido = -1;
  return 1;
}

void perfil_evento(const SDL_Event *e) {
  if (!aberto || !e || e->type != SDL_KEYDOWN) return;
  SDL_Keycode k = e->key.keysym.sym;
  if(lateral) {
    if(k==SDLK_ESCAPE || k==SDLK_AC_BACK || k==SDLK_BACKSPACE || k==SDLK_LEFT) {perfil_fechar();return;}
    if(k==SDLK_UP && lateralFoco>0)lateralFoco--;
    if(k==SDLK_DOWN && lateralFoco<2)lateralFoco++;
    if(k==SDLK_RETURN || k==SDLK_KP_ENTER) {
      if(lateralFoco==0)perfil_fechar();
      else if(lateralFoco==1){if(!carregando)pedirAtualizar=1;}
      else {completo=1;aberto=0;}
    }
    return;
  }
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE || k == SDLK_DELETE) {
    perfil_fechar(); return;
  }
  if(k==SDLK_r || ((!temDados || erro[0]) &&
     (k==SDLK_RETURN || k==SDLK_KP_ENTER))) {
    if(!carregando)pedirAtualizar=1;
    return;
  }
  if (!temDados) return;
  // O calendario recebe o D-pad real, com continuidade entre semanas. Sair
  // pela primeira/ultima semana devolve a navegacao para as secoes.
  if(secao==1 && dados.nDias>0) {
    if(k==SDLK_LEFT && dia>0){dia--;return;}
    if(k==SDLK_RIGHT && dia+1<dados.nDias){dia++;return;}
    if(k==SDLK_UP && dia>=7){dia-=7;return;}
    if(k==SDLK_DOWN && dia+7<dados.nDias){dia+=7;return;}
  }
  if (k == SDLK_LEFT && (secao!=3 || item==0)) { perfil_fechar(); return; }
  if (k == SDLK_UP && secao > 0) secao--;
  else if (k == SDLK_DOWN && secao < PF_SECOES - 1) {
    secao++;
    if(secao==1)dia=0;
  }
  else if (secao == PF_SECOES - 1 && k == SDLK_LEFT && item > 0) item--;
  else if (secao == PF_SECOES - 1 && k == SDLK_RIGHT && item + 1 < dados.nDestaques) item++;
  else if (secao == PF_SECOES - 1 && (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE)) {
    if (dados.nDestaques > 0) escolhido = item;
  }
}

void perfil_atualizar(float dt, Uint32 agora) {
  (void)agora;
  int reduzida=ajustes_animacoes_reduzidas();
  entrada = anim_mola(entrada, aberto ? 1.0f : 0.0f, dt, NV_MOLA_TELA);
  if (!aberto && entrada < 0.002f) entrada = 0;
  float alvo = SECAO_Y[secao] - (secao == 0 ? PF_RESUMO_Y : 120.0f);
  float maxScroll = fmax0(PF_DOC_H - NV_TELA_H + 54.0f);
  scrollAlvo = anim_clamp(alvo, 0, maxScroll);
  scroll = anim_mola2(&velScroll, scroll, scrollAlvo, dt, NV_MOLA2_SCROLL);
  float alvoX=item*(PF_CARD_W+PF_CARD_GAP);
  scrollX=anim_mola2(&velX,scrollX,alvoX,dt,NV_MOLA2_SCROLL);
  if(reduzida) { entrada=aberto?1:0;scroll=scrollAlvo;velScroll=0;scrollX=alvoX;velX=0; }
  for (int i = 0; i < PF_SECOES; i++)
    focoSec[i] = anim_mola(focoSec[i], i == secao ? 1.0f : 0.0f, dt, NV_MOLA_FOCO);
  for (int i = 0; i < PERFIL_MAX_DESTAQUES; i++)
    focoItem[i] = anim_mola(focoItem[i], secao == 3 && i == item ? 1.0f : 0.0f,
                            dt, NV_MOLA_FOCO);
  if(reduzida) {
    for(int i=0;i<PF_SECOES;i++)focoSec[i]=(i==secao);
    for(int i=0;i<PERFIL_MAX_DESTAQUES;i++)focoItem[i]=(secao==3&&i==item);
  }
}

static void tituloSecao(const char *s, int qual, float y, float a) {
  float f = focoSec[qual];
  GfxRect ponto = { PF_X, y + 15 - scroll, 11, 11 };
  gfx_cor(ponto, .5f, .54f + .12f*f, .24f + .08f*f, .72f + .16f*f, a);
  texto(TXT_TITULO3, s, 244, PF_X + 28, y, a);
  if (f > .02f) {
    const char *ajuda=qual==1?"Setas: dias  ·  Voltar: menu":
       qual==3?"Esquerda/direita: títulos  ·  OK: detalhes":"Cima/baixo: seções  ·  Voltar: menu";
    TxtLinha d = txt_linha(TXT_MINI, ajuda, 185,185,194,255);
    txt_desenhar_alpha(d, PF_X + PF_W - d.w, y + 11 - scroll, a * f * .72f);
  }
}

static void desenharLoading(Uint32 agora, float a) {
  float pulso = ajustes_animacoes_reduzidas()?.55f:.48f+.12f*sinf((float)agora*.004f);
  tituloSecao("Perfil e Stats", 0, PF_TOPO, a);
  gfx_cor((GfxRect){PF_X,144-scroll,510,40},.18f,.18f,.18f,.20f,pulso*a);
  gfx_cor((GfxRect){PF_X,214-scroll,930,92},.08f,.18f,.18f,.20f,pulso*a);
  for (int i=0;i<4;i++)
    gfx_cor((GfxRect){PF_X+i*274,342-scroll,230,58},.15f,.18f,.18f,.20f,pulso*a);
  gfx_cor((GfxRect){PF_X,PF_ATIV_Y+58-scroll,PF_W*.57f,432},.035f,.18f,.18f,.20f,pulso*a);
  gfx_cor((GfxRect){PF_X,PF_GENEROS_Y+58-scroll,PF_W,176},.05f,.18f,.18f,.20f,pulso*a);
  for (int i=0;i<2;i++)
    gfx_cor((GfxRect){PF_X+i*(PF_CARD_W+PF_CARD_GAP),PF_DESTAQUES_Y+62-scroll,PF_CARD_W,PF_CARD_H},
            .035f,.18f,.18f,.20f,pulso*a);
}

static void desenharVazio(float a) {
  tituloSecao("Perfil e Stats", 0, PF_TOPO, a);
  corta(TXT_TITULO2, erro[0]?"Seu histórico está indisponível":"Nenhuma reprodução neste período",244,PF_X,248,PF_W,a);
  txt_bloco(TXT_BODY,
    erro[0]?erro:"Conecte o Trakt e assista a um filme ou episódio. Seu resumo usa somente o histórico disponível.",
    177,179,187, PF_X, 326-scroll, 780, NV_LD_BODY, a, 3);
  GfxRect btn={PF_X,452-scroll,330,72};
  gfx_cor(btn,.24f,NV_COR_FOCO_R,NV_COR_FOCO_G,NV_COR_FOCO_B,a);
  anel(btn,.24f,a);
  texto(TXT_DET_BOTAO,"OK · Tentar novamente",240,PF_X+24,472,a);
  texto(TXT_CAPTION,"Voltar retorna ao menu",182,PF_X,554,a);
}

static void desenharResumo(float a) {
  if(!visivel(PF_RESUMO_Y,400))return;
  char b[64], t[64];
  tituloSecao("Resumo mensal", 0, PF_RESUMO_Y, a);
  textoC(TXT_CAPTION, dados.periodo[0] ? dados.periodo : "ESTE MÊS",
         184,112,220, PF_X, PF_RESUMO_Y+64, a);
  tempo(t,sizeof(t),dados.minutos);
  snprintf(b,sizeof(b),"%s assistidos",t);
  corta(TXT_TITULO1,b,246,PF_X,PF_RESUMO_Y+102,PF_W*.52f,a);

  const char *rot[4]={"REPRODUÇÕES","FILMES","EPISÓDIOS","DIAS ATIVOS"};
  int val[4]={dados.plays,dados.filmes,dados.episodios,dados.diasAtivosMes};
  float x=PF_X+PF_W*.58f;
  for(int i=0;i<4;i++) {
    float xx=x+(i%2)*PF_W*.22f, yy=PF_RESUMO_Y+62+(i/2)*104;
    numero(b,sizeof(b),val[i]); texto(TXT_TITULO2,b,244,xx,yy,a);
    textoC(TXT_MINI,rot[i],166,168,178,xx,yy+48,a);
  }
  if (dados.nome[0] || dados.usuario[0]) {
    corta(TXT_BODY,dados.nome[0]?dados.nome:dados.usuario,224,PF_X,PF_RESUMO_Y+208,PF_W*.52f,a);
    if(dados.usuario[0]) { snprintf(b,sizeof(b),"@%s",dados.usuario);
      corta(TXT_CAPTION,b,183,PF_X,PF_RESUMO_Y+250,PF_W*.52f,a); }
  }
  if(dados.plays>0 && dados.minutos>0) {
    tempo(t,sizeof(t),(int)((double)dados.minutos/dados.plays+.5));
    snprintf(b,sizeof(b),"%s por reprodução",t);
    corta(TXT_CAPTION,b,193,x,PF_RESUMO_Y+292,PF_W*.42f,a);
  }
  // As contagens representam eventos de reproducao, nao titulos unicos.
  double total=(double)dados.filmes+dados.episodios;
  if(total>0) {
    float w=PF_W*.50f, filmes=w*(float)(dados.filmes/total);
    gfx_cor((GfxRect){PF_X,PF_RESUMO_Y+316-scroll,w,10},.4f,.23f,.60f,.82f,a);
    if(filmes>0)gfx_cor((GfxRect){PF_X,PF_RESUMO_Y+316-scroll,filmes,10},.4f,.65f,.30f,.83f,a);
    snprintf(b,sizeof(b),"Filmes %.0f%%  ·  Episódios %.0f%%",100*dados.filmes/total,100*dados.episodios/total);
    corta(TXT_CAPTION,b,201,PF_X,PF_RESUMO_Y+340,w,a);
  }
}

static void desenharAtividade(float a) {
  if(!visivel(PF_ATIV_Y,500))return;
  tituloSecao("Ritmo de atividade",1,PF_ATIV_Y,a);
  GfxRect painel={PF_X,PF_ATIV_Y+58-scroll,PF_W*.57f,432};
  gfx_cor(painel,.045f,.075f,.073f,.085f,.98f*a);
  static const char *dias[7]={"Dom","Seg","Ter","Qua","Qui","Sex","Sáb"};
  float x0=PF_X+32, y0=PF_ATIV_Y+110-scroll, cel=44, gap=10;
  for(int c=0;c<7;c++) {
    TxtLinha l=txt_linha(TXT_MINI,dias[c],145,147,156,255);
    txt_desenhar_alpha(l,x0+c*(cel+gap)+(cel-l.w)*.5f,y0-28,a);
  }
  int max=1,melhor=0;
  for(int i=0;i<dados.nDias;i++) if(dados.atividade[i]>max){max=dados.atividade[i];melhor=i;}
  for(int i=0;i<dados.nDias;i++) {
    int p=dados.primeiroDiaSemana+i, col=p%7, lin=p/7;
    float q=(float)dados.atividade[i]/max;
    GfxRect c={x0+col*(cel+gap),y0+lin*(cel+gap),cel,cel};
    if(q<=0) gfx_cor(c,.16f,.12f,.12f,.14f,.78f*a);
    else gfx_cor(c,.16f,.38f+.23f*q,.13f+.13f*q,.52f+.31f*q,a);
    char nd[8];snprintf(nd,sizeof(nd),"%d",i+1);
    TxtLinha l=txt_linha(TXT_MINI,nd,242,242,246,255);
    txt_desenhar_alpha(l,c.x+(c.w-l.w)*.5f,c.y+(c.h-l.h)*.5f,a);
    if(secao==1 && i==dia)anel((GfxRect){c.x-3,c.y-3,c.w+6,c.h+6},.20f,a);
  }
  char b[96];
  if(dados.nDias)snprintf(b,sizeof(b),"Dia %d: %u reproduções",dia+1,dados.atividade[dia]);
  else snprintf(b,sizeof(b),"Atividade diária indisponível");
  corta(TXT_CAPTION,b,226,PF_X+32,PF_ATIV_Y+450,painel.w-64,a);
  // Legenda separada do calendario: leitura numerica continua acessivel sem
  // depender de distinguir cinco tons de violeta na tela.
  float lx=PF_X+454;
  texto(TXT_CAPTION,"Reproduções",205,lx,PF_ATIV_Y+106,a);
  for(int i=0;i<5;i++) {
    float q=i/4.0f;
    gfx_cor((GfxRect){lx+i*38,PF_ATIV_Y+150-scroll,28,28},.18f,
             i?.38f+.23f*q:.12f,i?.13f+.13f*q:.12f,i?.52f+.31f*q:.14f,a);
  }
  snprintf(b,sizeof(b),"0 a %d por dia",max);
  corta(TXT_MINI,b,184,lx,PF_ATIV_Y+196,painel.w-480,a);
  if(dados.atividade[melhor]){
    snprintf(b,sizeof(b),"Dia %d foi o mais ativo",melhor+1);
    corta(TXT_CAPTION,b,219,lx,PF_ATIV_Y+276,painel.w-480,a);
  }
  float rx=PF_X+PF_W*.63f;
  numero(b,sizeof(b),dados.streakAtual); textoC(TXT_TITULO1,b,190,111,224,rx,PF_ATIV_Y+76,a);
  corta(TXT_CALLOUT,dados.streakCompleto?"dias em sequência":"dias em sequência no mês",220,rx,PF_ATIV_Y+148,PF_W*.37f,a);
  snprintf(b,sizeof(b),"%d de %d dias ativos no mês",dados.diasAtivosMes,dados.nDias);
  corta(TXT_BODY,b,203,rx,PF_ATIV_Y+218,PF_W*.37f,a);
  if(dados.anoCompleto){
    snprintf(b,sizeof(b),"%d dias ativos no ano",dados.diasAtivosAno);
    corta(TXT_CAPTION,b,185,rx,PF_ATIV_Y+278,PF_W*.37f,a);
  } else corta(TXT_CAPTION,"Cobertura: período selecionado",185,rx,PF_ATIV_Y+278,PF_W*.37f,a);
}

static void desenharGeneros(float a) {
  if(!visivel(PF_GENEROS_Y,280))return;
  tituloSecao("Seu mapa de gêneros",2,PF_GENEROS_Y,a);
  if(!dados.nGeneros) { texto(TXT_BODY,"Os gêneros aparecem conforme o histórico cresce.",170,
                              PF_X,PF_GENEROS_Y+78,a); return; }
  double total=0; for(int i=0;i<dados.nGeneros;i++) total+=dados.generos[i].quantidade;
  if(total<1) total=1;
  float x=PF_X,y=PF_GENEROS_Y+92-scroll;
  for(int i=0;i<dados.nGeneros;i++) {
    float w=PF_W*((float)dados.generos[i].quantidade/total),r,g,b;
    rgb(dados.generos[i].cor?dados.generos[i].cor:PALETA[i],&r,&g,&b);
    if(w>2)gfx_cor((GfxRect){x,y,w-2,30},.12f,r,g,b,a); x+=w;
  }
  x=PF_X;
  for(int i=0;i<dados.nGeneros;i++) {
    float w=PF_W/4,xx=PF_X+(i%4)*w,yy=PF_GENEROS_Y+148+(i/4)*52;
    float r,g,b;rgb(dados.generos[i].cor?dados.generos[i].cor:PALETA[i],&r,&g,&b);
    gfx_cor((GfxRect){xx,yy+7-scroll,14,14},.5f,r,g,b,a);
    char rot[64];snprintf(rot,sizeof(rot),"%s · %d",dados.generos[i].nome,dados.generos[i].quantidade);
    corta(TXT_CAPTION,rot,219,xx+26,yy,w-42,a);
  }
}

static void desenharDestaques(float a) {
  if(!visivel(PF_DESTAQUES_Y,520))return;
  tituloSecao("Mais vistos",3,PF_DESTAQUES_Y,a);
  if(!dados.nDestaques) { texto(TXT_BODY,"Nenhum destaque neste período.",170,
                                PF_X,PF_DESTAQUES_Y+78,a); return; }
  gfx_recorte(PF_X-12,0,PF_W+24,NV_TELA_H);
  for(int i=0;i<dados.nDestaques;i++) {
    float f=focoItem[i], x=PF_X+i*(PF_CARD_W+PF_CARD_GAP)-scrollX;
    if(x+PF_CARD_W<PF_X-30 || x>NV_TELA_W+30) continue;
    float lift=f*NV_FOCO_LIFT;
    GfxRect r={x,PF_DESTAQUES_Y+62-scroll-lift,PF_CARD_W,PF_CARD_H};
    if(f>.02f) {
      GfxRect s={r.x-18,r.y+10,r.w+36,r.h+26};
      gfx_rect(s,0,GFX_SOMBRA,f,0,0,.05f,0,0,0,NV_SOMBRA_ALFA*a*f);
    }
    const char *art=dados.destaques[i].backdrop[0]?dados.destaques[i].backdrop:dados.destaques[i].poster;
    GLuint tx=art[0]?tex_obter_larg(art,PF_CARD_W):0;
    if(tx){gfx_tex_aspect_atual=tex_aspecto(art);gfx_rect(r,tx,GFX_CARD,f,0,0,.045f,1,1,1,a);gfx_tex_aspect_atual=0;}
    else gfx_cor(r,.045f,.13f,.13f,.15f,a);
    gfx_rect(r,0,GFX_VEU_BAIXO,0,0,0,.045f,1,1,1,.96f*a);
    char rank[12],meta[80]; snprintf(rank,sizeof(rank),".%d",i+1);
    textoC(TXT_TITULO1,rank,189,112,220,r.x+30,r.y+30+scroll,a*.88f);
    corta(TXT_TITULO2,dados.destaques[i].titulo,246,r.x+32,r.y+r.h-136+scroll,r.w-64,a);
    corta(TXT_CAPTION,dados.destaques[i].detalhe,216,r.x+32,r.y+r.h-84+scroll,r.w-64,a);
    tempo(meta,sizeof(meta),dados.destaques[i].minutos);
    char linha[112]; snprintf(linha,sizeof(linha),"%d reproduções  ·  %s",dados.destaques[i].plays,meta);
    corta(TXT_CAPTION,linha,194,r.x+32,r.y+r.h-44+scroll,r.w-64,a);
    if(secao==3 && item==i)anel((GfxRect){r.x-NV_ANEL_FOCO,r.y-NV_ANEL_FOCO,
                              r.w+NV_ANEL_FOCO*2,r.h+NV_ANEL_FOCO*2},.047f,a);
  }
  gfx_sem_recorte();
}

void perfil_desenhar(Uint32 agora) {
  if (entrada <= .002f) return;
  float a=entrada;
  if(lateral) {
    float x=1120+(1-a)*800;
    gfx_cor((GfxRect){0,0,1920,1080},0,.015f,.018f,.025f,.65f*a);
    gfx_cor((GfxRect){x,24,776,1032},.035f,.08f,.075f,.09f,a);
    txt_desenhar_alpha(txt_linha(TXT_TITULO3,"Sua atividade",244,242,248,255),x+44,62,a);
    const char *labels[]={"Fechar","Atualizar","Ver perfil completo"};
    for(int i=0;i<3;i++) {
      GfxRect b={x+44,i==0?130:878+(i-1)*76,688,60};
      int f=lateralFoco==i;
      gfx_cor(b,.18f,f?.92f:.14f,f?.91f:.13f,f?.96f:.16f,a);
      txt_desenhar_alpha(txt_linha(TXT_BODY,labels[i],f?25:236,f?24:234,f?30:242,255),b.x+24,b.y+14,a);
    }
    const char *msg=erro[0]?erro:carregando?"Carregando histórico…":dados.periodo;
    txt_desenhar_alpha(txt_linha_corta(TXT_BODY,msg,193,186,202,255,688),x+44,220,a);
    if(!carregando && !erro[0] && !temDados)
      txt_desenhar_alpha(txt_linha(TXT_BODY,"Sem atividade neste período",221,217,228,255),x+44,270,a);
    if(temDados) {
      const char *sem[]={"D","S","T","Q","Q","S","S"};
      for(int c=0;c<7;c++)txt_desenhar_alpha(txt_linha(TXT_CAPTION,sem[c],185,177,197,255),x+63+c*94,280,a);
      unsigned max=1;for(int d=0;d<dados.nDias;d++)if(dados.atividade[d]>max)max=dados.atividade[d];
      for(int d=0;d<dados.nDias;d++) {
        int slot=d+dados.primeiroDiaSemana;float v=(float)dados.atividade[d]/max;
        GfxRect b={x+44+(slot%7)*94,320+(slot/7)*70,82,58};
        gfx_cor(b,.14f,.14f+v*.43f,.12f+v*.08f,.18f+v*.51f,a);
        char n[8];snprintf(n,sizeof n,"%d",d+1);
        txt_desenhar_alpha(txt_linha(TXT_CAPTION,n,243,239,249,255),b.x+24,b.y+16,a);
      }
      char b[128],dur[32];tempo(dur,sizeof dur,dados.minutos);
      snprintf(b,sizeof b,"%s · %d reproduções",dur,dados.plays);
      txt_desenhar_alpha(txt_linha_corta(TXT_BODY,b,244,240,248,255,688),x+44,755,a);
      snprintf(b,sizeof b,"%d dias ativos · %d dias seguidos no mês",dados.diasAtivosMes,dados.streakAtual);
      txt_desenhar_alpha(txt_linha_corta(TXT_CAPTION,b,193,181,210,255,688),x+44,801,a);
      if(dados.parcial)txt_desenhar_alpha(txt_linha(TXT_MINI,"Histórico parcial · detalhes no perfil",180,170,192,255),x+44,840,a);
    }
    return;
  }
  gfx_cor((GfxRect){0,0,NV_TELA_W,NV_TELA_H},0,
          NV_COR_FUNDO_R,NV_COR_FUNDO_G,NV_COR_FUNDO_B,a);
  gfx_recorte(0,0,NV_TELA_W,NV_TELA_H);
  if(carregando && !temDados) desenharLoading(agora,a);
  else if(!temDados) desenharVazio(a);
  else {
    desenharResumo(a); desenharAtividade(a); desenharGeneros(a); desenharDestaques(a);
  }
  gfx_sem_recorte();
  const char *aviso=erro[0]?erro:carregando?"Atualizando histórico…":
    dados.aviso[0]?dados.aviso:dados.parcial?"Histórico parcial: os totais consideram somente os registros carregados.":NULL;
  if(aviso){
    gfx_cor((GfxRect){PF_X,1010,PF_W,54},.15f,.13f,.11f,.16f,.97f*a);
    TxtLinha l=txt_linha_corta(TXT_CAPTION,aviso,226,217,235,255,PF_W-40);
    txt_desenhar_alpha(l,PF_X+20,1024,a);
  }
}
