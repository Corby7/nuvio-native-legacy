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
#include "catalog.h"
#include "profile.h"
#include <stddef.h>

int  trakt_load(const char *dirArt);   // 1 quando ha credencial

// Monta os tres cabecalhos que TODO pedido ao Trakt exige (token, versao da
// api e a chave do aplicativo) em `cab`, que precisa ter 4 posicoes — a
// ultima recebe NULL. Devolve 0 quando nao ha credencial carregada.
//
// Existe porque este mesmo bloco estava copiado em cada funcao do trakt.c, e o
// extras.c seria a quarta copia. Os buffers `aut` e `chave` sao de quem chama:
// os cabecalhos apontam para eles e precisam viver ate o fim do pedido.
int  trakt_headers(const char **header, char *aut, size_t nAut,
                      char *key, size_t nKey);
int  trakt_active(void);

// Credencial vinda da CONTA, no lugar do arquivo. O token sai de
// sync_pull_provider_credentials (provider "trakt"); o clientId e do
// APLICATIVO, nao da pessoa, e vem compilado (-DNV_TRAKT_CLIENT_ID, gerado de
// local.properties). Enquanto isto nao existia, o vinculo Trakt do app nativo
// era o do dono do pacote — para todo mundo que instalasse.
int  trakt_set(const char *token, const char *clientId);

// Esquece a credencial. Chamado ao SAIR: um token de Trakt que sobrevive ao
// logout continua ESCREVENDO (trakt_marcar) na conta de quem saiu, com o que a
// proxima pessoa assistir.
void trakt_forget(void);

// Preenche ate `max` itens do "continue assistindo", ja com arte resolvida.
// BLOQUEIA — chamar do fio de descoberta. Devolve quantos preencheu.
int  trakt_resume(CatItem *output, int max);

// Atividade recente dos AMIGOS do dono. Usa o feed social oficial do Trakt
// (/users/me/friends/activities), mantendo no CatItem o titulo/arte normais e
// os dados sociais nos campos de apresentacao: `pais` = nome do amigo,
// `provNome` = acao e `direcao` = contexto do episodio. BLOQUEIA.
int  trakt_social(CatItem *output, int max);

// Snapshot mensal usado pela tela Perfil e Stats. Faz as chamadas no fio de
// trabalho do app e nunca deve ser executado no laco de desenho.
int  trakt_profile(ProfileData *output);

// Informa onde o dono parou. `imdb` pode trazer episodio ("tt123:4:9").
// Ate agora o app so LIA o Trakt; sem isto, assistir aqui nao mexia no
// "continue assistindo" dos outros aparelhos dele. Nao bloqueia: sai num fio.
void trakt_mark(const char *imdb, double posSeg, double durationSeg);

// Watchlist ("Minha Lista") e colecao ("Comprados") do dono. `qual` e
// "watchlist" ou "collection". BLOQUEIA — chamar do fio de descoberta.
int  trakt_list(const char *which, CatItem *output, int max);

// Acrescenta ou tira o titulo da WATCHLIST do dono. Nao bloqueia. O estado de
// leitura ja vem em CatItem.naLista, preenchido por trakt_lista na descoberta —
// o que faltava era escrever de volta: o botao "+" so mexia num vetor local e a
// lista nos outros aparelhos nunca sabia.
void trakt_watchlist(const char *imdb, int add);

// Marca (ou desmarca) o titulo como ASSISTIDO em /sync/history.
//
// NAO confundir com trakt_marcar, que e /scrobble/pause ("parei aqui") e serve
// ao player. O botao do olho quer dizer "ja vi este", e usava trakt_marcar com
// duracao 1.0 — valor que a propria guarda daquela funcao rejeita, entao nada
// chegava ao Trakt.
void trakt_watched(const char *imdb, int mark);

#endif
