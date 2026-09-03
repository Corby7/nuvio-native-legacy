// Busca HTTP(S) simples, para memoria.
//
// Usa a libcurl DO APARELHO por dlopen. Escrever TLS a mao estava fora de
// cogitacao e o SDK nao traz libcurl para linkar — mas /usr/lib/libcurl.so.5
// existe na TV, e os addons so falam https. No Mac usa a libcurl do sistema.
#ifndef NV_REDE_H
#define NV_REDE_H

// Baixa `url` inteiro para um buffer novo (terminado em NUL) e devolve-o; o
// chamador libera com free(). NULL em qualquer falha. BLOQUEIA — chamar de um
// fio proprio, nunca do laco de desenho.
char *rede_baixar(const char *url, int segundos);

// Igual, mas para conteudo BINARIO: devolve o tamanho em *n. A versao acima
// termina em NUL e serve para JSON; imagem tem zeros no meio e strlen mentiria.
char *rede_baixar_bin(const char *url, int segundos, long *n);

// Com cabecalhos. `cabecalhos` e um vetor terminado em NULL de linhas prontas
// ("Authorization: Bearer x"). Existe por causa do Trakt, que exige token e
// chave de aplicativo em cabecalho — nao ha como passar por URL.
char *rede_baixar_com(const char *url, int segundos, const char *const *cabecalhos);

// Baixa SO UM TRECHO, por cabecalho Range. Devolve o tamanho em *tam.
//
// Existe para ler o cabecalho de um MKV sem puxar o arquivo inteiro: o
// pipeline da LG devolve "language":"(null)" em TODA faixa de legenda (medido
// num arquivo de 43 legendas — o audio vem com idioma, a legenda nao), e a
// unica forma de saber o idioma e ler o proprio container, que e o que o
// navegador faz por conta propria no app web.
//
// Servidor que ignora o Range devolve o arquivo inteiro; por isso quem chama
// tem de estar preparado para receber MAIS do que pediu, e parar de ler quando
// achou o que queria.
char *rede_baixar_trecho(const char *url, int segundos, long ini, long fim,
                         long *tam);

// Teto de bytes da transferencia corrente (0 = sem teto). E interno ao modulo;
// esta exposto so porque rede_baixar_trecho o usa. Nao mexer de fora.
extern long rede_teto;

// Segue os redirecionamentos e devolve o endereco FINAL, sem baixar o corpo.
// Serve para saber se um link de debrid leva ao arquivo ou a um video de aviso
// ("downloading.mp4", "slate.mp4") — que TOCA NORMALMENTE e por isso nao da
// erro nenhum. 1 se conseguiu resolver.
int rede_url_final(const char *url, int segundos, char *dst, unsigned tam);

// POST de JSON. Existe para o Trakt, que so aceita escrita por POST.
char *rede_postar(const char *url, int segundos, const char *const *cabecalhos,
                  const char *corpo);

// Igual, mas devolve o CODIGO HTTP em *status (0 quando a requisicao nem saiu).
// Existe por causa do Supabase: la o 401 nao e falha, e a instrucao para
// renovar o token e repetir. Sem o codigo na mao, "sessao vencida" e
// "servidor fora do ar" chegam identicos — como NULL — e o app ou perde a
// sessao a toa ou entra em laco de retry contra um erro que nao passa.
// O corpo do erro tambem volta: o PostgREST explica no corpo qual funcao ou
// tabela nao existe, e essa string e o que distingue "servidor antigo" de
// "parametro errado".
char *rede_postar_st(const char *url, int segundos, const char *const *cabecalhos,
                     const char *corpo, int *status);

// GET com cabecalhos E codigo HTTP. Pelo mesmo motivo do POST acima: a leitura
// de tabela do Supabase precisa distinguir "tabela nao existe" (404 com
// PGRST205 no corpo) de "sem rede", porque a primeira significa cair para a
// tabela seguinte e a segunda significa nao mexer em nada.
//
// ATENCAO: ao contrario de rede_baixar_com, este NAO transforma 4xx em NULL.
// O corpo de erro e justamente o que o chamador quer ler.
char *rede_baixar_st(const char *url, int segundos, const char *const *cabecalhos,
                     int *status);

// Carrega a libcurl AGORA, no fio que chamar. Existe para o arranque fazer isso
// no fio principal, antes de qualquer fio de rede nascer: `curl_global_init`
// nao e seguro entre fios, e a trava interna e a segunda linha de defesa, nao a
// primeira. Chamar mais de uma vez nao custa nada.
void rede_preparar(void);

#endif
