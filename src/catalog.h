// Catalogo de titulos vindo de um arquivo, no lugar das listas fixas no codigo.
//
// Existe porque testar layout com nomes inventados esconde problemas reais: os
// titulos de verdade tem tamanhos muito diferentes ("CODA" contra "Assassinos
// da Lua das Flores"), acentos, e sinopses que nao cabem em tres linhas. Cada
// item tambem carrega o LOGO do titulo, que e o que o app da Apple desenha no
// lugar do nome em texto.
#ifndef NV_CATALOG_H
#define NV_CATALOG_H

// 40 titulos hoje (14 do historico do dono + 26 dos catalogos). A folga evita
// o corte silencioso que ja aconteceu: com 32 os oito ultimos sumiam sem aviso.
// Nao ha mais teto de catalogo: o vetor cresce conforme a rede entrega. Um
// numero fixo aqui sempre foi arbitrario — comecou em 32, virou 48, 160, 260,
// e a watchlist do dono continuava batendo no limite. O que limita de verdade
// e o cache de TEXTURA, que tem teto proprio e so guarda o que esta na tela;
// o item em si custa ~3,5 KB de texto.
//
// CAT_MAX sobrevive so como teto de seguranca contra resposta absurda.
#define CAT_MAX 2000

typedef struct {
  char backdrop[512];
  char poster[512];
  char logo[512];      // vazio quando o titulo nao tem logo
  char title[160];
  char genre[160];    // "Programa de TV · Drama · Misterio"
  char meta[96];       // "2022 · 3 temporadas"
  char age_rating[8];
  char synopsis[900];
  // Elenco real: nome, papel e a foto (quando o TMDB tem). Sem isto a secao
  // "Elenco e equipe" fica com nomes inventados, e nomes inventados nao testam
  // o layout — os de verdade tem tamanhos que quebram a coluna.
  // `tmdb` e o id da PESSOA no TMDB, nao do titulo: e a chave para abrir a
  // filmografia dela (/person/<id>?append_to_response=combined_credits), que e
  // o que o web faz no `openCastDetail`. Sem ele o unico caminho seria procurar
  // por nome, que erra em homonimo e em nome com acento.
  struct { char name[64]; char role[64]; char photo[512]; long tmdb; } cast[6];
  int nCast;
  char directing[128];
  // Nota da critica em porcentagem e o logo do servico onde o titulo esta. Sao
  // as duas coisas que a linha tecnica do app da Apple mostra alem do ano e da
  // duracao — sem elas a linha fica com metade da informacao.
  int  score;              // 0 = desconhecida
  // Pais de producao, para a ultima linha de meta do detalhe (o web mostra
  // 'United States of America' ali). Vem do /meta, nao do catalogo.
  char pais[64];
  char providerLogo[512];
  char providerName[64];
  // Onde assistir alem da assinatura: aluguel e compra na regiao BR do TMDB.
  // Vazios = o servico nao oferece o titulo por esse meio aqui. O detalhe
  // encolhe a secao de acordo — um card sem dado e pior que a ausencia dele.
  char rentLogo[512];
  char rentName[64];
  char compLogo[512];
  char compName[64];
  // Identificador do titulo no IMDb ("tt11280740") e o tipo que os addons usam
  // ("movie"/"series"). Vem de art/ids.txt, resolvido pelo Cinemeta — sem ele
  // nao ha como perguntar fontes a addon nenhum.
  // Quanto do titulo o dono ja assistiu, 0..100. Vem do app web (chave
  // watchProgressItems), quarta coluna de extra.txt. 0 = nao comecou.
  int  progress;
  // Legenda do card em "Continue Assistindo". Serie mostra "T1, E8 · 16 min";
  // filme mostra so o tempo que falta. Temporada/episodio ficam em 0 no filme,
  // e e isso que separa os dois casos no desenho.
  int  season, episode;
  int  remainingMin;
  char nameEpisode[120]; // titulo do episodio em andamento, nunca nome do arquivo
  // Temporadas que a serie tem, na ordem. Sai do campo `videos` do Cinemeta,
  // buscado quando o titulo abre. 0 = ainda nao se sabe (ou e filme), e as
  // abas caem no padrao de 3 que existia fixo.
  int  seasons[12];
  int  nSeasons;
  // Vem do Trakt: 1 se esta na watchlist do dono, 1 se esta na colecao dele.
  // Ficam no item e nao numa tabela a parte da biblioteca porque o catalogo e
  // reconstruido da rede — uma tabela por indice apontaria para outro titulo
  // depois da primeira atualizacao.
  int  inList, inCollection;
  char imdb[16];
  char kind[8];
  // Autoria do feed social, separada dos metadados do filme.
  char socialName[96], socialSlug[128], socialAvatar[768], socialAction[64];
  // Id do titulo no TMDB, quando a busca por imdb_id ja o resolveu (ver
  // fotosDoElenco em descoberta.c). Era descartado; e por ele que se chega a
  // COLECAO do filme, que o TMDB so expoe por id proprio.
  long tmdb;
} CatItem;

