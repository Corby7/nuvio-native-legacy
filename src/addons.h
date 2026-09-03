// Ponte com os addons (protocolo Stremio) — as fontes de verdade.
//
// Um addon e uma URL base; as fontes de um titulo saem de
//   <base>/stream/<movie|series>/<id>.json
// e vem como {"streams":[{name,title|description,url,behaviorHints},...]}.
// Nao ha autenticacao propria: a chave, quando existe, ja vem embutida no
// caminho da URL do addon (por isso addons.txt e conteudo sensivel do dono e
// nao deve ir para repositorio nenhum).
//
// A busca BLOQUEIA e roda num fio proprio. O resultado entra por
// stream_definir_lista, e a tela so precisa olhar addons_estado().
#ifndef NV_ADDONS_H
#define NV_ADDONS_H

typedef enum { ADD_PARADO = 0, ADD_BUSCANDO, ADD_PRONTO, ADD_VAZIO } AddEstado;

// Le art/addons.txt (nome<TAB>url por linha). Sem ele a lista fica vazia.
int  addons_carregar(const char *dirArte);

// Lista vinda da CONTA, substituindo o arquivo. E isto que torna o pacote
// distribuivel: enquanto a lista sair de art/addons.txt, o .ipk carrega as
// chaves de debrid de quem o montou embutidas nas URLs.
//
// Uma lista VAZIA e ignorada de proposito. O servidor pode responder vazio por
// perfil errado, 401 mal tratado ou queda — e nenhum desses e "o usuario
// removeu todos os addons". Trocar por vazio deixaria a pessoa sem fonte
// nenhuma e sem entender por que.
typedef struct { char nome[64]; char url[600]; int ativo; } AddonRemoto;
int  addons_definir_lista(const AddonRemoto *lista, int n);

// Lista atual, para o sync poder empurrar de volta o que este aparelho tem.
int  addons_exportar(AddonRemoto *saida, int max);

// Esquece a lista da conta. Chamado ao SAIR: sem isto, a proxima pessoa a usar
// esta TV navega com os addons da anterior — e como as chaves de debrid vao
// embutidas nas URLs, ela tambem consome a assinatura da anterior — ate o
// primeiro sync terminar. Ficar sem fonte por alguns segundos e o
// comportamento correto de "ninguem logado".
void addons_esquecer(void);
int  addons_n(void);
const char *addons_base(int i);   // URL base, sem /manifest.json
int  addons_tem_catalogo(int i);  // 1 quando o addon fornece catalogo

// Dispara a busca das fontes de `imdb` ("tt1234567", ou "tt1234567:1:2" para
// episodio). Volta na hora; o resultado chega por stream_definir_lista.
void addons_buscar(const char *imdb, const char *tipo);

// --- legendas externas (OpenSubtitles) ---------------------------------------
// Addon de legenda responde em /subtitles/<tipo>/<id>.json com
// {"subtitles":[{lang,url,subtitleFileName,...}]}. Sao dezenas por titulo, a
// maioria em idiomas que nao interessam — por isso a lista e FILTRADA por
// idioma antes de chegar na tela: 70 linhas para rolar seria pior que nenhuma.
#define LEG_MAX 12

typedef struct {
  char rotulo[64];   // "Portugues (BR)  ·  Silo.S01E05.WEB"
  char idioma[8];
  char url[600];
} Legenda;

void addons_buscar_legendas(const char *imdb, const char *tipo);
int  addons_n_legendas(void);
const Legenda *addons_legenda(int i);

AddEstado addons_estado(void);
void addons_encerrar(void);

#endif
