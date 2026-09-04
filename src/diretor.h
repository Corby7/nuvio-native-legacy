// Ficha curta de um diretor para detalhe e filmografia: foto, bio, nascimento e
// os titulos por que e conhecido. Tudo do TMDB, a partir do NOME (a colecao so
// tem o nome). Nao bloqueia: pedir e perguntar, quadro a quadro. A ficha so e
// publicada depois de confirmar o nome e a identidade de diretor no TMDB.
#ifndef NV_DIRETOR_H
#define NV_DIRETOR_H
void diretor_pedir(const char *nome);        // cacheia; falhas retomam com backoff
int  diretor_pronto(const char *nome);       // 1 quando a ficha chegou
const char *diretor_foto(const char *nome);  // URL original, ou ""
const char *diretor_hero(const char *nome);  // backdrop original de uma obra conhecida
const char *diretor_bio(const char *nome);
const char *diretor_meta(const char *nome);  // "Diretor · Nascido em ... · Lugar"
const char *diretor_conhecido(const char *nome); // "A · B · C"
#endif
