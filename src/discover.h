// Monta o catalogo EM EXECUCAO, a partir da rede.
//
// Antes tudo vinha de arquivos gerados a mao (catalogo.txt, ids.txt,
// episodios.txt) com a arte baixada junto do pacote. Funcionava, mas
// congelava: recomendacao de ontem, episodio da temporada de ontem, e cada
// mudanca exigia regerar e reinstalar. Agora as fileiras vem dos catalogos dos
// addons do dono e os episodios vem do Cinemeta na hora que o titulo abre.
//
// Os arquivos continuam servindo de RESERVA: sem rede, o app abre com o que
// veio no pacote em vez de abrir vazio.
#ifndef NV_DISCOVER_H
#define NV_DISCOVER_H
#include <stddef.h>
#include "catalog.h"

// Dispara a montagem do catalogo num fio proprio. Volta na hora.
void disc_start(void);

// Le a chave do TMDB (art/tmdb.txt). Sem ela o elenco fica so com nomes, sem
// foto nem personagem.
void disc_tmdb(const char *dirArt);

// Chave do TMDB vinda da CONTA, no lugar de art/tmdb.txt. MEDIDO na conta do
// dono: `sync_pull_provider_credentials` devolve o provedor "tmdb" com um
// campo `api_key`. Enquanto a chave sair do arquivo, ela vai dentro do .ipk e
// e a chave de quem montou o pacote — cota dele, para todo mundo que instalar.
void disc_tmdb_set(const char *key);

// A chave do TMDB ja carregada. Devolve "" quando art/tmdb.txt nao existe.
// O modulo `pessoa` precisa dela para a filmografia, e ler o arquivo duas vezes
// daria duas fontes de verdade para o mesmo segredo.
const char *disc_key_tmdb(void);

// "2026-07-29" -> "29 de julho de 2026". Vive aqui porque a descoberta ja
// precisava dela para a data de episodio; a tabela "Detalhes do Filme" e o
// segundo consumidor, e duplicar a lista de meses era pedir para as duas
// divergirem. Entrada fora do padrao ISO sai como veio.
void disc_date_long(const char *iso, char *dst, size_t size);

// Nome de genero em portugues ("Action" -> "Ação"). O Cinemeta e o catalogo do
// pacote guardam os generos em INGLES, e eles apareciam crus numa interface em
// portugues. Genero fora da tabela sai como veio.
const char *disc_genre_label(const char *g);

// --- busca por titulo --------------------------------------------------------
// Consulta o Cinemeta em filme e serie. NAO BLOQUEIA: dispara um fio e volta na
// hora; chamar de novo com o mesmo termo nao refaz o pedido, e com termo
// diferente faz o fio em voo descartar o resultado velho e ir atras do novo (o
// dono continua digitando enquanto a rede responde).
//
// Existe porque a tela de busca so filtrava o que ja estava em memoria, e
// procurar qualquer coisa fora das primeiras linhas de cada catalogo nao
// achava nada.
void disc_fetch(const char *term);

// Quantos resultados ha PARA ESTE TERMO. Devolve 0 quando o que chegou e de uma
// consulta anterior — assim a tela nunca mostra o resultado de outra palavra.
int  disc_search_n(const char *term);

// Copia o resultado `i`. 1 se copiou.
int  disc_search_item(int i, CatItem *dst);

// --- busca em VARIOS catalogos ----------------------------------------------
//
// Um "alvo" e um catalogo que declara busca no manifesto. Sao os 2 do Cinemeta
// mais os que os addons do dono declararem (hoje 8: Xperience, AIOStreams por
// TMDB e por TVDB, e Akashi TV — filme e serie em cada um).
//
// A tela desenha UMA FILEIRA POR ALVO, na ordem em que os alvos foram
// registrados, pulando os que ainda nao responderam ou vieram vazios. Assim o
// primeiro catalogo a responder ja aparece, em vez de a tela esperar o mais
// lento de dez.
int  disc_search_n_targets(void);
const char *disc_search_target_title(int target);   // "Filmes", "Séries"
const char *disc_search_target_addon(int target);    // "Cinemeta", "Xperience"
int  disc_search_target_n(int target, const char *term);
int  disc_search_target_item(int target, int i, CatItem *dst);
// Sobe a cada termo novo. Quem guarda posicao de foco entre quadros deve
// reajustar quando este numero mudar.
int  disc_search_generation(void);

// Registro dos alvos, chamado pelo carregamento dos manifestos. `zerar` repoe
// so o Cinemeta.
void disc_targets_search_reset(void);
void disc_target_search(const char *base, const char *kind, const char *id,
                     const char *title, const char *addon);

// 1 enquanto busca; a home pode usar isto para um indicador.
int  disc_searching(void);

// Pede os episodios do titulo `indiceItem` na temporada `temporada`
// (0 = a temporada onde o dono parou, ou a primeira). Idempotente: pedir duas
// vezes a mesma coisa nao refaz a busca.
// --- VER TUDO: um catalogo inteiro, em paginas ------------------------------
//
// A home mostra 12 itens por fileira (MAX_POR_FILEIRA). O catalogo tem mais, e
// o protocolo Stremio pagina por `skip`:
//   <base>/catalog/<tipo>/<id>/skip=<n>.json
// E o mesmo caminho da busca, com outro filtro no lugar do termo.
//
// Assincrono, como todo o resto: dispara e volta na hora. Quem desenha pergunta
// quantos ja chegaram.
#define SEEALL_MAX 1000

// Comeca (ou continua) a leitura do catalogo. `pagina` 0 e o inicio; cada
// pagina seguinte pede skip = pagina * VT_PASSO. Repetir a mesma pagina nao
// refaz o pedido.
void disc_seeall_open(const char *base, const char *kind, const char *catId);
void disc_seeall_filter(const char *base, const char *kind, const char *catId, const char *genre);
int disc_seeall_error(void);
// Pede a proxima pagina, se houver. Nada acontece se a ultima veio curta — sinal
// de fim de lista no protocolo.
void disc_seeall_more(void);
int  disc_seeall_n(void);
int  disc_seeall_item(int i, CatItem *dst);
int  disc_seeall_loading(void);
// 1 quando a ultima pagina veio curta: nao ha mais o que pedir.
int  disc_seeall_end(void);
void disc_seeall_close(void);

void disc_episodes(int indexItem, int season);
// Solta o pedido de episodios que chegou enquanto outro carregava. Chame por
// quadro; sem isto uma troca de temporada feita durante um carregamento fica
// pendurada e a lista nunca chega na temporada escolhida.
void disc_episodes_pending(void);
int disc_episodes_loading(int indexItem);

// Busca o meta de um titulo que o catalogo NAO tem e o acrescenta ao fim.
// Nao bloqueia. Serve ao credito de um ator e ao item de "Mais como este":
// sem isto, tudo que estivesse fora do catalogo do dono nao abria.
void disc_request_title(const char *imdb);
// Mesma coisa a partir do id do TMDB, que e o que o credito de um ator traz.
// `tipo` e "movie" ou "tv". Resolve o IMDb por external_ids antes de pedir o
// meta — uma chamada a mais, so quando o dono abre o credito.
void disc_request_title_tmdb(long tmdbId, const char *kind);
// Indice do titulo que acabou de entrar, ou -1. CONSOME o resultado.
int  disc_title_ready(void);
int  disc_title_searching(void);

#endif
