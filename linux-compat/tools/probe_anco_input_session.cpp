// Real L410 registration/reopen/callback/ownership diagnostic, no user input.
#include "anco_native_input_session.hpp"
#include "anco_native_input_windows.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <unistd.h>
namespace {
std::atomic<unsigned> destroyed{0};
volatile std::sig_atomic_t stopping=0;
void stop(int){stopping=1;}
class Receiver final : public AncoNativeInput::Consumer {
    AncoNativeInput::HostWindows windows_;
public:
    std::atomic<unsigned> states{0};
    ~Receiver() override {++destroyed;}
    int Knuckle(bool state) override {
        ++states;std::printf("SESSION_REAL_KNUCKLE state=%d\n",state);return 0;
    }
    int Windows(const AncoOriginalInput::WindowsFields& fields) override {
        windows_.Apply(fields);
        const auto state=windows_.Read();
        std::printf("SESSION_REAL_WINDOWS update_type=%u focus=%d count=%zu\n",
            fields.update_type,fields.focus_window_id,fields.windows.size());
        for(const auto& window:fields.windows)
            std::printf("SESSION_REAL_WINDOW id=%d display=%d action=%u flags=%u transform=%zu areas=%zu,%zu\n",
                window.window_id,window.display_id,window.action,window.flags,
                window.transform.size(),window.hot_areas.size(),window.pointer_areas.size());
        std::printf("SESSION_WINDOW_STATE focus=%d container=%d retained=%zu\n",
            state.focus,state.container,state.records.size());
        return 0; // This diagnostic consumes/logs state, not Wayland focus.
    }
};
}
int main(int argc,char** argv){
    std::setbuf(stdout,nullptr);
    auto receiver=std::make_shared<Receiver>();std::weak_ptr<Receiver> weak=receiver;
    {
        AncoNativeInput::Session session(receiver);
        if(argc==2&&std::strcmp(argv[1],"--listen")==0){
            std::signal(SIGTERM,stop);std::signal(SIGINT,stop);
            int rc=session.Open();std::printf("SESSION_LISTEN result=%d\n",rc);
            if(rc)return 2;
            // Diagnostic remains until its owner stops it. No UI watchdog.
            while(!stopping)usleep(100000);
            session.Close();
        }else {
        for(unsigned round=0;round<2;++round){
            unsigned before=receiver->states.load();int rc=session.Open();
            std::printf("SESSION_OPEN round=%u result=%d\n",round,rc);
            if(rc)return 2;
            if(session.Open()!=0)return 3; // Idempotent, no duplicate channel.
            for(unsigned i=0;i<20&&receiver->states.load()==before;++i)usleep(100000);
            session.Close();session.Close();
            std::printf("SESSION_CLOSE round=%u callbacks=%u\n",round,receiver->states.load());
            if(receiver->states.load()==before)return 4;
        }
        }
    }
    receiver.reset();
    std::printf("SESSION_OWNERSHIP expired=%d destructors=%u\n",weak.expired(),destroyed.load());
    return weak.expired()&&destroyed.load()==1?0:5;
}
