// MARCOS DE TEMPO: um carimbo nomeado, com o milissegundo em que aconteceu.
//
// Existe porque "ta muito lerdo" nao tem como ser respondido olhando a tela. O
// relatorio de FPS diz o custo de um QUADRO; o que faltava era o custo de uma
// JORNADA — do arranque ate a home ter conteudo, da tecla ate o texto assentar,
// do "Reproduzir" ate o video comecar. Sao dezenas de segundos distribuidos por
// fios diferentes, e nenhum deles aparece num numero de quadro.
//
// A saida vai para /tmp/nuvio-marcos.txt porque no aparelho a saida padrao do
// app lancado pelo applicationManager nao chega a lugar nenhum que se possa
// ler — a mesma razao que ja fez /tmp/nuvio-fps.txt existir.
//
// Barato de proposito: um fprintf por evento, e sao poucas dezenas por sessao.
// Nao instrumentar por quadro com isto.
#ifndef NV_MARCO_H
#define NV_MARCO_H

// Carimba `nome` com os ms decorridos desde marco_iniciar(). Seguro de chamar
// de qualquer fio.
void marco(const char *nome);

// Zera o relogio e recomeca o arquivo. Chamado uma vez, no inicio do main.
void marco_iniciar(void);

#endif
