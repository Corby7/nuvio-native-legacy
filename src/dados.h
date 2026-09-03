// Onde o app grava o que e DO USUARIO — sessao, perfil ativo, cache de sync.
//
// Ate agora ajustes.c e catalogo.c gravavam dentro de dirArte, a pasta do
// PACOTE. Em modo desenvolvedor isso funciona e por isso passou despercebido;
// num app instalado de verdade a pasta do pacote e o lugar errado, e e a mesma
// para todo mundo que usar o aparelho. Com login, gravar ali significaria a
// sessao de uma pessoa dentro do app de outra.
//
// A pasta e DESCOBERTA, nao chutada: nao ha documentacao publica confiavel de
// onde um app NATIVO do webOS pode escrever, e um caminho fixo errado
// transforma "nao salvou nada" num defeito mudo — o app abre, parece logado, e
// no proximo arranque esqueceu tudo sem nenhuma mensagem. A sonda tenta
// escrever de verdade em cada candidato e registra no log qual venceu.
#ifndef NV_DADOS_H
#define NV_DADOS_H

// Escolhe a pasta. `dirArte` entra como ULTIMO recurso (e o comportamento de
// hoje, e e melhor que nao gravar nada). Chamar uma vez, no arranque.
void dados_iniciar(const char *dirArte);

// Pasta escolhida, sem barra no fim. Nunca NULL depois de dados_iniciar; pode
// ser "" se nenhum candidato aceitou escrita — nesse caso gravar e no-op e o
// log ja disse por que.
const char *dados_dir(void);

// Monta `dados_dir()/nome` em `dst`. Devolve dst, ou NULL se nao ha pasta.
char *dados_caminho(char *dst, unsigned tam, const char *nome);

// Grava `conteudo` em `nome` de forma atomica (temporario + rename). 1 se deu
// certo. Atomico porque perder a sessao por causa de um arquivo escrito pela
// metade e exatamente o tipo de defeito que so aparece no aparelho de outra
// pessoa.
int dados_gravar(const char *nome, const char *conteudo);

// Le `nome` inteiro para um buffer novo terminado em NUL (free pelo chamador).
char *dados_ler(const char *nome);

int dados_apagar(const char *nome);

// Identificador ESTAVEL desta instalacao, gerado na primeira execucao e
// gravado. O sync do web manda isto em `p_origin_client_id` para o servidor nao
// devolver ao aparelho a escrita que ele mesmo acabou de fazer — sem um id
// estavel, cada arranque parece um aparelho novo e o eco volta.
const char *dados_cliente_id(void);

// UUID v4 novo a cada chamada. MEDIDO contra o servidor: a RPC
// start_tv_login_session recusa com "Invalid device nonce" qualquer nonce que
// nao seja um UUID — um identificador proprio, mesmo unico, nao passa.
void dados_uuid(char *dst, unsigned tam);

#endif
