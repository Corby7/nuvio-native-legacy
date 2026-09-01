addons.txt contem as URLs dos addons do dono, e ELAS EMBUTEM CHAVES DE API
no proprio caminho (AIOStreams, Debridio, Xperience). Tratar como segredo:
nao versionar, nao publicar, nao colar em transcrito.

Foi extraido do localStorage do app web na TV:
  /var/lib/wam/Default/Local Storage/file_space.nuvio.webos_0.localstorage
  (SQLite; chaves installedAddonUrls / DisplayNames / EnabledStates,
   com o formato {"profiles":{"1":[...]}})
Separador TAB de proposito: nome de addon contem "|" ("AIOStreams | ElfHosted").
