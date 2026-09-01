// Guia parental do titulo em reproducao — o painel que o app web mostra no
// canto superior esquerdo do player quando os controles aparecem
// (.player-parental-guide). Sao ate cinco linhas "Categoria · Gravidade" com
// uma barra vertical na cor de destaque a esquerda.
//
// O port desenhava ali outra coisa: um selo de classificacao com o GENERO do
// titulo ao lado, que nao existe no web. Genero nao e advertencia de conteudo.
#ifndef NV_PARENTAL_H
#define NV_PARENTAL_H

#define PG_MAX 5

// Pede o guia de `imdb` (formato "tt1234567"). Nao bloqueia: dispara um fio e
// devolve na hora. Chamar de novo com o MESMO id nao refaz o pedido.
void parental_pedir(const char *imdb);

// Quantas linhas ja chegaram (0 enquanto busca, ou quando o titulo nao tem
// dado). `rotulo` e a categoria traduzida, `gravidade` o nivel.
int  parental_n(void);
const char *parental_rotulo(int i);
const char *parental_gravidade(int i);

#endif