// Um episodio de serie. Vem de art/episodios.txt, gerado a partir do campo
// `videos` do Cinemeta (/meta/series/<id>.json) — os mesmos episodios que os
// addons indexam, entao o que a tela lista e o que da para pedir fonte.
typedef struct {
  int  season, episode;
  char name[120];
  char duration[16];    // "38 min"; vazio quando o Cinemeta nao informa
  // Data por EXTENSO, como o web: "27 de janeiro de 2023". Ele usa
  // toLocaleDateString com {month:"long", day:"numeric", year:"numeric"}
  // (metaDetailsScreen.js:1387) — "27/01/2023" era invencao do port. 16 bytes
  // nao cabiam: "15 de novembro de 2024" tem 22.
  //
  // Quem desenha encurta para so o ano quando `showFullReleaseDate` esta
  // desligado (ajustes_data_completa()); o ano sao os 4 ultimos caracteres.
  char date[40];
  char synopsis[420];
  char thumb[512];     // still do episodio; vazio cai na arte do titulo
} CatEp;

// Le <dir>/catalogo.txt. Devolve quantos itens carregou (0 = nenhum, e quem
// chama deve seguir com o que tiver).
int  cat_load(const char *dirArt);

// --- CACHE EM DISCO DO CATALOGO MONTADO PELA REDE ----------------------------
//
// Medido na TV: 14,5 s entre abrir o app e o catalogo da rede estar completo, e
// TODA abertura refazia os ~30 pedidos. O catalogo do PACOTE (catalogo.txt)
// cobria esse vao com 40 titulos estaticos que nao sao os do dono.
//
// Aqui o que a descoberta montou e gravado como esta na memoria e relido na
// proxima abertura, antes de qualquer rede. A rede continua rodando por cima e
// substitui quando chega — o cache nao e a verdade, e o que mostrar enquanto a
// verdade nao chega.
//
// Formato BINARIO e nao texto: CatItem e POD (so vetores de char e inteiros,
// nenhum ponteiro), entao gravar em bloco e correto e dispensa um serializador
// que teria de ser mantido em sincronia com a struct a cada campo novo. O
// cabecalho guarda `sizeof(CatItem)` e uma versao: se a struct mudar, o arquivo
// e RECUSADO em vez de lido torto. Ler lixo aqui seria pior que nao ter cache.
int  cat_write_cache(const char *dirArt);
// Devolve 1 se carregou. Chamar DEPOIS de cat_carregar: ele substitui o
// catalogo do pacote quando o cache existe e e valido.
int  cat_read_cache(const char *dirArt);
// 1 enquanto o que esta na tela veio do CACHE, e nao da rede desta sessao.
//
// A descoberta publica cada fileira assim que ela chega, o que e certo numa
// tela vazia e ERRADO sobre o cache: a home iria de 16 fileiras para 1 e
// voltaria a crescer na frente do dono. Com o cache no ar, ela espera o
// catalogo completo. Sem cache, publica em partes como antes.
int  cat_do_cache(void);
void cat_cache_replaced(void);
int  cat_n(void);
const CatItem *cat_item(int i);

// Indice do titulo com este IMDb id, ou -1. O id do catalogo pode trazer
// episodio ("tt123:2:1"); a comparacao para no primeiro ':' dos dois lados.
//
// Existe para abrir um titulo a partir de um id que veio de FORA do catalogo —
// a filmografia de um ator e a aba "Mais como este" devolvem tt..., e sem esta
// busca nao haveria como saber se aquele titulo e um dos que ja temos meta.
// Onde progresso.txt e gravado. Passou a ser necessario com o login: o
// progresso e dado DO USUARIO e nao pode morar na pasta do pacote, que e a
// mesma para todo mundo que usar o aparelho. Chamar depois de cat_carregar.
void cat_dir_writing(const char *dir);

int cat_index_by_imdb(const char *imdb);

// Acrescenta um titulo ao FIM e devolve o indice, ou -1. Para o titulo que veio
// de fora do catalogo (filmografia de ator, "Mais como este"). Ver a nota sobre
// a troca de bloco em catalogo.c.
int cat_append(const CatItem *item);

// Acrescenta `qtd` itens numa UNICA troca de bloco e escreve os indices em
// `saidaIdx` (pode ser NULL). Devolve quantos entraram.
//
// Use este, e nao cat_acrescentar em laco, sempre que houver mais de um: aquele
// copia o catalogo inteiro por chamada, e a busca chegava a mover dezenas de MB
// no fio de desenho a cada tecla.
int cat_append_lote(const CatItem *v, int count, int *outputIdx);

// Atualiza o espelho local de "esta na watchlist". A verdade e o Trakt, mas
// esperar o proximo ciclo de descoberta para o botao mudar de cara faria o
// toque parecer sem efeito.
void cat_set_in_list(int i, int inList);

