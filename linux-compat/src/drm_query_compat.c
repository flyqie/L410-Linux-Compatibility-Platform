#define _GNU_SOURCE
#include <string.h>
int drmSetInterfaceVersion(int fd,void *version){(void)fd;(void)version;return 0;}
char *drmGetBusid(int fd){(void)fd;return strdup("");}
