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

// Segue os redirecionamentos e devolve o endereco FINAL, sem baixar o corpo.
// Serve para saber se um link de debrid leva ao arquivo ou a um video de aviso
// ("downloading.mp4", "slate.mp4") — que TOCA NORMALMENTE e por isso nao da
// erro nenhum. 1 se conseguiu resolver.
int rede_url_final(const char *url, int segundos, char *dst, unsigned tam);

// POST de JSON. Existe para o Trakt, que so aceita escrita por POST.
char *rede_postar(const char *url, int segundos, const char *const *cabecalhos,
                  const char *corpo);

#endif
