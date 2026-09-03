# Verificação do player nativo

`bash tests/player.sh` verifica extração e ordenação de fontes, ausência de
fontes fictícias, URLs longas, seleção de episódios e persistência de T/E.
Também verifica a navegação sem botões de salto, filmes sem subtítulo de
episódio, navegação/cancelamento do menu e descarte de nome de episódio antigo.
`bash tests/player.sh --visual` renderiza seis estados SDL/OpenGL e salva
capturas em `/tmp/nuvio-player-*.bmp`. As capturas usam dados de teste, não
comprovam reprodução de vídeo nem disponibilidade de uma faixa.

`bash tests/player.sh --live` consulta os addons configurados para Silo T2E4 e
o progresso do Trakt. Confere o alvo do botão Reproduzir com `next_episode`.
Não reproduz nem envia marcações de histórico. Não imprime URLs assinadas.

Parser isolado com sanitizers, sem inicializar SDL:

```sh
cc -fsanitize=address,undefined -g src/stream_parse.c src/js.c \
  tests/stream_parser.c -Isrc -I/opt/homebrew/include -o /tmp/nuvio-parser-tests
/tmp/nuvio-parser-tests
```

Em 02/09/2026: 36 fontes diretas, sete anunciadas como DV, uma MP4/DV;
Trakt e botão Reproduzir concordaram em T2E4. Builds Mac e ARM compilaram.
ASan/UBSan passaram no parser isolado. O executável SDL completo com ASan
abortou no inicializador da biblioteca SDL do macOS 27 beta antes de `main`;
não conta como teste de memória do aplicativo completo.

Limites: verificar DV real na LG continua necessário. O Mac não possui o
pipeline webOS. A folha de legendas mantém as opções efetivamente suportadas
no backend, ainda sem a coluna de idiomas e todos os controles do app web.

Reproduzir usa seleção explícita, retomada ainda não concluída e então o
próximo episódio informado pelo Trakt. Progresso local guarda T/E em colunas
opcionais compatíveis com arquivos antigos. Conclusões acima de 90% usam
scrobble/stop; abaixo disso usam pause. Nenhum teste envia essas marcações.

## Limite de tamanho das legendas na LG C9

Inspeção somente leitura do firmware em 02/09/2026: os cinco índices de
`setSubtitleFontSize` mapeiam para 36, 46, 50, 56 e 70 no renderizador.
`setSubtitleCharacterFontSize` (`very_small` a `very_large`) usa os mesmos
valores: não acrescenta tamanhos menores. Resposta de sucesso da API não
comprova alteração visual. Não enviar índices negativos ou fora de 0–4.
As duas opções solicitadas abaixo do mínimo exigem outro caminho de
renderização de legendas; continuam pendentes, sem opções fictícias no menu.

O firmware também expõe `setSubtitleCharacterOpacity`; a folha oferece agora
100%, 75%, 50% e 25%, persistidos por aparelho. A opacidade do fundo ganhou os
passos intermediários de 25% e 75%, além de uma ação para restaurar o
estilo padrão. Fonte/família não foi exposta porque o binário só revela o nome
do campo (`charFont`), não o vocabulário aceito; janela e presentation mode não
foram apresentados como estilo porque não acrescentam controle visual
comprovado sobre a legenda de vídeo usada pelo app.

O nome do episódio em Continue Assistindo vem do Trakt ou da lista de
episódios já disponível; nenhuma consulta é feita durante o desenho do card.
O novo campo muda `sizeof(CatItem)` e invalida o cache binário antigo pelo
controle de tamanho existente; a primeira abertura repopula esse cache.
