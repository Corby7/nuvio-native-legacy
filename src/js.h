// Leitura tolerante de JSON, compartilhada.
//
// Nao e um analisador completo e nao pretende ser: os formatos daqui (Stremio,
// Cinemeta, Trakt) sao conhecidos e rasos, e o que importa e NUNCA travar com
// campo faltando ou tipo inesperado. Cada funcao devolve o que achou ou nada, e
// quem chama decide. Esta logica nasceu duplicada em addons.c e video.c; virou
// modulo quando o terceiro consumidor apareceu.
#ifndef NV_JS_H
#define NV_JS_H
#include <stddef.h>

// Fim do objeto/array que comeca em `p` (que aponta para '{' ou '['),
// respeitando aspas e escapes.
const char *js_fim(const char *p);

// Valor textual de "chave" dentro de [ini,fim). 1 se achou. Escapes \uXXXX
// viram espaco de proposito: os textos vem cheios de emoji e sao so para
// exibicao — decodificar UTF-16 aqui seria trabalho sem retorno.
int js_texto(const char *ini, const char *fim, const char *chave,
             char *dst, size_t tam);

// Numero de "chave". Exige que o caractere apos a chave seja digito/sinal, o
// que evita casar com um OBJETO de mesmo nome — o caso real e
// {"currentTime":{"currentTime":8580}}, onde a primeira ocorrencia da 0.
double js_num(const char *ini, const char *fim, const char *chave, double padrao);

// Primeiro elemento do array de nome `chave`; NULL se nao houver. Avance com
// js_prox.
const char *js_array(const char *ini, const char *fim, const char *chave);

// Proximo elemento do array a partir do fim do anterior; NULL no fim.
const char *js_prox(const char *fimAnterior);

// Primeiro elemento de um array que e a RAIZ do documento. Toda RPC do
// Supabase responde `[{...},{...}]` sem chave em volta, e js_array — que
// procura por nome — nao tem o que procurar ali. Avance com js_prox.
const char *js_raiz_array(const char *corpo);

// Copia o valor de `chave` como TEXTO JSON CRU, com as chaves e colchetes.
// Existe para o `credential_json` das credenciais: o app repassa aquele objeto
// ao servidor sem interpretar, e reconstrui-lo campo a campo perderia tudo que
// esta versao do app nao conhece. 1 se achou e coube.
int js_bruto(const char *ini, const char *fim, const char *chave,
             char *dst, size_t tam);

#endif
