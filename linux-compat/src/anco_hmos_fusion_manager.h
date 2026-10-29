#ifndef ANCO_HMOS_FUSION_MANAGER_H
#define ANCO_HMOS_FUSION_MANAGER_H

#include <gbinder.h>

#define ANCO_HMOS_FUSION_MANAGER_IFACE "OHOS.HmosFusion.IHmosFusionManager"
#define ANCO_HMOS_FUSION_ROIDT_KEEPALIVE_TRANSACTION 58

typedef enum anco_roidt_env_state {
    ANCO_ROIDT_ENV_START = 0,
    ANCO_ROIDT_ENV_END = 1
} AncoRoidtEnvState;

typedef struct anco_hmos_fusion_result {
    int local_error;
    int transport;
    gboolean decoded;
    gint32 service_result;
} AncoHmosFusionResult;

/* Exact original HmosFusionProxy::RoidtKeepAlive wire contract. */
gboolean anco_hmos_fusion_roidt_keepalive(GBinderRemoteObject *manager,
    AncoRoidtEnvState state, AncoHmosFusionResult *out);

#endif
