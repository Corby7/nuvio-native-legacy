# Conta e sincronizacao no app nativo — paridade com o app web

Objetivo (DECIDIDO PELO DONO, rota B): outra pessoa instala o `.ipk`, faz login na
propria conta e o aparelho passa a se comportar como o app oficial — mesmos
addons, mesmo Trakt, mesmo debrid, mesmo progresso, mesmos perfis, e o que ela
assiste aqui aparece nos outros aparelhos dela.

Hoje o nativo e um app de UM dono: toda credencial e um arquivo de texto dentro
do pacote (`art/trakt.txt`, `art/addons.txt`, `art/tmdb.txt`). O `.ipk` atual NAO
pode ser distribuido — ele entrega o token Trakt e as chaves de debrid do dono
para quem instalar. Este documento e o caminho para trocar esses arquivos por uma
sessao de verdade.

Fonte de tudo que esta aqui: leitura do `NuvioWeb-0.3.38-beta` (`js/core/auth/`,
`js/core/profile/`, `js/data/remote/supabase/supabaseApi.js`). Onde o texto diz
FATO, foi lido no codigo do web; onde diz SUPOSICAO, ainda nao foi medido contra
o servidor.

---

## Parte 1 — O contrato de rede

### 1.1 Transporte

FATO: todo o sync do web passa por UM unico formato — `POST` em
`<SUPABASE_URL>/rest/v1/rpc/<funcao>` com corpo JSON, cabecalhos:

    apikey: <SUPABASE_ANON_KEY>
    Authorization: Bearer <access_token da sessao, ou o proprio anon key>
    Content-Type: application/json

A resposta e sempre um array JSON de linhas (ou um objeto, nas RPC de blob).
Nao ha WebSocket, nao ha `postgrest` cru no caminho feliz — o acesso direto a
tabela (`tv_profiles`, `tv_addons`, `plugins`, `addons`) so aparece como FALLBACK
quando a RPC responde "funcao nao encontrada" (PGRST202) ou "tabela nao
encontrada" (PGRST205). O nativo implementa o caminho da RPC e trata o fallback
como "essa superficie nao esta disponivel neste servidor", sem tentar reproduzir
as duas escadas de compatibilidade do web.

CONSEQUENCIA PRATICA: o nativo nao precisa de biblioteca de Supabase. Precisa de
POST com cabecalhos (ja existe em `rede_postar`), leitura de JSON (ja existe em
`js.c`) e ESCRITA de JSON (nao existe — modulo novo `jsw.c`).

### 1.2 Login de TV (sem teclado)

FATO, `js/core/auth/qrLoginService.js` e `authManager.js`:

1. **Sessao anonima primeiro.** As RPC de login rodam autenticadas como um
   usuario anonimo; sem isso o servidor responde 401. `ensureQrSessionAuthenticated`.
2. `POST /rest/v1/rpc/start_tv_login_session`
   `{ p_device_nonce, p_redirect_base_url, p_device_name }` -> devolve o `code`.
   O `p_device_name` e omitido e a chamada repetida quando o servidor recusa o
   parametro (servidor antigo).
3. `POST /rest/v1/rpc/poll_tv_login_session` `{ p_code, p_device_nonce }` ->
   `status`. Enquanto for pendente, repetir.
4. `POST /functions/v1/tv-logins-exchange` `{ code, device_nonce }` ->
   `access_token` + `refresh_token`. E o unico endpoint fora de `/rest/v1/`.

O `p_redirect_base_url` sai de `TV_LOGIN_WEB_BASE_URL`: e a pagina que a pessoa
abre no celular. O QR do web codifica essa URL com o codigo.

#### MEDIDO contra `https://api.nuvio.tv` (03/09/2026)

Tres coisas que o codigo do web nao dizia e que so a chamada real mostrou:

```
start_tv_login_session ->
[{"code":"fa0010cad8b5d2f512e58646ab82ca6b",
  "web_url":"https://nuvio.tv/tv-login?code=fa0010cad8b5d2f512e58646ab82ca6b",
  "expires_at":"2026-09-03T03:49:55Z","poll_interval_seconds":3}]
poll_tv_login_session  -> [{"status":"pending", ...}]
```

1. **O `p_device_nonce` TEM de ser um UUID.** Um identificador proprio, mesmo
   unico, devolve `400 {"message":"Invalid device nonce"}`.
2. **`p_redirect_base_url` vazia e recusada** (`Invalid TV login redirect base
   URL`). Sem `TV_LOGIN_WEB_BASE_URL` configurada nao existe login.
