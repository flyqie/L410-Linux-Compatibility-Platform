#ifndef ANCO_LINUX_SESSION_BUS_H
#define ANCO_LINUX_SESSION_BUS_H
#include <glib.h>
/* Linux-private standard session dependency; no native host interface. */
int anco_linux_session_bus_start(const char *directory, const char *tray_directory,
                                 GSourceFunc lost, gpointer data);
void anco_linux_session_bus_stop(void);
#endif
