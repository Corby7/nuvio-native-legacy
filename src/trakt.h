// Continue Assistindo vindo do Trakt.
//
// Antes o historico era um retrato exportado do app web (watchProgressItems do
// localStorage) e congelava no momento da exportacao. O dono ja usa o Trakt
// como fonte de progresso (traktSettings.watchProgressSource = "trakt"), entao
// perguntar direto a ele e o que mantem a fileira viva — e o que faz o app
// nativo concordar com o app web sem os dois se escreverem.
//
// CREDENCIAIS ficam em art/trakt.txt ("token<TAB>clientId"), arquivo do dono:
// tratar como segredo, nao versionar. Sem o arquivo o modulo simplesmente nao
// faz nada e a fileira cai no que veio no pacote.
#ifndef NV_TRAKT_H
#define NV_TRAKT_H
#include "catalogo.h"

int  trakt_carregar(const char *dirArte);   // 1 quando ha credencial
int  trakt_ativo(void);

// Preenche ate `max` itens do "continue assistindo", ja com arte resolvida.
// BLOQUEIA — chamar do fio de descoberta. Devolve quantos preencheu.
int  trakt_continuar(CatItem *saida, int max);

// Informa onde o dono parou. `imdb` pode trazer episodio ("tt123:4:9").
// Ate agora o app so LIA o Trakt; sem isto, assistir aqui nao mexia no
// "continue assistindo" dos outros aparelhos dele. Nao bloqueia: sai num fio.
void trakt_marcar(const char *imdb, double posSeg, double durSeg);

// Watchlist ("Minha Lista") e colecao ("Comprados") do dono. `qual` e
// "watchlist" ou "collection". BLOQUEIA — chamar do fio de descoberta.
int  trakt_lista(const char *qual, CatItem *saida, int max);

#endif