3. **A resposta ja traz `web_url` pronta e `poll_interval_seconds`** (3, nao os
   2 do web). Usar os dois evita montar a URL a mao e martelar o servidor.
4. O poll com nonce errado devolve 400 `Invalid TV login session` — a sessao e
   mesmo amarrada ao aparelho que pediu.

#### DECISAO REVISTA: o QR e obrigatorio

O plano dizia "mostra o codigo em letras grandes, QR depois". A medicao derrubou
isso: **o codigo tem 32 digitos hexadecimais**. Ninguem le isso da TV e digita no
celular sem errar. O QR deixou de ser enfeite e virou o unico caminho de
entrada — por isso `qr.c` entrou na Fase A e nao numa fase de polimento.

### 1.3 Renovacao e expiracao

FATO: o web checa `exp` do JWT com 30s de folga e usa o anon key como Bearer
quando o token esta vencido. A renovacao propria do Supabase e
`POST /auth/v1/token?grant_type=refresh_token` com `{ refresh_token }`.

REGRA no nativo: `nuvem_rpc` tenta uma vez; em **401**, renova e repete UMA vez.
Se a renovacao falhar, a sessao vira deslogada e a UI volta para a tela de login —
nunca fica num limbo em que o app parece logado e nada sincroniza.

### 1.4 As RPC, por superficie

MEDIDO contra o servidor (03/09/2026) — a tabela abaixo veio do app web, e
nem tudo que ela promete existe:

- `sync_pull_addons` **NAO EXISTE** (PGRST202) e a tabela `tv_addons` tambem
  nao (PGRST205). Addons so saem da tabela `addons`, filtrada por
  `user_id` + `profile_id` e ordenada por `sort_order`.
- Ler a tabela `addons` com a chave ANONIMA devolve
  `401 permission denied for table addons`: o RLS precisa do token do usuario.
  Foi um defeito real neste codigo — a leitura ia sem o token e voltava vazia,
  o que se parecia com "a conta nao tem addons".
- `sync_pull_watched_items` usa `p_page` comecando em **1**. Com 0 responde
  `400 OFFSET must not be negative`.
- `sync_pull_saved_library` **NAO EXISTE** neste servidor.
- `get_sync_owner` devolve uma **string JSON crua** (`"441bf572-…"`), nao um
  objeto — e o id do DONO, que pode nao ser o `sub` de quem logou.

| Superficie | pull | push | delete | parametros alem de `p_profile_id` |
|---|---|---|---|---|
| Perfis | `sync_pull_profiles` | `sync_push_profiles` | `sync_delete_profile_data` | push leva `p_client_max_profiles`, `p_profiles` |
| Travas de perfil | `sync_pull_profile_locks` | `set_profile_pin` / `clear_profile_pin` | — | `verify_profile_pin` valida |
| Addons | `sync_pull_addons` | `sync_push_addons` | — | `p_addons: [{url, sort_order, enabled, name?}]` |
| Plugins | (le da tabela `plugins`) | `sync_push_plugins` | — | `p_plugins`, `p_origin_client_id` |
| Biblioteca | `sync_pull_library` | `sync_push_library` | — | — |
| Biblioteca salva | `sync_pull_...` paginado | `sync_push_...` | — | `p_limit`, `p_offset`, `p_items` |
| Colecoes | `sync_pull_collections` | `sync_push_collections` | — | `p_collections_json` |
| Progresso | `sync_pull_watch_progress` | `sync_push_watch_progress` | `sync_delete_watch_progress` | `p_entries`, `p_keys`, `p_origin_client_id` |
| Vistos | `sync_pull_watched_items` | `sync_push_watched_items` | `sync_delete_watched_items` | `p_page`, `p_page_size`, `p_items`, `p_keys` |
| Ajustes do perfil | `sync_pull_profile_settings_blob` | `sync_push_profile_settings_blob` | — | `p_platform: "tv"`, `p_settings_json` |
| Catalogos da home | `sync_pull_home_catalog_settings` | `sync_push_home_catalog_settings` | — | `p_platform: "home_catalog_shared"` |
| Credenciais (debrid) | `sync_pull_provider_credentials` | `sync_push_provider_credentials` | `sync_delete_provider_credentials` | `p_credentials: [{provider, credential_json}]` |
| Trakt | mesma RPC de credenciais | idem | idem | `provider = "trakt"` |
| Simkl | mesma RPC de credenciais | idem | idem | `provider = "simkl"` |

FATO IMPORTANTE: Trakt, Simkl e debrid compartilham as MESMAS tres RPC de
credenciais, distinguidos pelo campo `provider`. Uma implementacao serve as tres.

