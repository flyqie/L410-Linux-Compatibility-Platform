#ifndef ANCO_NATIVE_BROKEN_MAIN_HPP
#define ANCO_NATIVE_BROKEN_MAIN_HPP
#include "anco_native_library_loader.hpp"
#include "iremote_object.h"
#include <atomic>
#include <mutex>

namespace OHOS::AppExecFwk { class EventRunner; class EventHandler; }
namespace OHOS::ANCO {
struct BrokenMainLayoutCheck;
struct LinuxStartBinding;
// Recovered native owner declaration; ARM64 OHOS libc++ only (loader guard).
// Evidence: original-owner-fields-20261004-r1. Member names are descriptive;
// their types, order and construction are checked against the original ELF.
// This is NOT the Linux common runtime or permission to activate modules.
class BrokenMain {
public:
    // Value-initialize the complete owner, as the original main does.
    // Real LibraryLoader C1 and actual C++ member constructors are used.
    BrokenMain() = default;
    ~BrokenMain() = default;
    BrokenMain(const BrokenMain&) = delete;
    BrokenMain& operator=(const BrokenMain&) = delete;
    BrokenMain(BrokenMain&&) = delete;
    BrokenMain& operator=(BrokenMain&&) = delete;

    // Supplied ONLY by the original libservicebroker_fwk.z.so.
    // Init creates the original runner, handler and FreezeManager linkage.
    // It may already load modules OR register original parameter listeners.
    // No extra unconditional module load, fake On/Off or watcher suppression.
    void Init(const std::string& config);
    void Init(const std::string& config, int system_ability_id);
    void Run();
    void RemoveSystemAbility(int system_ability_id);
    void RemoveParamsListener();
    void LoadAllBrokers(const std::string& config, int system_ability_id);

private:
    friend struct BrokenMainLayoutCheck;
    friend struct LinuxStartBinding;
struct LinuxStartBinding;
    LibraryLoader loader_;
    sptr<IRemoteObject> broker_;
    std::string config_;
    int service_id_ = 0;
    std::atomic<bool> active_{false};
    std::atomic<bool> initial_callback_{false};
    std::mutex mutex_;
    std::shared_ptr<AppExecFwk::EventRunner> runner_;
    std::shared_ptr<AppExecFwk::EventHandler> handler_;
};
static_assert(sizeof(BrokenMain) == 0xb0);
static_assert(alignof(BrokenMain) == 8);
// Original main retains this owner through Run and ends with _exit, without
// active-member destruction. Preserve that lifetime in any eventual caller.
// A destructor or cleared listener flag is NOT a callback-retirement barrier.
// Complete activation/retirement and host dependencies must be resolved before
// execution. Current evidence establishes declaration/compile binding only.
} // namespace OHOS::ANCO
#endif
