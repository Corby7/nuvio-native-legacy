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

// 1 enquanto busca; a home pode usar isto para um indicador.
int  desc_buscando(void);

// Pede os episodios do titulo `indiceItem` na temporada `temporada`
// (0 = a temporada onde o dono parou, ou a primeira). Idempotente: pedir duas
// vezes a mesma coisa nao refaz a busca.
void desc_episodios(int indiceItem, int temporada);

#endif