### 1.5 Formato das linhas

Perfil (`mapProfileRow`): `profile_index`, `name`, `avatar_color_hex`,
`avatar_id`, `avatar_url`, `profile_background_id`, `profile_background_url`,
`uses_primary_addons`, `uses_primary_plugins`, `is_primary`.

Addon: `{ url, sort_order, enabled, name? }`.

Plugin: `{ url, name, enabled, sort_order, repo_type }` — `repo_type` so aceita
`nuvio_js` e `external_dex`.

Progresso: `{ content_id, content_type, video_id, season, episode, position,
duration, last_watched, progress_key }`. Na LEITURA aceitar tambem
`position_ms`/`duration_ms` e `updated_at`, e tratar `last_watched` menor que
1e12 como SEGUNDOS (o web multiplica por 1000).

Visto: `{ content_id, content_type, title, season, episode, watched_at }`.
Chave de remocao: `{ content_id, season?, episode? }`.

Item salvo: `{ content_id, content_type, name, poster, poster_shape, background,
description, release_info, imdb_rating, genres, addon_base_url, added_at }`.

Credencial: `{ provider, credential_json }` — `credential_json` e um OBJETO, e a
leitura tem de aceitar tambem string JSON (o web faz `JSON.parse` quando vem
string).

### 1.6 Regras de seguranca do sync que NAO sao opcionais

Estas estao escritas no codigo do web com comentario explicando; repetir no
nativo, porque cada uma existe por causa de perda de dados real:

1. **Lista remota vazia nao e delecao.** Se o pull devolve zero itens e o local
   tem itens, MANTER o local e nao avancar a linha de base. Uma resposta vazia
   pode ser perfil errado, 401 mal tratado ou queda do servidor.
2. **Push vazio nao apaga.** Delecao so pelo RPC de delete explicito, com chaves.
3. **Nunca omitir linha desconhecida no push.** O `pluginSyncService` recusa o
   push inteiro quando ha linha de tipo que ele nao sabe representar — omitir
   viraria delecao silenciosa.
4. **`p_origin_client_id`** identifica este aparelho para o servidor nao ecoar de
   volta a propria escrita. Gerar um id estavel por instalacao e persisti-lo.
5. **Backoff.** Falha de sync entra em `syncBackoffPolicy` e todas as superficies
   param juntas. Sem isso, um servidor fora do ar vira uma tempestade de retry.

---

## Parte 2 — Modulos novos no nativo

| Arquivo | Responsabilidade |
|---|---|
| `jsw.c/h` | ESCRITA de JSON: escape correto, objetos, arrays. `js.c` so le. |
| `nuvem.c/h` | Transporte Supabase: `nuvem_rpc`, status HTTP, 401 -> renova -> repete uma vez, backoff comum. |
| `sessao.c/h` | Tokens em disco, sessao anonima, login de TV (start/poll/exchange), renovacao, `sub`/`exp` do JWT, sair. |
| `login.c/h` | Tela de login: codigo grande, URL, estado, erro. |
| `perfis.c/h` | Lista de perfis, perfil ativo, PIN. |
| `sync.c/h` | As superficies, na ordem do `startupSyncService`, num fio proprio. |
| `dados.c/h` | Onde grava: descobre um diretorio GRAVAVEL e e a unica resposta a essa pergunta no app. |
| `qr.c/h` | Gerador de QR (byte, nivel L, versoes 1-6). Obrigatorio, ver 1.2. |

### O QR nao pode ser conferido no olho

Um QR errado NAO da erro: ele desenha, os localizadores ficam no lugar, a tela
fica bonita, e nenhum celular decodifica. Aconteceu aqui — a ordem dos bits do
formato estava invertida e a segunda copia mal mapeada, e nada disso e visivel.
Por isso `tools/qr_conferir.py` DECODIFICA (OpenCV) em vez de comparar com outra
implementacao: duas matrizes podem diferir e as duas serem validas, entao
comparar matriz nao prova nada. A pergunta util e uma so: um leitor le?

### Onde gravar

PROBLEMA MEDIDO NO CODIGO ATUAL: `ajustes.c` e `catalogo.c` gravam dentro de
`dirArte`, que e a pasta do pacote. Em modo desenvolvedor isso funciona; num app
instalado de verdade e o lugar errado, e e o mesmo diretorio para todos os
usuarios do aparelho.

