#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include "nocopyable.h"
#include "refbase.h"
namespace OHOS { class IRemoteObject; }
namespace OHOS::AppExecFwk { class EventHandler; }
namespace OHOS::LowpowerManager {
class SubscriberImpl;
class AncoStatusSubscriber {
public:
 AncoStatusSubscriber();
 virtual ~AncoStatusSubscriber();
 virtual void OnAncoStatusChanged(int);
 virtual bool HasAncoCommunications();
 virtual void OnConnected();
 virtual void OnDisconnected();
 virtual void OnRemoteDied(const wptr<IRemoteObject>&);
private:
 sptr<SubscriberImpl> impl_;
};
}
namespace OHOS::ANCO {
class LowpowerListener; class FreezeControl;
struct LayoutCheck;
class FreezeManager : public LowpowerManager::AncoStatusSubscriber, public NoCopyable {
public:
 FreezeManager();
 ~FreezeManager() override;
 void OnAncoStatusChanged(int) override;
 bool HasAncoCommunications() override;
 void Wait();
 void SetMainHandler(std::shared_ptr<AppExecFwk::EventHandler>);
private:
 friend struct LayoutCheck;
 // ABI binding experiment: descriptive member names, not recovered source names.
 std::string fsm_state_, quick_ability_;
 int32_t status_;
 sptr<LowpowerListener> listener_;
 std::mutex restore_mutex_;
 std::vector<sptr<FreezeControl>> restore_;
 std::shared_ptr<AppExecFwk::EventHandler> handler_;
 std::shared_mutex state_mutex_;
 bool stopping_;
 std::mutex stopping_mutex_;
};
struct LayoutCheck {
 static_assert(__builtin_offsetof(FreezeManager,fsm_state_)==0x18);
 static_assert(__builtin_offsetof(FreezeManager,quick_ability_)==0x30);
 static_assert(__builtin_offsetof(FreezeManager,status_)==0x48);
 static_assert(__builtin_offsetof(FreezeManager,listener_)==0x50);
 static_assert(__builtin_offsetof(FreezeManager,restore_mutex_)==0x58);
 static_assert(__builtin_offsetof(FreezeManager,restore_)==0x80);
 static_assert(__builtin_offsetof(FreezeManager,handler_)==0x98);
 static_assert(__builtin_offsetof(FreezeManager,state_mutex_)==0xa8);
 static_assert(__builtin_offsetof(FreezeManager,stopping_)==0x138);
 static_assert(__builtin_offsetof(FreezeManager,stopping_mutex_)==0x140);
};
static_assert(sizeof(FreezeManager)==0x168);
static_assert(alignof(FreezeManager)==8);
static_assert(sizeof(LowpowerManager::AncoStatusSubscriber)==16);
static_assert(sizeof(std::shared_mutex)==0x90);
}

// Use original header without changing its GetInstance implementation.
#pragma GCC visibility push(default)
#include "singleton.h"
#pragma GCC visibility pop
// Explicit-instantiation declarations suppress local storage/guard definitions.
extern template std::shared_ptr<OHOS::ANCO::FreezeManager>
 OHOS::DelayedSingleton<OHOS::ANCO::FreezeManager>::instance_;
extern template std::mutex
 OHOS::DelayedSingleton<OHOS::ANCO::FreezeManager>::mutex_;

#include "event_handler.h"
using OHOS::AppExecFwk::EventRunner;
using OHOS::AppExecFwk::EventHandler;
using OHOS::AppExecFwk::Mode;
static_assert(sizeof(EventHandler) == 0xb0);
static_assert(alignof(EventHandler) == 16);
static_assert(static_cast<unsigned>(Mode::DEFAULT) == 0);

#include "anco_native_broken_main.hpp"
#include <unistd.h>
namespace OHOS::ANCO {
// Local semantic binding only. No original method implementation is replaced.
// Compile-only until Linux stop -> original retirement is fully connected.
struct LinuxStartBinding {
 // Local result reports submissions only, never completed shutdown.
 enum class StopResult { KeepAlive, NoHandler, Submitted, SubmissionFailed };
 static StopResult Retire(BrokenMain& owner) {
  std::lock_guard<std::mutex> lock(owner.mutex_);
  owner.loader_.UnloadAll();
  if (owner.service_id_ >= 1) {
   owner.RemoveSystemAbility(owner.service_id_);
   owner.broker_ = nullptr;
  }
  DelayedSingleton<FreezeManager>::GetInstance()->Wait();
  if (owner.loader_.IsKeepAlive()) return StopResult::KeepAlive;
  if (!owner.handler_) return StopResult::NoHandler;
  static_assert(static_cast<unsigned>(EventHandler::Priority::LOW) == 3);
  // Original callback ownership: owner lives until Run/_exit; runner retained.
  // Caller must serialize actual lifecycle and must not run this from a signal.
  const bool remove = owner.handler_->PostTask(
   std::bind(&BrokenMain::RemoveParamsListener, &owner), std::string(),
   0, EventHandler::Priority::LOW);
  const bool stop = owner.handler_->PostTask(
   std::bind(&EventRunner::Stop, owner.runner_), std::string(),
   0, EventHandler::Priority::LOW);
  return remove && stop ? StopResult::Submitted : StopResult::SubmissionFailed;
 }
 static void Prepare(BrokenMain& owner, const std::string& config, int id) {
  owner.config_ = config;
  owner.service_id_ = id;
  owner.runner_ = EventRunner::Create(false, Mode::DEFAULT);
  owner.handler_ = std::make_shared<EventHandler>(owner.runner_);
  auto freeze = DelayedSingleton<FreezeManager>::GetInstance();
  freeze->SetMainHandler(owner.handler_);
 }
};
}

