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
#ifndef NV_DESCOBERTA_H
#define NV_DESCOBERTA_H

// Dispara a montagem do catalogo num fio proprio. Volta na hora.
void desc_iniciar(void);

// Le a chave do TMDB (art/tmdb.txt). Sem ela o elenco fica so com nomes, sem
// foto nem personagem.
void desc_tmdb(const char *dirArte);

// A chave do TMDB ja carregada. Devolve "" quando art/tmdb.txt nao existe.
// O modulo `pessoa` precisa dela para a filmografia, e ler o arquivo duas vezes
// daria duas fontes de verdade para o mesmo segredo.
const char *desc_chave_tmdb(void);

// 1 enquanto busca; a home pode usar isto para um indicador.
int  desc_buscando(void);

// Pede os episodios do titulo `indiceItem` na temporada `temporada`
// (0 = a temporada onde o dono parou, ou a primeira). Idempotente: pedir duas
// vezes a mesma coisa nao refaz a busca.
void desc_episodios(int indiceItem, int temporada);

// Busca o meta de um titulo que o catalogo NAO tem e o acrescenta ao fim.
// Nao bloqueia. Serve ao credito de um ator e ao item de "Mais como este":
// sem isto, tudo que estivesse fora do catalogo do dono nao abria.
void desc_pedir_titulo(const char *imdb);
// Mesma coisa a partir do id do TMDB, que e o que o credito de um ator traz.
// `tipo` e "movie" ou "tv". Resolve o IMDb por external_ids antes de pedir o
// meta — uma chamada a mais, so quando o dono abre o credito.
void desc_pedir_titulo_tmdb(long tmdbId, const char *tipo);
// Indice do titulo que acabou de entrar, ou -1. CONSOME o resultado.
int  desc_titulo_pronto(void);
int  desc_titulo_buscando(void);

#endif
