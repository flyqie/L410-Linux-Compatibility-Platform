/* Original AnCo package metadata registration transport, Linux guest side.
 * This is NOT APK installation or a new host interface. Caller supplies the
 * original registration JSON generated from its authoritative app catalog.
 */
#ifndef ANCO_BMS_REGISTRATION_H
#define ANCO_BMS_REGISTRATION_H

#include <gbinder.h>

#define ANCO_BMS_SA 66816
#define ANCO_BMS_IFACE "OHOS.BundleManager.BmsBrokerService"
#define ANCO_BMS_INSTALL_TRANSACTION 1
#define ANCO_BMS_UNINSTALL_TRANSACTION 2
#define ANCO_BMS_UNINSTALL_WITH_USER_TRANSACTION 25
#define ANCO_BMS_UPDATE_STATUS_TRANSACTION 27
#define ANCO_BMS_GET_BUNDLE_TRANSACTION 10004
#define ANCO_BMS_MAX_BYTES ((gsize)104857600)

typedef struct {
    int local_error;       /* negative errno, before/during local setup */
    int transport;         /* exact Binder transport status */
    gboolean decoded;
    gint32 service_result; /* exact original install/uninstall result */
} AncoBmsRegistrationResult;

/* Private anonymous Linux copy equivalent to the guest MemoryFile byte payload.
 * No filesystem path, file hash, UID change or host policy modification.
 * Caller owns the returned FD. No JSON/schema acceptance is claimed here.
 */
int anco_bms_payload_fd(const void *utf8, gsize byte_length, int *fd_out);

/* Appends ONLY original FD object then int32 length to an existing request.
 * A successful request owns its duplicate; the caller retains the primary FD.
 * Duplication failure returns negative errno without appending an object,
 * length or offset. Never sends/owns the caller FD as a fallback.
 */
int anco_bms_install_append(GBinderLocalRequest *request, int fd,
                            gsize byte_length);

/* Uses the borrowed original BMS remote endpoint. Returns TRUE only when
 * transport succeeds, the exact original response decodes, and service=0.
 * Caller must preserve the original UID1000/type1 admission, not impersonate
 * another UID or change host permission checks. No service is auto-started.
 */
gboolean anco_bms_register(GBinderRemoteObject *bms, const void *utf8,
                           gsize byte_length, AncoBmsRegistrationResult *out);

/* Original GetBundleInfo transport. Returns an owned Binder reply on
 * successful transport and a readable service result (including nonzero errors).
 * decoded covers ONLY that result, not the nested BundleInfo parcel. The caller
 * must parse the original record before admitting it to the Linux catalog.
 * The reply retains any raw-data FDs until the caller unrefs it. host_user is
 * a host account ID, not the guest user. No registration or writes.
 */
GBinderRemoteReply *anco_bms_query_bundle(GBinderRemoteObject *bms, const char *bundle,
    gint32 flags, gint32 host_user, AncoBmsRegistrationResult *out);

/* Original ordinary unregister, never overrides the removable check. Caller
 * supplies ONLY a bundle it owns in the accepted app catalog. with_user is the
 * original guest privacy-mode branch, not a guessed host user ID; user_id is
 * the guest user and original BMS performs its existing mapping. This removes
 * host registration only, not Linux packages/data or the running process.
 */
int anco_bms_uninstall_append(GBinderLocalRequest *request, const char *bundle,
                              gboolean with_user, gint32 user_id);
gboolean anco_bms_unregister(GBinderRemoteObject *bms, const char *owned_bundle,
                             gboolean with_user, gint32 guest_user_id,
                             AncoBmsRegistrationResult *out);

/* Original IAncoPackageManagerProxy::sendUpdateStatusFromAnco transport.
 * The guest package name, status and guest user are forwarded in the exact
 * original order; BMS performs its own host-user mapping and publishes the
 * existing UPDATING_ANCO_BUNDLE_STATUS event to SceneBoard. */
int anco_bms_update_status_append(GBinderLocalRequest *request,
                                  const char *bundle, gint32 status,
                                  gint32 guest_user_id);
gboolean anco_bms_send_update_status(GBinderRemoteObject *bms,
                                     const char *bundle, gint32 status,
                                     gint32 guest_user_id,
                                     AncoBmsRegistrationResult *out);

#endif
