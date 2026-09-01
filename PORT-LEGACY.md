# Nuvio 1.0.1 — port nativo legacy

Este build é separado do protótipo `nuvio-native` (layout Apple TV). A fonte
de verdade é o app `NuvioWeb-0.3.38-beta` na branch `legacy-tv`, tag
`v1.0.1+legacy.1`.

## Contrato visual

- rail lateral fixa de 144px, expandida apenas ao abrir o menu;
- hero moderno no topo, mídia à direita e texto à esquerda;
- viewport de fileiras independente, com `Continue Assistindo` em 419×236 e
  cards retrato de 212×318, gap de 24px;
- foco por borda/escala e navegação espacial por D-pad;
- busca, biblioteca e ajustes respeitam a mesma área útil depois da rail.

O código nativo mantém o cache assíncrono de imagens, cliente de addons/Trakt,
pipeline de vídeo e telemetria de frame do protótipo. Nenhum token visual ou
comportamento do layout Apple TV é usado como requisito deste build.

## Identidade e build

Durante a validação o pacote usa o id `space.nuvio.native.legacy`, para poder
ser instalado ao lado do protótipo sem sobrescrevê-lo. A versão exibida é
`1.0.1`; o id pode voltar a `space.nuvio.native` somente quando este build
substituir oficialmente o protótipo.

```bash
bash tools/mac.sh
```

O script compila todos os módulos SDL2/GLES2 para validação no Mac. O arquivo
`deploy/app/nuvio-proto` que veio da cópia é apenas um artefato de referência e
não deve ser distribuído: o pacote webOS precisa substituir esse executável
por um binário ARM compilado a partir deste diretório, mantendo o manifesto
`space.nuvio.native.legacy`.
