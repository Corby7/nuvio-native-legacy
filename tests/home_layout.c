// Sem janela, rede ou TV: valida composição editorial e foco sobre dados reais
// de catálogo (fixtures), usando a implementação da Home e do catálogo.
#include <assert.h>
#include "../src/home.c"

int main(void) {
  assert(MAX_FIL <= FOCUS_MAX_FILEIRAS);
  assert(perfilCatalogo("Oscars 2026 - Filme") == FILEIRA_COLECAO);
  assert(perfilCatalogo("NETFLIX - Série") == FILEIRA_SERVICO);
  assert(perfilCatalogo("For You - Filme") == FILEIRA_NORMAL);
  assert(larguraDe(FILEIRA_DESTAQUE) > larguraDe(FILEIRA_COLECAO));
  assert(larguraDe(FILEIRA_COLECAO) > larguraDe(FILEIRA_SERVICO));
  assert(!temRotulo(FILEIRA_DESTAQUE));
  assert(!temRotulo(FILEIRA_CATALOGOS));

  CatItem *itensTeste = calloc(48, sizeof *itensTeste);
  CatFileira fils[16] = {0};
  assert(itensTeste);
  for (int i = 0; i < 16; i++) {
    snprintf(fils[i].chave, sizeof fils[i].chave, "catalogo_%d", i);
    snprintf(fils[i].titulo, sizeof fils[i].titulo, "Lista %d", i);
    snprintf(fils[i].base, sizeof fils[i].base, "https://example.invalid/addon");
    snprintf(fils[i].tipo, sizeof fils[i].tipo, "movie");
    snprintf(fils[i].catId, sizeof fils[i].catId, "id%d", i);
    fils[i].ini = i*3; fils[i].n = 3;
  }
  snprintf(fils[0].chave, sizeof fils[0].chave, "continue_watching");
  fils[0].base[0] = fils[0].catId[0] = 0;
  snprintf(fils[14].titulo, sizeof fils[14].titulo, "Netflix - Filme");
  snprintf(fils[15].titulo, sizeof fils[15].titulo, "Oscar - Filme");
  cat_definir_tudo(itensTeste, 48, fils, 16);
  sincronizarFileiras();
  assert(nFileiras == 17);
  assert(foco.nFileiras == 17);
  assert(fileiras[1].tipo == FILEIRA_SOCIAL && fileiras[1].ini == -1 && fileiras[1].n == 1);
  assert(fileiras[2].tipo == FILEIRA_DESTAQUE);
  for (int i = 0; i < 16; i++) {
    int achou = 0;
    for (int r = 0; r < nFileiras; r++)
      if (!strcmp(fileiras[r].chave, fils[i].chave)) achou++;
    assert(achou == 1); // nenhum catálogo removido ou duplicado
  }
  foco.fileira = 0; foco.coluna = 0;
  for (int i = 0; i < 16; i++) assert(focus_mover(&foco, 0, 1));
  assert(foco.fileira == 16);
  assert(!focus_mover(&foco, 0, 1));
  // Mesma contagem, ordem diferente: manter chave, coluna e scroll.
  foco.fileira = 5; foco.coluna = 2; scrollX[5] = 123;
  char chave[192]; snprintf(chave, sizeof chave, "%s", fileiras[5].chave);
  CatFileira troca = fils[4]; fils[4] = fils[8]; fils[8] = troca;
  cat_definir_tudo(itensTeste, 48, fils, 16);
  sincronizarFileiras();
  assert(!strcmp(fileiras[foco.fileira].chave, chave));
  assert(foco.coluna == 2);
  assert(scrollX[foco.fileira] == 123);
  assert(col_carregar("tests/fixtures/collections") == 2);
  assert(col_folder(0)->nSources==2);
  assert(col_folder(0)->frames==0);
  assert(col_folder(-1)==NULL);
  const char *ids[]={"", "now_playing_movies","trending_movies","trending_series",
    "ai_movies_for_you","ai_series_for_you","snoak_top100_movies","snoak_top100_series"};
  for(int i=1;i<8;i++)snprintf(fils[i].catId,sizeof fils[i].catId,"%s",ids[i]);
  cat_definir_tudo(itensTeste,48,fils,16);filsAplicadas=-1;sincronizarFileiras();
  // A curadoria ordena os atalhos conhecidos, mas nao apaga fileiras novas
  // declaradas pelo addon. O fixture tem oito chaves fora da tabela editorial.
  assert(nFileiras>=11);
  assert(fileiras[1].tipo==FILEIRA_SOCIAL);
  assert(!strcmp(fileiras[2].titulo,"Recent Release"));
  assert(!strcmp(fileiras[3].titulo,"Streaming"));
  assert(fileiras[3].tipo==FILEIRA_CATALOGOS);
  assert(!strcmp(col_folder(fileiras[3].folders[0])->title,"Netflix"));
  assert(!strcmp(fileiras[4].titulo,"Trending Movies"));
  assert(!strcmp(fileiras[6].titulo,"Themes"));
  assert(!strcmp(fileiras[7].catId,"ai_movies_for_you"));
  assert(fileiras[9].tipo==FILEIRA_TOP10);
  assert(fileiras[10].tipo==FILEIRA_TOP10);
  assert(fileiras[9].stackN==3 && fileiras[9].n==1);
  for (int i=8; i<16; i++) {
    int encontrado=0;
    for (int r=0; r<nFileiras; r++)
      if (!strcmp(fileiras[r].chave, fils[i].chave)) encontrado=1;
    assert(encontrado);
  }
  fileiras[9].n=3;fileiras[9].stackN=0;fileiras[9].verTudo=1;
  for(int i=0;i<nFileiras;i++)assert(strcmp(fileiras[i].titulo,"Seus catálogos"));
  snprintf(fils[15].chave,sizeof fils[15].chave,"social_activity");
  fils[15].base[0]=fils[15].catId[0]=0;
  cat_definir_tudo(itensTeste,48,fils,16);sincronizarFileiras();
  int sociais=0;
  for(int i=0;i<nFileiras;i++)if(fileiras[i].tipo==FILEIRA_SOCIAL){
    sociais++;assert(fileiras[i].ini==45 && fileiras[i].n==3);
  }
  assert(sociais==1); // dados reais substituem vazio, nunca duplicam a fileira
  assert(fileiras[9].stackN==0 && fileiras[9].n==3 && fileiras[9].verTudo);

  // Arte de outro titulo nunca e fallback silencioso, mesmo quando o indice
  // esta alem do acervo local. Sem catalogo, os vetores locais continuam
  // disponiveis apenas na mesma posicao.
  snprintf(itensTeste[0].poster,sizeof itensTeste[0].poster,"own-poster.jpg");
  snprintf(itensTeste[0].backdrop,sizeof itensTeste[0].backdrop,"own-backdrop.jpg");
  snprintf(itensTeste[1].poster,sizeof itensTeste[1].poster,"other-poster.jpg");
  nBd=2; nPst=1;
  snprintf(bd[0],sizeof bd[0],"fallback-0.jpg");
  snprintf(bd[1],sizeof bd[1],"fallback-1.jpg");
  snprintf(pst[0],sizeof pst[0],"fallback-poster-0.jpg");
  cat_definir_tudo(itensTeste,48,NULL,0);
  assert(!strcmp(arte_por_identidade(0,0),"own-poster.jpg"));
  assert(!strcmp(arte_por_identidade(0,1),"own-backdrop.jpg"));
  assert(!strcmp(arte_por_identidade(1,0),"other-poster.jpg"));
  assert(arte_por_identidade(999,0)==NULL);

  // A integracao de segunda ordem retargeta sem overshoot, e reduced motion
  // pode saltar ao destino sem deixar velocidade residual.
  { float x=0, v=0;
    for (int i=0; i<60; i++) {
      x=anim_mola2(&v,x,1.0f,0.016f,NV_MOLA2_SCROLL);
      assert(x>=0.0f && x<=1.0f);
    }
    x=anim_mola2(&v,x,0.0f,0.016f,NV_MOLA2_SCROLL);
    assert(x>=0.0f && x<=1.0f);
    x=anim_mola2_reduzida(&v,x,0.35f,0.016f,NV_MOLA2_SCROLL,1);
    assert(x==0.35f && v==0.0f);
  }
  assert(NV_HERO_FADE_MS>=180.0f && NV_HERO_FADE_MS<=250.0f);
  free(itensTeste);
  puts("home layout: PASS (fallback, imported collections, requested order, ranks, focus)");
  return 0;
}