`dados.c` resolve isso com uma SONDA, nao com um palpite: tenta na ordem
`$NUVIO_DADOS`, `$HOME/.nuvio`, `/media/developer/temp/nuvio`, `dirArte`, criando
e escrevendo um arquivo de teste em cada, e usa o primeiro que aceitar. O
caminho escolhido vai para o log. Isto e deliberado: nao ha documentacao publica
confiavel de onde um app NATIVO do webOS pode gravar, e chutar um caminho fixo
transformaria "nao salvou nada" num defeito mudo.

### Segredos de compilacao

`SUPABASE_URL` e `SUPABASE_ANON_KEY` sao os mesmos que o app web ja publica no
bundle (a anon key e publica por projeto — e o que autoriza o RLS a decidir).
Entram por `-D` na compilacao, geradas de `local.properties` por `tools/env.sh`,
e nunca escritas em arquivo de codigo versionado. Para desenvolvimento, um
`art/nuvem.txt` sobrescreve em tempo de execucao.

---

## Parte 3 — Ordem de execucao

- **Fase A — FUNDACAO. FEITA (nao verificada no aparelho).** `jsw`, `dados`,
  `nuvem`, `sessao`, `qr`, tela de login.
  VERIFICADO no Mac: sessao anonima e pedido de codigo contra o servidor de
  producao; QR decodificado por leitor de verdade a partir da CAPTURA DA TELA do
  app (`https://nuvio.tv/tv-login?code=882d18…`), nao so do gerador.
  NAO VERIFICADO: a troca final pelo token (exige alguem autorizar no celular),
  a renovacao por refresh token, e qual pasta o aparelho aceita para gravar — a
  sonda de `dados.c` decide isso no arranque e escreve no log.
- **Fase B — PERFIS. FEITA.** `perfis.c` (pull de perfis + travas + dono +
  perfil ativo persistido) e `perfilsel.c` (tela "Quem está assistindo?", com
  teclado de PIN validado NO SERVIDOR — guardar o PIN no aparelho anularia o
  proposito de um perfil travado).
- **Fase C — O QUE FAZ O APP FUNCIONAR. FEITA.** VERIFICADO no app rodando:
  `[addons] 2 vindos da conta` — a lista da conta substituiu a do arquivo. A
  credencial do Trakt sai da mesma RPC de credenciais (provider `trakt`); o
  client id e do APLICATIVO e vem compilado (`-DNV_TRAKT_CLIENT_ID`).
- **Fase D — CONTINUIDADE. FEITA (o pull nao pode ser verificado sem conta com
  dado).** Progresso puxado e empurrado, com temporada/episodio separados do
  `content_id`. O player marca `sync_sujar_progresso()` ao fechar.
- **Fase E — RESTO. PUXADO, NAO EMPURRADO — e isso e deliberado.** Biblioteca,
  vistos, colecoes, ajustes do perfil e catalogos da home sao lidos e contados
  no resumo, mas NAO recebem push. O app nativo nao tem tela que edite nenhum
  deles, entao o unico push possivel seria uma lista vazia — e lista vazia
  apaga o dado nos outros aparelhos da pessoa (regra 2 da secao 1.6).
  Participar do sync bidirecional so nas superficies que o app realmente possui
  e a unica forma segura de fazer isto sem ter todas as telas.
- **Fase F — SOLTAR. FEITA no codigo; falta a montagem do pacote.**
  `home_iniciar` devolvendo 0 nao derruba mais o app (era o comportamento da
  PRIMEIRA execucao de todo mundo num pacote sem arte: abria e fechava antes da
  tela de login); ha estado vazio "Preparando seu catálogo…"; "Sair da conta"
  entrou em Ajustes > Conta, junto do perfil ativo e do resumo do sync.
  FALTA, e e decisao de quem monta o `.ipk`: **nao incluir `art/addons.txt`,
  `art/trakt.txt` nem `art/tmdb.txt`**. O codigo ja nao depende deles — eles
  seguem sendo lidos apenas como reserva de desenvolvimento — mas enquanto
  estiverem dentro do pacote, o `.ipk` continua entregando as credenciais de
  quem o montou.

---

## Parte 4 — MIGRADO para o repositorio certo (03/09/2026)

A migracao para `nuvio-native-legacy` **foi feita e verificada**. O que segue
descreve o que foi movido e as diferencas que o legacy impos. O texto original
do plano vem depois, como registro.

### Diferencas que o legacy impos (nao estavam previstas)

1. **`rede.h` do legacy JA DECLARAVA `rede_postar_st` e `rede_baixar_st`** — com
   os meus comentarios — mas o `rede.c` **nao implementava nenhuma das duas**.
   Declaracao sem corpo nao quebra o build enquanto ninguem chama. Implementei.
