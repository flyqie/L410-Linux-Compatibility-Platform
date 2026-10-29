/* Linux semantic provider for the original AnCo resource-manager interface. */
#ifndef ANCO_RESOURCE_AGENT_H
#define ANCO_RESOURCE_AGENT_H

#include <gbinder.h>

typedef struct anco_resource_agent AncoResourceAgent;

/*
 * Publishes the original guest ResourceManager endpoint (SA 66894).  The
 * caller owns one directory containing <bundle>.png files derived from the
 * corresponding freedesktop .desktop entries.
 */
AncoResourceAgent *anco_resource_agent_new(GBinderIpc *ipc,
                                           const char *icon_directory);
int anco_resource_agent_register(AncoResourceAgent *agent);
int anco_resource_agent_free(AncoResourceAgent *agent);

#endif
