/*
 * Process-local AnCo broker activation.
 *
 * The stock broker waits for the global fusion parameter before invoking its
 * exported OnFusionStateOn transition.  Our runtime must not enable that
 * global parameter because it would start the built-in container stack.  This
 * preload wrapper lets the unmodified stock broker finish Init(), then invokes
 * the same exported transition only inside the project-owned broker process.
 */
#include <dlfcn.h>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace OHOS::ANCO {

class BrokenMain {
public:
    void Init(const std::string &config, int systemAbilityId);
    void Init(const std::string &config);
    void OnFusionStateOff();
};

using InitWithSa = void (*)(BrokenMain *, const std::string &, int);
using InitWithoutSa = void (*)(BrokenMain *, const std::string &);
using FusionOn = void (*)(BrokenMain *);
using FusionOff = void (*)(BrokenMain *);
using LoadAllBrokers = void (*)(BrokenMain *, const std::string &, int);

static bool KeepProjectBrokerActive()
{
    const char *value = std::getenv("L410_PROJECT_BROKER_KEEP_ACTIVE");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

static void ActivateForThisProcess(BrokenMain *self, const std::string &config, int systemAbilityId)
{
    if (KeepProjectBrokerActive()) {
        auto loadAll = reinterpret_cast<LoadAllBrokers>(dlsym(
            RTLD_NEXT,
            "_ZN4OHOS4ANCO10BrokenMain14LoadAllBrokersERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEi"));
        if (loadAll == nullptr) {
            static constexpr char missing[] = "l410-broker-trigger: LoadAllBrokers unavailable\n";
            write(STDERR_FILENO, missing, sizeof(missing) - 1);
            return;
        }
        loadAll(self, config, systemAbilityId);
        static constexpr char loaded[] = "l410-broker-trigger: stock LoadAllBrokers called\n";
        write(STDERR_FILENO, loaded, sizeof(loaded) - 1);
        return;
    }
    auto fusionOn = reinterpret_cast<FusionOn>(dlsym(
        RTLD_NEXT, "_ZN4OHOS4ANCO10BrokenMain15OnFusionStateOnEv"));
    if (fusionOn == nullptr) {
        static constexpr char message[] = "l410-broker-trigger: OnFusionStateOn unavailable\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        return;
    }
    fusionOn(self);
    static constexpr char message[] = "l410-broker-trigger: project broker fusion-on requested\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
}

void BrokenMain::OnFusionStateOff()
{
    if (KeepProjectBrokerActive()) {
        static constexpr char message[] = "l410-broker-trigger: project broker ignores global fusion off\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        return;
    }
    auto original = reinterpret_cast<FusionOff>(dlsym(
        RTLD_NEXT, "_ZN4OHOS4ANCO10BrokenMain16OnFusionStateOffEv"));
    if (original != nullptr) {
        original(this);
    }
}

void BrokenMain::Init(const std::string &config, int systemAbilityId)
{
    auto original = reinterpret_cast<InitWithSa>(dlsym(
        RTLD_NEXT,
        "_ZN4OHOS4ANCO10BrokenMain4InitERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEi"));
    if (original == nullptr) {
        return;
    }
    original(this, config, systemAbilityId);
    ActivateForThisProcess(this, config, systemAbilityId);
}

void BrokenMain::Init(const std::string &config)
{
    auto original = reinterpret_cast<InitWithoutSa>(dlsym(
        RTLD_NEXT,
        "_ZN4OHOS4ANCO10BrokenMain4InitERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"));
    if (original == nullptr) {
        return;
    }
    original(this, config);
    ActivateForThisProcess(this, config, -1);
}

} // namespace OHOS::ANCO
