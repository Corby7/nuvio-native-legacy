// Ficha curta de um diretor para o hero da fileira "Directors": foto, bio,
// nascimento e os titulos por que e conhecido. Tudo do TMDB, a partir do NOME
// (a colecao so tem o nome). Nao bloqueia: pedir e perguntar, quadro a quadro.
#ifndef NV_DIRETOR_H
#define NV_DIRETOR_H
void diretor_pedir(const char *nome);        // ignora se ja tem ou em voo
int  diretor_pronto(const char *nome);       // 1 quando a ficha chegou
const char *diretor_foto(const char *nome);  // URL w500, ou ""
const char *diretor_bio(const char *nome);
const char *diretor_meta(const char *nome);  // "Diretor · Nascido em ... · Lugar"
const char *diretor_conhecido(const char *nome); // "A · B · C"
#endif
