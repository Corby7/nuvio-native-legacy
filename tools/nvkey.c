// Injeta teclas no device do controle remoto da TV.
//
// Existe para poder navegar em QUALQUER app — inclusive o da Apple, que e a
// referencia — e capturar a mesma tela nos dois. Sem isto, comparar o nosso app
// com o original dependia de alguem com o controle na mao.
//
// Uso: nvkey /dev/input/event1 up down left right ok back ...
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int codigo(const char *nome) {
  if (!strcmp(nome, "up"))    return KEY_UP;
  if (!strcmp(nome, "down"))  return KEY_DOWN;
  if (!strcmp(nome, "left"))  return KEY_LEFT;
  if (!strcmp(nome, "right")) return KEY_RIGHT;
  if (!strcmp(nome, "ok"))    return KEY_ENTER;
  if (!strcmp(nome, "back"))  return KEY_BACK;
  if (!strcmp(nome, "home"))  return KEY_HOMEPAGE;
  // aceita codigo numerico cru: o Back do controle da LG nao e o KEY_BACK do
  // kernel, e descobrir qual e exige tentar
  if (nome[0] >= '0' && nome[0] <= '9') return atoi(nome);
  return -1;
}

static void manda(int fd, int tipo, int cod, int valor) {
  struct input_event e;
  memset(&e, 0, sizeof e);
  e.type = tipo; e.code = cod; e.value = valor;
  if (write(fd, &e, sizeof e) != (ssize_t)sizeof e) perror("write");
}

int main(int argc, char **argv) {
  if (argc < 3) { printf("uso: nvkey <device> <tecla>...\n"); return 1; }
  int fd = open(argv[1], O_WRONLY);
  if (fd < 0) { perror("open"); return 1; }
  for (int i = 2; i < argc; i++) {
    int c = codigo(argv[i]);
    if (c < 0) { printf("tecla desconhecida: %s\n", argv[i]); continue; }
    manda(fd, EV_KEY, c, 1); manda(fd, EV_SYN, SYN_REPORT, 0);
    usleep(60000);
    manda(fd, EV_KEY, c, 0); manda(fd, EV_SYN, SYN_REPORT, 0);
    // pausa entre teclas: navegacao de TV tem animacao, e sem esperar o
    // aplicativo engole as teclas seguintes
    usleep(420000);
  }
  close(fd);
  return 0;
}