2. **O `rede_baixar_interno` do legacy MATA 4xx**: devolve NULL e loga. Para o
   Supabase isso apagaria a unica pista (`PGRST202`/`PGRST205` vem no corpo do
   404). A implementacao nova pula esse corte **so quando `status` foi pedido**.
3. **Colisao de tipo:** o `trakt.h` do legacy ja usa `Perfil`. Renomeei o meu
   para `ContaPerfil` / `CONTA_PERFIL_MAX`.
4. **Colisao de tela:** o legacy ja tem `TELA_PERFIL` (estatisticas do Trakt).
   A minha virou `TELA_ESCOLHA_PERFIL`.
5. **O legacy ja tinha `cat_indice_por_imdb`, e melhor que a minha** (compara
   ate o `:` sem calcular tamanho). Mantida a dele; portei so `cat_dir_gravacao`.
6. **O legacy tem `cat_salvar_progresso_ep`** (com temporada e episodio). O
   `sync.c` de la passou a usar essa versao: sem ela, o progresso de uma serie
   perderia em qual episodio a pessoa parou.
7. **O `js.c` do legacy tem uma correcao que o meu nao tem** (`js_num` aceitando
   numero entre aspas, que e como o Cinemeta manda `imdbRating`). Por isso o
   arquivo foi **acrescido**, nunca sobrescrito.
8. **O `ajustes.c` do legacy e outro programa** (10 secoes, tipos `OP_ESCOLHA`/
   `OP_NUMERO`/`OP_LEITURA` com macros). Acrescentei `OP_ACAO` e a macro
   `ACAO(...)` no padrao dele, em vez de copiar a minha versao.

### Verificacao no legacy

| Passo | Resultado |
|---|---|
| Build de referencia ANTES de tocar em nada | 0 erro, **0 aviso** |
| Build depois da migracao | 0 erro, **0 aviso** — nenhum aviso novo |
| `tools/qr_conferir.py` | TODOS LEGIVEIS |
| App aberto sem sessao | abre na tela de login |
| QR decodificado **da captura de tela do legacy** | `https://nuvio.tv/tv-login?code=ea053e96…` |
| Ciclo de sync completo | `[addons] 2 vindos da conta`, aplicados na lista do app |
| Log | `[dados] gravando em /tmp/nvl-dados`, `[nuvem] https://api.nuvio.tv` |

Nenhum commit, nenhum stash, nenhum checkout: as 85 alteracoes nao commitadas do
dono seguem intactas. O contador foi de 85 para 109 (21 arquivos novos + 3 que
ainda estavam limpos).

---

## Parte 4 (registro) — por que este codigo nasceu no repositorio ERRADO

A Fase A foi escrita em `nuvio-native/`, que esta parado: 10.8 mil linhas e
NENHUM commit. O projeto vivo e `nuvio-native-legacy/` — 22 mil linhas, commits
ate 01/09/2026, e modulos que aqui nem existem (`parental`, `colecoes`,
`extras`, `episodios`, `badges`, `mkv`, `vertudo`, `pessoa`, `marco`,
`ctxmenu`).

A migracao e barata porque os modulos novos quase nao dependem do app:

- COPIAR sem alteracao: `jsw.[ch]`, `dados.[ch]`, `nuvem.[ch]`, `sessao.[ch]`,
  `qr.[ch]`, `login.[ch]`, `perfis.[ch]`, `perfilsel.[ch]`, `sync.[ch]`,
  `tools/env.sh`, `tools/qr_conferir.py`, `tools/qr_despejo.c`.
- APLICAR em `rede.[ch]`: `rede_postar_st` e `rede_baixar_st` (POST e GET que
  devolvem o codigo HTTP). O `rede.c` do legacy tem coisa a mais
  (`rede_baixar_trecho`, `rede_teto`) — aplicar o trecho, nao trocar o arquivo.
- APLICAR em `js.[ch]`: `js_raiz_array` (as RPC respondem um array na RAIZ, e
  `js_array` so procura array por NOME) e `js_bruto` (valor JSON cru, para o
  `credential_json` passar inteiro sem ser reconstruido campo a campo).
- APLICAR em `addons.[ch]`: `addons_definir_lista` e `addons_exportar`.
- APLICAR em `trakt.[ch]`: `trakt_definir`.
- APLICAR em `catalogo.[ch]`: `cat_dir_gravacao` e `cat_indice_por_imdb`.
- APLICAR em `player.c`: `sync_sujar_progresso()` ao lado do `trakt_marcar`.
- APLICAR em `ajustes.c`: a secao "Conta" (perfil, resumo do sync, sair) e o
  campo `acao` na struct `Opcao` — linha de acao responde ao OK e tem de ter o
  mesmo destaque de quem muda, senao o usuario aperta OK esperando que nada
  aconteca.