// Grava onde o dono parou NESTE app. Ate agora o progresso so era LIDO (do app
// web); sem isto, assistir pelo app nativo nao mudava nada na tela.
//
// Vai para <arte>/progresso.txt e nao para o SQLite do app web: aquele arquivo
// pertence a outro processo, que o mantem aberto e em cache — escrever la de
// fora corromperia o estado dele. Unir as duas fontes e trabalho a parte; por
// enquanto o que este app grava ganha do que veio de la, que e o certo, porque
// e mais recente.
void cat_save_progress(int index_, double posSeg, double durationSeg);
void cat_save_progress_ep(int index_, double posSeg, double durationSeg, int season, int episode);

// Episodios do titulo `indiceItem`. Filme devolve 0 — e o que a tela usa para
// decidir se mostra a secao de episodios.
// Substitui o catalogo inteiro pelo que veio da rede. Os caminhos de arte
// passam a ser URLs — o tex_cache baixa e guarda em disco sozinho.
void cat_set(const CatItem *list, int n);

// --- FILEIRAS DA HOME --------------------------------------------------------
// O app web nao tem fileira fixa. Cada fileira e UM CATALOGO de UM addon, e a
// lista sai de `homeCatalogPrefs` (por perfil), aplicada em
// `sortAndFilterRowsInternal` (js/ui/screens/home/homeScreen.js:9856):
//
//   1. junta catalogos e colecoes num mapa indexado por `homeCatalogKey`
//   2. `ensureOrderKeysWithPrefs` devolve a ordem salva com as chaves NOVAS
//      acrescentadas no FIM — catalogo que apareceu depois entra por ultimo
//   3. tira as desativadas, conferindo DUAS chaves: `homeCatalogDisableKey`
//      (<baseUrl>_<tipo>_<catalogoId>_<nome>) e `homeCatalogKey`
//   4. aplica `customTitles[homeCatalogKey]` sobre o nome do catalogo
//   5. colecoes com `pinToTop` vao na frente e nunca sao cortadas
//   6. corta o total em `getHomeRowLimit()`
//
// O teto para NOS e 16: `HOME_MAX_ROWS_LEGACY_TV` em homeConstants.js, o ramo
// que `isLegacyTvRuntime()` escolhe — e esta TV e exatamente esse caso. O 40 do
// `HOME_MAX_ROWS_DEFAULT` e do navegador de mesa.
#define CAT_FILTER_MAX 16

typedef struct {
  char key[192];   // homeCatalogKey: <addonId>_<tipo>_<catalogoId>
  char title[96];   // ja formatado, com o sufixo de tipo
  char kind[8];      // "movie" | "series"
  // DE ONDE A FILEIRA VEIO. A chave acima identifica o catalogo mas nao serve
  // para CHAMAR de novo: ela carrega o id do ADDON, nao o endereco dele.
  // Sem estes dois nao ha como pedir a continuacao da lista, que e o que a tela
  // "Ver tudo" faz — ela chama o mesmo catalogo com `skip`.
  // 600 e nao 300: o Xperience embute um JWT no CAMINHO e a base dele tem 367
  // caracteres. Com 300 ela era truncada em silencio, a URL montada aqui virava
  // outra coisa e o catalogo respondia sem `metas` — a tela "Ver tudo" abria
  // vazia sem nenhum erro. addons.c ja usa 600 pelo mesmo motivo.
  char base[600];
  char catId[96];
  // Janela no vetor de itens. As fileiras NAO tem vetor proprio: apontam para
  // o catalogo unico, que e o que a biblioteca e a busca varrem. Duplicar os
  // itens por fileira custaria ~3,5 KB por titulo repetido.
  int  start, n;
} CatRow;

int cat_n_rows(void);
const CatRow *cat_row(int r);   // NULL fora da faixa

// Troca itens E fileiras de uma vez. Tem de ser uma chamada so: com duas, o fio
// do desenho pega um quadro com as fileiras novas apontando para os itens
// velhos, e a janela (ini,n) cai fora do vetor.
void cat_set_all(const CatItem *list, int count,
                      const CatRow *filters, int nFilters);


// Substitui os episodios de UM titulo. Chamado quando o detalhe abre.
void cat_set_episodes(int indexItem, const CatEp *list, int n);

// Substitui UM item, preservando o resto. Usado quando o detalhe abre e traz
// elenco, direcao e temporadas que o catalogo da fileira nao tinha.
void cat_update_item(int index_, const CatItem *new);

// Titulos parecidos com o de `indice`: mesmo tipo (filme/serie) e pelo menos um
// genero em comum, os de nota mais alta primeiro. Devolve quantos escreveu.
//
// Nao ha endpoint de "similares" no protocolo dos addons — o Cinemeta nao tem e
// o Xperience so oferece "More Like X" para os titulos recentes do dono, nao
// para um qualquer. Cruzar genero dentro do catalogo que ja esta carregado
// responde na hora, sem rede, e acerta o suficiente para a fileira valer.
int           cat_similar(int index_, int *output, int max);

int           cat_n_episodes(int indexItem);
const CatEp  *cat_episode(int indexItem, int i);   // indice circular; NULL se o catalogo esta vazio

#endif
