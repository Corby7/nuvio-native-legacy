// Sincronizacao com a conta: e isto que faz o app nativo se comportar como o
// oficial na TV de outra pessoa.
//
// O QUE ELE SUBSTITUI: hoje os addons e o token do Trakt sao arquivos de texto
// dentro do pacote (art/addons.txt, art/trakt.txt), o que torna o .ipk
// indistribuivel — ele entrega as credenciais de quem o montou. Depois deste
// modulo, os dois vem da conta de quem logou.
//
// ONDE ELE ESCREVE E ONDE NAO ESCREVE — a distincao mais importante do arquivo:
//
//   PUXA E EMPURRA (o app tem a informacao de verdade):
//     addons, credenciais (Trakt, TMDB, mdblist), progresso de reproducao.
//
// MEDIDO na conta do dono: sync_pull_provider_credentials devolve animeskip,
// debrid:premiumize, debrid:realdebrid, debrid:torbox, introdb, mdblist e tmdb
// — e NENHUM "trakt". O leitor de trakt fica aqui porque a RPC e a mesma e a
// linha aparece sozinha assim que o app web a escrever; ate la o vinculo Trakt
// deste app continua saindo de art/trakt.txt, que NAO pode ir no pacote.
//   SO PUXA (o app le, mas nao tem edicao local para empurrar):
//     perfis, vistos, biblioteca, salvos, colecoes, ajustes do perfil,
//     catalogos da home.
//
// Isto NAO e preguica, e a regra de seguranca numero 2 da secao 1.6 do plano.
// Empurrar uma superficie que o app nao edita significaria mandar uma lista
// VAZIA para o servidor, e uma lista vazia apaga o que existe nos outros
// aparelhos da pessoa. Empurrar so o que o app realmente possui e a unica
// forma segura de participar de um sync bidirecional sem ter todas as telas.
//
// Todas as chamadas de rede acontecem num fio proprio. A UI so consulta estado.
#ifndef NV_SYNC_H
#define NV_SYNC_H

typedef enum {
  SYNC_PARADO = 0,
  SYNC_RODANDO,
  SYNC_PRONTO,
  SYNC_FALHOU
} SyncEstado;

// Dispara um ciclo completo (puxa e, onde faz sentido, empurra). Volta na hora.
// Idempotente enquanto um ciclo estiver em andamento.
void sync_iniciar(void);

SyncEstado  sync_estado(void);
const char *sync_resumo(void);   // uma linha para a tela de ajustes

// Marca uma superficie como suja: o proximo ciclo empurra. Chamar quando o
// usuario mexe em algo local.
void sync_sujar_progresso(void);
void sync_sujar_addons(void);

// Ultimo instante em que um ciclo terminou bem (SDL_GetTicks); 0 se nunca.
unsigned sync_ultimo_ok(void);

// Intervalo entre ciclos automaticos. Ate agora o sync so rodava no arranque,
// depois do login e ao trocar de perfil — entao parar um episodio no celular
// nao aparecia na TV sem fechar e reabrir o app, que e o oposto do que a conta
// promete.
//
// 5 minutos, e nao 30 segundos: o ciclo sao ~8 requisicoes (perfis, travas,
// addons, credenciais, progresso e as so-leitura). A TV fica LIGADA horas na
// mesma tela, entao um intervalo curto vira um martelo constante no backend —
// e ja houve um episodio de estouro de cota neste projeto em que o efeito
// colateral (login impossivel, sessao anonima, sync da conta errada) pareceu
// bug do app. O ciclo tambem so roda com o app em uso, nunca durante o player.
#define SYNC_INTERVALO_MS 300000u

// Chamar uma vez por quadro. Nao bloqueia: so recolhe o resultado do fio e
// aplica no app (lista de addons, credencial do Trakt, progresso).
void sync_passo(unsigned agoraMs);

// Dispara um ciclo se ja passou SYNC_INTERVALO_MS desde o ultimo que deu certo.
// Nao roda com um ciclo em andamento, com o freio ativo, nem antes do primeiro
// sucesso. 1 quando disparou.
int  sync_periodico(unsigned agoraMs);

// Apaga do aparelho tudo que pertence a quem estava logado. Chamar JUNTO com
// sessao_sair() — a sessao sozinha nao basta.
//
// O defeito que isto conserta: sair da conta apagava o token e mais nada. A
// lista de addons continuava em memoria (com as chaves de debrid embutidas nas
// URLs), o token do Trakt continuava valido e ESCREVENDO o que a proxima
// pessoa assistisse na conta de quem saiu, o perfil ativo continuava gravado —
// entao o primeiro sync da conta seguinte escreveria progresso no
// `p_profile_id` da anterior — e o progresso.txt da anterior seguia no disco.
//
// Numa TV de sala, "sair" e a unica barreira entre duas pessoas. Ela tem de
// apagar de verdade.
void sync_esquecer_usuario(void);

// Faz o PROXIMO ciclo reaplicar os ajustes vindos da conta. Chamar ao entrar e
// ao trocar de perfil.
//
// Por que nao aplicar em TODO ciclo: o app nativo le os ajustes do perfil mas
// nao os escreve de volta. Reaplicar sempre desfaria, na volta seguinte, tudo
// que a pessoa mudasse na propria TV — ela mexeria numa opcao e veria a opcao
// voltar sozinha. Aplicando so na primeira volta depois de entrar (ou de trocar
// de perfil), a conta define o ponto de partida e a mudanca local vale pelo
// resto da sessao.
void sync_reaplicar_ajustes(void);

void sync_encerrar(void);

#endif