- LIGAR: `TELA_LOGIN` e `TELA_PERFIS` no `app.h`, os desvios no `app.c`
  (evento, atualizar, desenhar, estado vazio da home) e a ordem
  `dados_iniciar` / `nuvem_configurar` / `sessao_iniciar` /
  `perfis_carregar_ativo` antes de `app_iniciar` no `main.c`, com
  `cat_dir_gravacao(dados_dir())` depois.
- CONFERIR: `js.h` era IDENTICO nos dois antes destas adicoes, e `gfx_cor`,
  `gfx_rect`, `gfx_tex_aspect_atual`, `GFX_SNAP`, `txt_linha`,
  `txt_linha_corta`, `txt_desenhar_alpha`, `anim_mola`, `js_texto`, `js_num`
  existem todos no legacy com o mesmo nome. Nao ha adaptacao a fazer.

O `ajustes_dir` do legacy tambem grava dentro da pasta do pacote e deve passar a
usar `dados_dir()` na migracao.

---

## Parte 4b — Depois da migracao (03/09/2026)

Duas pendencias fechadas, as duas de VAZAMENTO ENTRE PESSOAS.

### O `.ipk` levava credencial (fechado)

`tools/arm.sh` fazia `ares-package deploy/app` — a pasta INTEIRA. Dentro de
`art/` viajavam `trakt.txt` (token), `addons.txt` (URLs com a chave do debrid
embutida), `tmdb.txt`, `mdblist.txt` (modo 0600) e `ajustes.txt` (o layout de
quem montou). Agora o empacotamento sai de uma COPIA limpa e o script CONFERE o
pacote pronto, abortando e apagando o `.ipk` se algum voltar.

ARMADILHA MEDIDA, que quase deixou a conferencia inutil: o `.ipk` e um pacote
Debian. `tar tzf pacote.ipk` lista SEM ERRO apenas `debian-binary`,
`control.tar.gz` e `data.tar.gz` — nunca os arquivos do app. Uma conferencia
escrita assim passa sempre, inclusive com o segredo dentro. E preciso
desempacotar o `ar` e listar o `data.tar.gz`.

`tools/testa-ipk.sh` prova isso sem docker, e foi conferido NOS DOIS SENTIDOS:
com a exclusao o pacote sai limpo; deixando `trakt.txt` entrar de proposito, o
teste acusa e devolve 1.

### Sair da conta nao apagava o usuario (fechado)

`sessao_sair()` apagava o token e mais nada. Ficavam para a proxima pessoa:

- a lista de addons em memoria — e com ela as chaves de debrid embutidas nas
  URLs, ou seja, a assinatura de quem saiu;
- o token do Trakt, que continuaria ESCREVENDO na conta de quem saiu o que a
  proxima pessoa assistisse;
- o perfil ativo gravado, entao o primeiro sync da conta seguinte escreveria
  progresso no `p_profile_id` da anterior;
- o `progresso.txt` da anterior, no disco.

Numa TV de sala, "sair" e a unica barreira entre duas pessoas. Agora existe
`sync_esquecer_usuario()`, chamado junto de `sessao_sair()`, e ele tambem zera
as caixas que o fio de sync preenche — senao um ciclo terminado logo antes do
logout reaplicaria os addons da conta anterior no `sync_passo` seguinte.

`tests/conta.sh` monta um estado real de usuario logado (2 addons vindos da
conta, Trakt ligado, perfil 3 escolhido, progresso no disco), sai, e confere
dez itens. Passa nos dez.

### Ajustes do perfil: aplicados (fechado)

O `ajustes.c` tem ~40 opcoes cujas chaves sao as MESMAS do app web
(`heroSectionEnabled`, `continueWatchingCardStyle`, `cardDepthEnabled`,
`posterCardWidthDp`...) e cujos padroes foram transcritos A MAO do perfil de
quem montou o pacote — o proprio codigo dizia "perfil do dono; fabrica:
desligado". A RPC `sync_pull_profile_settings_blob` devolve exatamente esse
objeto, e o `sync.c` so o CONTAVA. Agora ele le o `settings_json` cru e
`ajustes_aplicar_blob()` o aplica.

