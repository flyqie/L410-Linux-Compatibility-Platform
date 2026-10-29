// Native real-device registration/virtual callback/lifetime diagnostic.
// No vptr writes, offset-based reference casts, Android libraries or key input.
#include "anco_uds_native_abi.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
extern "C" int ConnectUdsServer(const char*,const char*,int,const char*);
extern "C" int GetFd(int);
extern "C" void CloseConn(int);
namespace {
std::atomic<unsigned> callbacks{0};
std::atomic<unsigned> destructions{0};
class InputChannel final : public UdsIpc::IPCObjectStub {
public:
    InputChannel():IPCObjectStub(u"ohos.anco.IAncoInputChannel") {}
    ~InputChannel() override {++destructions;std::puts("TYPED_ORIGINAL_INPUT_DESTRUCTOR");}
    int OnRemoteRequest(uint32_t code,UdsIpc::MessageParcel& data,
        UdsIpc::MessageParcel& reply,UdsIpc::MessageOption&) override
    {
        auto token=data.ReadInterfaceToken();
        bool valid=token==u"ohos.anco.IAncoInputChannel";
        std::printf("TYPED_ORIGINAL_INPUT_CALLBACK code=%u token=%d\n",code,valid);
        if(!valid)return 201;
        if(code!=3)return 203; // Other original callbacks not decoded yet.
        bool state=false;
        if(!reinterpret_cast<UdsIpc::Parcel&>(data).ReadBool(state))return 203;
        if(!reinterpret_cast<UdsIpc::Parcel&>(reply).WriteInt32(0))return 202;
        ++callbacks;
        std::printf("TYPED_ORIGINAL_KNUCKLE state=%d replied=1\n",state);
        return 0;
    }
};
class InputBus final : public UdsIpc::IUdsBus {
    std::atomic<int> connection_{-1},fd_{-1};
public:
    ~InputBus() override {Close();}
    bool Open() override
    {
        if(connection_.load()>=0)return true;
        int connection=ConnectUdsServer("virt_input_broker","virtinput.sock",1,"hmos");
        if(connection<0)return false;
        int fd=::GetFd(connection);
        connection_.store(connection);fd_.store(fd);
        if(fd<0||Send("input",6)!=6){Close();return false;}
        return true;
    }
    int Recv(void *data,size_t size,int flags) override
    {
        int fd=fd_.load();if(fd<0)return -1;
        ssize_t rc;do{rc=recv(fd,data,size,flags);}while(rc<0&&errno==EINTR);
        return static_cast<int>(rc);
    }
    int Send(const void *data,int size) override
    {
        int fd=fd_.load();if(fd<0||size<0)return -1;
        int total=0;auto *bytes=static_cast<const unsigned char*>(data);
        while(total<size){
            ssize_t rc=send(fd,bytes+total,size-total,MSG_NOSIGNAL);
            if(rc<0&&errno==EINTR)continue;
            if(rc<=0)return -1;
            total+=static_cast<int>(rc);
        }
        return total;
    }
    void Close() override
    {
        int connection=connection_.exchange(-1);
        int fd=fd_.exchange(-1);
        if(connection<0)return;
        if(fd>=0)shutdown(fd,SHUT_RDWR);
        CloseConn(connection);
    }
    int GetFd() override {return fd_.load();}
};
}
int main()
{
    std::setbuf(stdout,nullptr);
    auto bus=std::make_shared<InputBus>();
    auto invoker=std::make_shared<UdsIpc::UdsClientInvoker>(bus);
    if(!invoker->Open()){std::puts("TYPED_ORIGINAL_INPUT_OPEN_FAIL");return 2;}
    auto *channel=new InputChannel;
    auto *remote=static_cast<UdsIpc::IRemoteObject*>(channel);
    auto *ref=static_cast<UdsIpc::RefBase*>(channel);
    std::printf("TYPED_ORIGINAL_ABI base_size=%zu derived_size=%zu ref_offset=%td parcel_size=%zu invoker_size=%zu\n",
        sizeof(UdsIpc::IPCObjectStub),sizeof(InputChannel),
        reinterpret_cast<char*>(ref)-reinterpret_cast<char*>(channel),
        sizeof(UdsIpc::MessageParcel),sizeof(UdsIpc::UdsClientInvoker));
    ref->IncStrongRef(&channel);
    auto associated=std::shared_ptr<UdsIpc::IRemoteInvoker>(invoker,
        reinterpret_cast<UdsIpc::IRemoteInvoker*>(invoker.get()));
    int linked=remote->SetInvoker(associated);
    int result=3;
    {
        UdsIpc::MessageParcel data,reply;
        UdsIpc::MessageOption option(0);
        UdsIpc::sptr<UdsIpc::IRemoteObject> object{remote};
        if(!linked&&data.WriteInterfaceToken(u"ohos.anco.IInputInterface")&&data.WriteRemoteObject(object)){
            int transport=invoker->SendRequest(invoker->GetHandle(),1,data,reply,option);
            int service=transport?transport:reinterpret_cast<UdsIpc::Parcel&>(reply).ReadInt32();
            std::printf("TYPED_ORIGINAL_INPUT_INITIALIZE linked=%d transport=%d service=%d\n",linked,transport,service);
            if(!transport&&!service)result=0;
        }
    }
    // Bounded diagnostic, not a production/UI timeout. Real broker sends state.
    for(unsigned i=0;i<20&&!callbacks.load();++i)usleep(100000);
    invoker->Close();
    associated.reset();
    ref->DecStrongRef(&channel);
    unsigned actual=callbacks.load(),retired=destructions.load();
    std::printf("TYPED_ORIGINAL_INPUT_DONE result=%d callbacks=%u destructors=%u\n",result,actual,retired);
    return result?result:actual&&retired==1?0:4;
}
