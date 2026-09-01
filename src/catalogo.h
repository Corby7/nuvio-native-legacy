// Catalogo de titulos vindo de um arquivo, no lugar das listas fixas no codigo.
//
// Existe porque testar layout com nomes inventados esconde problemas reais: os
// titulos de verdade tem tamanhos muito diferentes ("CODA" contra "Assassinos
// da Lua das Flores"), acentos, e sinopses que nao cabem em tres linhas. Cada
// item tambem carrega o LOGO do titulo, que e o que o app da Apple desenha no
// lugar do nome em texto.
#ifndef NV_CATALOGO_H
#define NV_CATALOGO_H

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
  char titulo[160];
  char genero[160];    // "Programa de TV · Drama · Misterio"
  char meta[96];       // "2022 · 3 temporadas"
  char classificacao[8];
  char sinopse[900];
  // Elenco real: nome, papel e a foto (quando o TMDB tem). Sem isto a secao
  // "Elenco e equipe" fica com nomes inventados, e nomes inventados nao testam
  // o layout — os de verdade tem tamanhos que quebram a coluna.
  struct { char nome[64]; char papel[64]; char foto[512]; } elenco[6];
  int nElenco;
  char direcao[128];
  // Nota da critica em porcentagem e o logo do servico onde o titulo esta. Sao
  // as duas coisas que a linha tecnica do app da Apple mostra alem do ano e da
  // duracao — sem elas a linha fica com metade da informacao.
  int  nota;              // 0 = desconhecida
  char provLogo[512];
  char provNome[64];
  // Onde assistir alem da assinatura: aluguel e compra na regiao BR do TMDB.
  // Vazios = o servico nao oferece o titulo por esse meio aqui. O detalhe
  // encolhe a secao de acordo — um card sem dado e pior que a ausencia dele.
  char alugLogo[512];
  char alugNome[64];
  char compLogo[512];
  char compNome[64];
  // Identificador do titulo no IMDb ("tt11280740") e o tipo que os addons usam
  // ("movie"/"series"). Vem de art/ids.txt, resolvido pelo Cinemeta — sem ele
  // nao ha como perguntar fontes a addon nenhum.
  // Quanto do titulo o dono ja assistiu, 0..100. Vem do app web (chave
  // watchProgressItems), quarta coluna de extra.txt. 0 = nao comecou.
  int  progresso;
  // Legenda do card em "Continue Assistindo". Serie mostra "T1, E8 · 16 min";
  // filme mostra so o tempo que falta. Temporada/episodio ficam em 0 no filme,
  // e e isso que separa os dois casos no desenho.
  int  temporada, episodio;
  int  restanteMin;
  // Temporadas que a serie tem, na ordem. Sai do campo `videos` do Cinemeta,
  // buscado quando o titulo abre. 0 = ainda nao se sabe (ou e filme), e as
  // abas caem no padrao de 3 que existia fixo.
  int  temporadas[12];
  int  nTemporadas;
  // Vem do Trakt: 1 se esta na watchlist do dono, 1 se esta na colecao dele.
  // Ficam no item e nao numa tabela a parte da biblioteca porque o catalogo e
  // reconstruido da rede — uma tabela por indice apontaria para outro titulo
  // depois da primeira atualizacao.
  int  naLista, naColecao;
  char imdb[16];
  char tipo[8];
} CatItem;

// Um episodio de serie. Vem de art/episodios.txt, gerado a partir do campo
// `videos` do Cinemeta (/meta/series/<id>.json) — os mesmos episodios que os
// addons indexam, entao o que a tela lista e o que da para pedir fonte.
typedef struct {
  int  temporada, episodio;
  char nome[120];
  char duracao[16];    // "38 min"; vazio quando o Cinemeta nao informa
  // Data por EXTENSO, como o web: "27 de janeiro de 2023". Ele usa
  // toLocaleDateString com {month:"long", day:"numeric", year:"numeric"}
  // (metaDetailsScreen.js:1387) — "27/01/2023" era invencao do port. 16 bytes
  // nao cabiam: "15 de novembro de 2024" tem 22.
  //
  // Quem desenha encurta para so o ano quando `showFullReleaseDate` esta
  // desligado (ajustes_data_completa()); o ano sao os 4 ultimos caracteres.
  char data[40];
  char sinopse[420];
  char thumb[512];     // still do episodio; vazio cai na arte do titulo
} CatEp;

// Le <dir>/catalogo.txt. Devolve quantos itens carregou (0 = nenhum, e quem
// chama deve seguir com o que tiver).
int  cat_carregar(const char *dirArte);
int  cat_n(void);
const CatItem *cat_item(int i);

// Grava onde o dono parou NESTE app. Ate agora o progresso so era LIDO (do app
// web); sem isto, assistir pelo app nativo nao mudava nada na tela.
//
// Vai para <arte>/progresso.txt e nao para o SQLite do app web: aquele arquivo
// pertence a outro processo, que o mantem aberto e em cache — escrever la de
// fora corromperia o estado dele. Unir as duas fontes e trabalho a parte; por
// enquanto o que este app grava ganha do que veio de la, que e o certo, porque
// e mais recente.
void cat_salvar_progresso(int indice, double posSeg, double durSeg);

// Episodios do titulo `indiceItem`. Filme devolve 0 — e o que a tela usa para
// decidir se mostra a secao de episodios.
// Substitui o catalogo inteiro pelo que veio da rede. Os caminhos de arte
// passam a ser URLs — o tex_cache baixa e guarda em disco sozinho.
void cat_definir(const CatItem *lista, int n);

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
#define CAT_FIL_MAX 16

typedef struct {
  char chave[192];   // homeCatalogKey: <addonId>_<tipo>_<catalogoId>
  char titulo[96];   // ja formatado, com o sufixo de tipo
  char tipo[8];      // "movie" | "series"
  // Janela no vetor de itens. As fileiras NAO tem vetor proprio: apontam para
  // o catalogo unico, que e o que a biblioteca e a busca varrem. Duplicar os
  // itens por fileira custaria ~3,5 KB por titulo repetido.
  int  ini, n;
} CatFileira;

int cat_n_fileiras(void);
const CatFileira *cat_fileira(int r);   // NULL fora da faixa

// Troca itens E fileiras de uma vez. Tem de ser uma chamada so: com duas, o fio
// do desenho pega um quadro com as fileiras novas apontando para os itens
// velhos, e a janela (ini,n) cai fora do vetor.
void cat_definir_tudo(const CatItem *lista, int qtd,
                      const CatFileira *fils, int nFils);


// Substitui os episodios de UM titulo. Chamado quando o detalhe abre.
void cat_definir_episodios(int indiceItem, const CatEp *lista, int n);

// Substitui UM item, preservando o resto. Usado quando o detalhe abre e traz
// elenco, direcao e temporadas que o catalogo da fileira nao tinha.
void cat_atualizar_item(int indice, const CatItem *novo);

// Titulos parecidos com o de `indice`: mesmo tipo (filme/serie) e pelo menos um
// genero em comum, os de nota mais alta primeiro. Devolve quantos escreveu.
//
// Nao ha endpoint de "similares" no protocolo dos addons — o Cinemeta nao tem e
// o Xperience so oferece "More Like X" para os titulos recentes do dono, nao
// para um qualquer. Cruzar genero dentro do catalogo que ja esta carregado
// responde na hora, sem rede, e acerta o suficiente para a fileira valer.
int           cat_similares(int indice, int *saida, int max);

int           cat_n_episodios(int indiceItem);
const CatEp  *cat_episodio(int indiceItem, int i);   // indice circular; NULL se o catalogo esta vazio

#endif
