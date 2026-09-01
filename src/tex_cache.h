// Cache de texturas com decode FORA da thread de desenho.
//
// Por que thread: decode medido no aparelho custa ~30ms por imagem. A 60fps o
// quadro inteiro tem 16,6ms — decodificar em linha significa perder 2 quadros
// por card que entra na tela. A thread decodifica para memoria; a thread de
// desenho so faz o upload GL (que precisa do contexto e e barato).
//
// Politica: LRU com teto de itens. Sem teto, percorrer o catalogo inteiro
// estoura a memoria do app — a TV tem orcamento apertado e ja vimos o web app
// em 266MB.
#ifndef NV_TEX_CACHE_H
#define NV_TEX_CACHE_H
#include "gl_compat.h"

int  tex_iniciar(int max_itens);

// Pasta onde as imagens vindas de URL sao guardadas em disco. Sem ela,
// tex_obter com http(s) simplesmente nao carrega — o app nao quebra, so fica
// sem arte.
void tex_cache_dir(const char *dir);
void tex_encerrar(void);

// Devolve a textura se ja estiver pronta; senao 0 e enfileira o decode.
// Nunca bloqueia a thread de desenho.
GLuint tex_obter(const char *caminho);

// Proporcao (w/h) da textura ja carregada; 0 se ainda nao esta pronta.
// Necessaria para o "cover" do shader — sem ela a arte estica.
float tex_aspecto(const char *caminho);

// Chamar uma vez por quadro, na thread de desenho: sobe para a GPU o que a
// thread de decode terminou. Devolve quantas subiu.
int tex_bombear(int max_por_quadro);

void tex_estatisticas(int *itens, int *pendentes, long *bytes);

#endif