O mapeamento e a parte perigosa, porque errar ali e MUDO — nao da erro, nao
trava, nao aparece no log; a pessoa so acha que a TV "veio com outras opcoes".
Os literais foram todos conferidos no codigo do web:

| chave | valores do web | indice |
|---|---|---|
| booleanos | `true` / `false` | 0 / 1 (o primeiro rotulo e "Ligado") |
| `collapseSidebar` | `true` / `false` | 0 "Recolhida" / 1 "Fixa" |
| `discoverLocation` | `in_search`, `in_sidebar`, `off` | 0, 1, 2 |
| `homeImdbRatingsVisibility` | `SHOW_ALL`, `HIDE_ALL` | 0, 1 |
| `continueWatchingCardStyle` | `card`, `wide`, `poster` | 0, 1, 2 |
| `continueWatchingSortMode` | `default`, `streaming_style`, `split_upcoming` | 0, 1, 2 |
| numeros | o proprio valor | limitado pela faixa da opcao |

Duas recusas deliberadas: chave AUSENTE nao mexe na opcao, e valor de texto que
este app nao reconhece (versao nova do web) tambem nao — escolher um padrao ali
inventaria uma preferencia que a pessoa nunca marcou, e ela veria a TV mudar
sozinha.

QUANDO aplica: so na PRIMEIRA volta depois de entrar ou de trocar de perfil
(`sync_reaplicar_ajustes()`). O app nativo le os ajustes mas nao os escreve de
volta; reaplicar a cada ciclo desfaria, segundos depois, tudo que a pessoa
mudasse na propria TV.

`tests/conta_ajustes.c` alimenta um blob com todo valor no OPOSTO do padrao —
para que uma opcao nao aplicada apareca como falha em vez de coincidir — e
confere 23 itens, incluindo os dois casos de recusa. Passa nos 23.

### Verificado NA TV, com conta de verdade (03/09/2026)

`[sessao] logado como 88376366-…` — o login por QR foi COMPLETADO no aparelho,
com alguem autorizando no celular. Era o unico caminho que nunca tinha sido
exercitado. Junto dele: `[perfis] 2 perfil(is)`, `[addons] 3 vindos da conta`,
`[ajustes] blob da conta: 33 chave(s) reconhecida(s)`, `[desc] tmdb: chave da
conta`, `27 de 51 progressos casaram com o catalogo`, 60,0 fps / 0 janks. A
pasta gravavel deixou de ser sonda: `.nuvio/` dentro da pasta do app, com
`sessao.txt`, `progresso.txt` e `ajustes.txt`.

#### O blob de ajustes nao era nada do que o codigo do web sugeria

Tres descobertas em cadeia, todas so visiveis com a conta real:

1. **12232 bytes** — nao cabia no buffer de 4096. A recusa de aplicar pela
   metade estava certa; o efeito era o recurso nao funcionar. Virou alocado.
2. **Nao e um mapa plano de camelCase.** E
   `{"version":1,"features":{"layout_settings":{"hero_section_enabled":
   {"type":"boolean","value":true}, ...}}}` — chave em SNAKE_CASE, aninhada por
   feature, valor EMBRULHADO em `{type,value}`. Procurando `heroSectionEnabled`
   o app achava ZERO chaves em 12 KB de ajustes.
3. **Os enums vem em MAIUSCULA** no servidor (`IN_SEARCH`, `CARD`, `DEFAULT`)
   enquanto o JS do app web os escreve em minuscula. A comparacao passou a
   ignorar caixa: de 30 para 33 chaves reconhecidas.

LICAO: ler o codigo de quem PRODUZ o dado nao substitui olhar o dado. O teste
que existia usava o formato SUPOSTO e passava verde enquanto o app nao aplicava
nada no aparelho. Foi reescrito no formato do servidor.

#### Trakt NAO esta na conta (medido, e nao e defeito do app)

`sync_pull_provider_credentials` devolve, para esta conta, nos DOIS perfis:
`animeskip`, `debrid:premiumize`, `debrid:realdebrid`, `debrid:torbox`,
`introdb`, `mdblist`, `tmdb` — e **nenhum `trakt`**. O leitor de trakt continua
no `sync.c` porque a RPC e a mesma e a linha aparece sozinha assim que o app web
a escrever. Ate la, o vinculo Trakt deste app sai de `art/trakt.txt`, que NAO
pode ir no pacote — ou seja, **quem instalar hoje fica sem Trakt** ate logar no
app web uma vez.

