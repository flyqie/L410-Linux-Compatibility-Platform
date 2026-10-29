// Linux-side consumers only. These are not new host interfaces.
// Original host IInputInterface/IAncoInputChannel protocols remain unchanged.
#pragma once
#include "anco_stock_input_events.hpp"
#include <memory>
namespace AncoNativeInput {
class Consumer {
public:
    virtual ~Consumer() = default;
    // Return the actual original service result. Unhandled is 203.
    // Calls run on the original invoker thread; consumers must queue state
    // to their Linux event loop rather than call Wayland across threads.
    virtual int Pointer(const AncoOriginalInput::PointerFields&) {return 203;}
    virtual int Key(const AncoOriginalInput::KeyFields&) {return 203;}
    virtual int Windows(const AncoOriginalInput::WindowsFields&) {return 203;}
    virtual int Knuckle(bool) {return 203;}
    virtual int OneHand(const AncoOriginalInput::OneHandFields&) {return 203;}
    virtual int ExcludedKeys(const std::vector<int>&) {return 203;}
    virtual int Devices(const std::vector<AncoOriginalInput::DeviceFields>&) {return 203;}
    virtual int TripleFinger(bool) {return 203;}
};
class Session final {
public:
    explicit Session(std::shared_ptr<Consumer>);
    ~Session();
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    // Open/Close are serialized. Do not call them from a Consumer callback.
    int Open();
    // Borrowed actual bus fd for its sole owner to poll HUP/ERR. Do not read,
    // write, close or call this from a Consumer callback. Close remains owner.
    int ConnectionFd();
    void Close();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
