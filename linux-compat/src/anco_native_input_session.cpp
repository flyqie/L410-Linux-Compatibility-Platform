#include "anco_native_input_session.hpp"
#include "anco_uds_native_abi.h"
#include <atomic>
#include <cerrno>
#include <mutex>
#include <sys/socket.h>
#include <unistd.h>
extern "C" int ConnectUdsServer(const char*,const char*,int,const char*);
extern "C" int GetFd(int);
extern "C" void CloseConn(int);
namespace AncoNativeInput {
namespace {
class Bus final : public UdsIpc::IUdsBus {
    std::atomic<int> connection_{-1},fd_{-1};
public:
    ~Bus() override {Close();}
    bool Open() override {
        if(connection_.load()>=0)return true;
        int connection=ConnectUdsServer("virt_input_broker","virtinput.sock",1,"hmos");
        if(connection<0)return false;
        connection_.store(connection);fd_.store(::GetFd(connection));
        if(fd_.load()<0||Send("input",6)!=6){Close();return false;}
        return true;
    }
    int Recv(void* data,std::size_t size,int flags) override {
        int fd=fd_.load();if(fd<0)return -1;
        ssize_t rc;do{rc=recv(fd,data,size,flags);}while(rc<0&&errno==EINTR);
        return static_cast<int>(rc);
    }
    int Send(const void* data,int size) override {
        int fd=fd_.load();if(fd<0||size<0)return -1;
        int total=0;auto* bytes=static_cast<const unsigned char*>(data);
        while(total<size){
            ssize_t rc=send(fd,bytes+total,size-total,MSG_NOSIGNAL);
            if(rc<0&&errno==EINTR)continue;
            if(rc<=0)return -1;
            total+=static_cast<int>(rc);
        }
        return total;
    }
    void Close() override {
        int connection=connection_.exchange(-1),fd=fd_.exchange(-1);
        if(connection<0)return;
        if(fd>=0)shutdown(fd,SHUT_RDWR);
        CloseConn(connection);
    }
    int GetFd() override {return fd_.load();}
};
class Channel final : public UdsIpc::IPCObjectStub {
    std::shared_ptr<Consumer> consumer_;
public:
    explicit Channel(std::shared_ptr<Consumer> consumer)
        :IPCObjectStub(u"ohos.anco.IAncoInputChannel"),consumer_(std::move(consumer)) {}
    int OnRemoteRequest(uint32_t code,UdsIpc::MessageParcel& data,
        UdsIpc::MessageParcel& reply,UdsIpc::MessageOption&) override {
        if(data.ReadInterfaceToken()!=u"ohos.anco.IAncoInputChannel")return 201;
        auto& parcel=reinterpret_cast<UdsIpc::Parcel&>(data);
        int result=203;
        switch(code){
        case 0: {
            AncoOriginalInput::PointerFields fields;
            if(!AncoOriginalInput::ReadPointer(parcel,fields))return 203;
            result=consumer_->Pointer(fields);break;
        }
        case 1: {
            AncoOriginalInput::KeyFields fields;
            if(!AncoOriginalInput::ReadKey(parcel,fields))return 203;
            result=consumer_->Key(fields);break;
        }
        case 2: {
            AncoOriginalInput::WindowsFields fields;
            if(!AncoOriginalInput::ReadWindows(parcel,fields))return 203;
            result=consumer_->Windows(fields);break;
        }
        case 3: {
            bool state=false;
            if(!parcel.ReadBool(state))return 203;
            result=consumer_->Knuckle(state);break;
        }

        case 4: {
            AncoOriginalInput::OneHandFields fields;
            if(!AncoOriginalInput::ReadOneHand(parcel,fields))return 202;
            result=consumer_->OneHand(fields);break;
        }
        case 5: {
            std::vector<int> fields;
            if(!AncoOriginalInput::ReadExcludedKeys(parcel,fields))return 202;
            result=consumer_->ExcludedKeys(fields);break;
        }
        case 6: {
            std::vector<AncoOriginalInput::DeviceFields> fields;
            if(!AncoOriginalInput::ReadDevicesCallback(parcel,fields))return 202;
            result=consumer_->Devices(fields);break;
        }
        case 7: {
            bool state=false;
            if(!parcel.ReadBool(state))return 202;
            result=consumer_->TripleFinger(state);break;
        }
        default:return 203; // Unknown host command, not fabricated support.
        }
        return reinterpret_cast<UdsIpc::Parcel&>(reply).WriteInt32(result)?0:202;
    }
};
}
class Session::Impl {
public:
    std::shared_ptr<Consumer> consumer;
    std::mutex control;
    std::shared_ptr<Bus> bus;
    std::shared_ptr<UdsIpc::UdsClientInvoker> invoker;
    Channel* channel=nullptr;
    bool initialized=false;
    explicit Impl(std::shared_ptr<Consumer> value):consumer(std::move(value)) {}
    void Close() {
        initialized=false;
        if(invoker)invoker->Close(); // Original closes bus and joins callbacks.
        if(channel){
            static_cast<UdsIpc::RefBase*>(channel)->DecStrongRef(this);
            channel=nullptr;
        }
        invoker.reset();bus.reset();
    }
    int Open() {
        if(initialized)return 0;
        if(!consumer)return 203;
        bus=std::make_shared<Bus>();
        invoker=std::make_shared<UdsIpc::UdsClientInvoker>(bus);
        if(!invoker->Open()){Close();return 203;}
        channel=new Channel(consumer);
        static_cast<UdsIpc::RefBase*>(channel)->IncStrongRef(this);
        auto associated=std::shared_ptr<UdsIpc::IRemoteInvoker>(invoker,
            reinterpret_cast<UdsIpc::IRemoteInvoker*>(invoker.get()));
        int result=channel->SetInvoker(associated);
        if(!result){
            UdsIpc::MessageParcel data,reply;
            UdsIpc::MessageOption option(0);
            UdsIpc::sptr<UdsIpc::IRemoteObject> object{channel};
            if(!data.WriteInterfaceToken(u"ohos.anco.IInputInterface")||
                !data.WriteRemoteObject(object))result=203;
            else {
                result=invoker->SendRequest(invoker->GetHandle(),1,data,reply,option);
                if(!result&&!reinterpret_cast<UdsIpc::Parcel&>(reply).ReadInt32(result))result=203;
            }
        }
        if(result){Close();return result;}
        initialized=true;return 0;
    }
};
Session::Session(std::shared_ptr<Consumer> consumer):impl_(new Impl(std::move(consumer))) {}
Session::~Session(){Close();}
int Session::Open(){std::lock_guard<std::mutex> guard(impl_->control);return impl_->Open();}
int Session::ConnectionFd(){std::lock_guard<std::mutex> guard(impl_->control);return impl_->initialized&&impl_->bus?impl_->bus->GetFd():-1;}
void Session::Close(){std::lock_guard<std::mutex> guard(impl_->control);impl_->Close();}
}
