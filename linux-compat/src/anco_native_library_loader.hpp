#ifndef ANCO_NATIVE_LIBRARY_LOADER_HPP
#define ANCO_NATIVE_LIBRARY_LOADER_HPP

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#if !defined(__aarch64__) || !defined(_LIBCPP_VERSION)
#error "This declaration requires the ARM64 HarmonyOS libc++ ABI"
#endif

// Recovered native declaration of the original native class.
// Definitions are supplied ONLY by libservicebroker_fwk.z.so. No local
// constructor, destructor, loader implementation or raw object storage.
// Evidence: original-loader-owner-20261004-r1 and
// original-loader-declaration-20261004-r1 under analysis_sp68_sceneboard_lifecycle.
namespace OHOS::ANCO {
class LibInfo;
struct BrokerInfo;
class LibraryLoader {
public:
    LibraryLoader();
    virtual ~LibraryLoader();

    // Calls the original configuration parser and module loader. These void
    // calls do not acknowledge SA publication, peer pairing or GUI readiness.
    // Invoke under the complete native lifecycle owner, not as a retry loop.
    void LoadAll(const std::string& config_path);
    void UnloadAll();
    bool IsKeepAlive();

    // Local callers must not copy ownership of the original module registry.
    LibraryLoader(const LibraryLoader&) = delete;
    LibraryLoader& operator=(const LibraryLoader&) = delete;
    LibraryLoader(LibraryLoader&&) = delete;
    LibraryLoader& operator=(LibraryLoader&&) = delete;

private:
    // Original member names unavailable; types/order come from original
    // constructor, destruction and named vector helper instantiations.
    std::vector<std::shared_ptr<LibInfo>> modules_;
    std::vector<BrokerInfo> definitions_;
    bool keep_alive_;
};
static_assert(sizeof(LibraryLoader) == 64);
static_assert(alignof(LibraryLoader) == 8);
static_assert(std::is_same_v<std::shared_ptr<LibInfo>,
                             std::__h::shared_ptr<LibInfo>>);
} // namespace OHOS::ANCO

// Not a complete startup owner: the original runner/handler, FreezeManager,
// service/listener lifecycle and module retirement must be connected first.
// Destroying an active loader is NOT proof that detached workers are gone.
#endif