O mesmo levantamento rendeu um ganho: `tmdb` e `mdblist` ESTAO na conta, e o app
lia os dois de arquivo do dono. Agora saem da conta (`desc_tmdb_definir`,
`extras_definir_chave`). Na conta testada o `mdblist` veio com `api_key` vazia,
entao o app manteve o valor local — o comportamento correto.

#### Um crash NAO explicado

Duas quedas no aparelho durante a sessao:

- **16:46, `libc index+0xe0`** — defeito MEU, entendido e corrigido: eu tinha
  acrescentado tres opcoes ao enum de ajustes sem acrescentar as tres chaves em
  `CHAVE[]`. As ultimas entradas ficaram NULL e todas as chaves depois do ponto
  de insercao DESALINHARAM. Nao aparece na hora: so quando `ajustes.txt` passa a
  existir, e ai `strcmp(NULL, ...)` derruba o app no arranque. `CHAVE[]` perdeu o
  tamanho explicito e ganhou verificacao em tempo de compilacao — esquecer uma
  chave agora e ERRO DE COMPILACAO (conferido removendo uma de proposito).
- **16:57, `libSDL2` num caminho de free (decremento de refcount, ponteiro
  0x1020)** — NAO explicado. Aconteceu na rodada em que 5 ajustes MUDARAM,
  incluindo `posterCardWidthDp` (126 -> 116), que alimenta o tamanho dos cards e
  o decode de textura; a hipotese e que aplicar dimensao com a home ja
  desenhando deixa o cache de textura inconsistente. NAO REPRODUZIDO: no Mac,
  com a mesma sessao e os mesmos 5 ajustes mudando, o app segue de pe; e a
  rodada seguinte na TV (0 ajustes mudando) tambem. Fica registrado como aberto,
  com o relatorio em `/var/log/reports/librdx/`.

#### Armadilhas de operacao que custaram tempo

- `luna://com.webos.applicationManager/launch` **nao reinicia** app ja rodando:
  so traz para frente. Dois deploys foram lidos no log de um processo antigo, e
  quase concluiram que a correcao nao funcionava. Matar o PID antes.
- O build ARM do `arm.sh` **nao recebia os `-D`** (a configuracao so tinha sido
  posta no `mac.sh`). O binario saia sem servidor, compilava, instalava, abria —
  e so a tela dizia "pacote montado sem servidor". Agora as variaveis entram por
  `docker run --env-file` e o script ABORTA se `api.nuvio.tv` nao estiver no
  binario.
- O log do app **perde as ultimas linhas num crash**. Os arquivos em `.nuvio/`
  sao a evidencia confiavel, nao o log.

### O que continua faltando

1. **Sync periodico** (5 min, nunca com o player aberto) foi implementado mas
   nao observado disparar na TV.
2. ~~Sync so rodava em tres momentos~~ (arranque, pos-login, troca de perfil).
   Sem repeticao periodica, parar no celular nao aparece na TV sem reabrir.
4. **Superficies sem push** (biblioteca, vistos, colecoes, catalogos da home):
   deliberado enquanto nao houver tela que as edite — push sem tela mandaria
   lista vazia e apagaria o dado nos outros aparelhos.
5. **O pacote tem 172 MB** de arte pre-assada. Nao e segredo, mas e peso.

---

## Parte 5 — O que foi verificado, e como

O metodo importa tanto quanto o resultado: quase tudo aqui falha em SILENCIO.

| O que | Como foi verificado | Resultado |
|---|---|---|
| Sessao anonima | chamada real a `api.nuvio.tv` | HTTP 200 |
| Pedido do codigo | idem | `code` + `web_url` + `poll_interval_seconds` |
| Poll | idem | `status: "pending"`; com nonce errado, 400 |
| QR | decodificado por leitor de verdade (OpenCV), **a partir da captura de tela do app rodando** | leu a URL certa |
| Escritor de JSON | teste com aninhamento, escape e `null` | saida exata |
| Renovacao do token | token de acesso trocado por lixo, refresh mantido | 401 -> renovou -> 200, token novo gravado |
| Renovacao impossivel | os dois tokens invalidos | saiu da conta e APAGOU o arquivo |
| Ciclo de sync | sessao anonima gravada como sessao de usuario | perfis, addons, credenciais, progresso e as RPC so-leitura, todos exercitados |
| Addons da conta | app rodando, com log | `[addons] 2 vindos da conta` |
| Ajustes > Conta | captura de tela do app | perfil, resumo do sync e "Sair da conta" |

NAO verificado, e nao da para verificar sozinho: a **troca final do codigo pelo
token**, que exige uma pessoa autorizando no celular. Todo o resto do caminho
depois dela ja foi exercitado com uma sessao real.