#include <pthread.h>
#include <signal.h>
#include <errno.h>
namespace {
struct StopSignalContext {
 OHOS::ANCO::BrokenMain* owner;
 int fd;
};
void* stop_lifecycle_thread(void* opaque) {
 auto& context = *static_cast<StopSignalContext*>(opaque);
 char byte;
 for (;;) {
  ssize_t count = read(context.fd, &byte, 1);
  if (count > 0) continue;
  if (count < 0 && errno == EINTR) continue;
  if (count < 0) {
   static const char message[] = "linux lifecycle: pipe read failed; no unload submitted\n";
   (void)write(STDERR_FILENO, message, sizeof(message)-1);
   return nullptr;
  }
  break;
 }
 close(context.fd);
 const auto result = OHOS::ANCO::LinuxStartBinding::Retire(*context.owner);
 // Never report completed shutdown from submission success. Main Run/_exit
 // and parent waitpid remain the actual termination boundary.
 if (result == OHOS::ANCO::LinuxStartBinding::StopResult::KeepAlive) {
  static const char message[] = "linux lifecycle: original keep-alive retained\n";
  (void)write(STDERR_FILENO, message, sizeof(message)-1);
 } else if (result != OHOS::ANCO::LinuxStartBinding::StopResult::Submitted) {
  static const char message[] = "linux lifecycle: original stop submission failed\n";
  (void)write(STDERR_FILENO, message, sizeof(message)-1);
 }
 // Exactly one retirement per owner. No retry-unload on repeated signals.
 return nullptr;
}
}
[[noreturn]] void inspect_original_owner_start(const std::string& config, int id, int lifecycle_fd) {
 sigset_t signals;
 if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGTERM) != 0 ||
     sigaddset(&signals, SIGINT) != 0 ||
     pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) _exit(70);
 // Dedicated process entry only. Supervisor must also block these signals
 // before exec so any original DSO initializer threads inherit the mask.
 OHOS::ANCO::BrokenMain owner{};
 OHOS::ANCO::LinuxStartBinding::Prepare(owner, config, id);
 owner.LoadAllBrokers(config, id);
 StopSignalContext context{&owner, lifecycle_fd};
 pthread_t worker;
 if (pthread_create(&worker, nullptr, stop_lifecycle_thread, &context) != 0) {
  static const char message[] = "linux lifecycle: control thread creation failed\n";
  (void)write(STDERR_FILENO, message, sizeof(message)-1);
  // Startup failure, NOT a graceful-unload success. Avoid active destructors.
  _exit(71);
 }
 owner.Run();
 // Worker/context and original objects live until whole-process exit.
 _exit(0);
}

#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
int main(int argc, char** argv) {
 if (argc < 2 || argc > 3 || argv[1][0] != '/') return 64;
 int service_id = -1; // original single-argument Init default
 if (argc == 3) {
  char* end = nullptr; errno = 0;
  long parsed = strtol(argv[2], &end, 10);
  if (errno || end == argv[2] || *end || parsed < -1 || parsed == 0 || parsed > INT_MAX) return 64;
  service_id = static_cast<int>(parsed);
 }
 const char* value = getenv("L410_LIFECYCLE_FD");
 if (!value || !*value) return 65;
 char* tail = nullptr; errno = 0;
 long fd = strtol(value, &tail, 10);
 if (errno || *tail || fd < 3 || fd > INT_MAX) return 65;
 struct stat st{}; int flags = fcntl((int)fd, F_GETFL);
 if (fstat((int)fd, &st) || !S_ISFIFO(st.st_mode) || flags < 0 ||
     (flags & O_ACCMODE) != O_RDONLY || (flags & O_NONBLOCK)) return 65;
 if (fcntl((int)fd, F_SETFD, FD_CLOEXEC)) return 65;
 inspect_original_owner_start(std::string(argv[1]), service_id, (int)fd);
}
