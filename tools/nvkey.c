// Injects keys into the TV remote's input device.
//
// It exists so you can navigate ANY app — including Apple's, which is the
// reference — and capture the same screen in both. Without it, comparing our app
// against the original depended on somebody holding the remote.
//
// Usage: nvkey /dev/input/event1 up down left right ok back ...
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int code(const char *name) {
  if (!strcmp(name, "up"))    return KEY_UP;
  if (!strcmp(name, "down"))  return KEY_DOWN;
  if (!strcmp(name, "left"))  return KEY_LEFT;
  if (!strcmp(name, "right")) return KEY_RIGHT;
  if (!strcmp(name, "ok"))    return KEY_ENTER;
  if (!strcmp(name, "back"))  return KEY_BACK;
  if (!strcmp(name, "home"))  return KEY_HOMEPAGE;
  // accepts a raw numeric code: the LG remote's Back is not the kernel's
  // KEY_BACK, and finding out which it is means trying
  if (name[0] >= '0' && name[0] <= '9') return atoi(name);
  return -1;
}

static void sends(int fd, int kind, int cod, int value) {
  struct input_event e;
  memset(&e, 0, sizeof e);
  e.type = kind; e.code = cod; e.value = value;
  if (write(fd, &e, sizeof e) != (ssize_t)sizeof e) perror("write");
}

int main(int argc, char **argv) {
  if (argc < 3) { printf("usage: nvkey <device> <key>...\n"); return 1; }
  int fd = open(argv[1], O_WRONLY);
  if (fd < 0) { perror("open"); return 1; }
  for (int i = 2; i < argc; i++) {
    int c = code(argv[i]);
    if (c < 0) { printf("unknown key: %s\n", argv[i]); continue; }
    sends(fd, EV_KEY, c, 1); sends(fd, EV_SYN, SYN_REPORT, 0);
    usleep(60000);
    sends(fd, EV_KEY, c, 0); sends(fd, EV_SYN, SYN_REPORT, 0);
    // a pause between keys: TV navigation is animated, and without waiting the
    // application swallows the following keys
    usleep(420000);
  }
  close(fd);
  return 0;
}
