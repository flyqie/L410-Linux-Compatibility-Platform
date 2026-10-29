// Original native HDF service implementation + original generated IPC Stub.
// Only service-instance registration is adapted; no new allocator protocol,
// Android framework, fake driver/device object or method implementation.
#include <cstdio>
#include <cstring>
#include <csignal>
#include <dlfcn.h>
#include <unistd.h>
#include <map> // Load libc++ before IPC's transitive kernel inline macro.
#include <new>
#include "object_collector.h"
#include "hdf_remote_service.h"
#include "ipc_skeleton.h"

extern "C" HdfRemoteService *HdfRemoteAdapterBind(const OHOS::sptr<OHOS::IRemoteObject> &);
// Bind creates this original holder around an arbitrary native IRemoteObject.
// AdapterRecycle is only safe for its own HdfRemoteServiceStub: it casts every
// local remote to that class and touches its private mutex. AllocatorStub is a
// different original class. Pair Bind with the holder's actual native destructor.
extern "C" void holder_destruct(HdfRemoteService *) asm("_ZN22HdfRemoteServiceHolderD1Ev");
static void release_bound_holder(HdfRemoteService *holder)
{
    if(holder){holder_destruct(holder);::operator delete(holder);}
}
static volatile sig_atomic_t stop_requested;
static void stop(int){stop_requested=1;}
template<class F> static F resolve(void *library,const char *name)
{
    void *symbol=dlsym(library,name);F result=nullptr;
    std::memcpy(&result,&symbol,sizeof(result));return result;
}
struct Sbuf {
    HdfSBuf *pointer=HdfSbufTypedObtain(SBUF_IPC);
    ~Sbuf(){HdfSbufRecycle(pointer);}
};
static int transaction(HdfRemoteService *manager,int code,const char *name,HdfRemoteService *service=nullptr)
{
    Sbuf data,reply;
    if(!data.pointer||!reply.pointer||!HdfRemoteServiceWriteInterfaceToken(manager,data.pointer)||
       !HdfSbufWriteString(data.pointer,name))return -1000;
    if(code==1){
        // Original DevSvcManagerProxy::WriteServiceInfo, exact native fields.
        // This pure allocator instance claims no physical device: devId0.
        if(!HdfSbufWriteUint16(data.pointer,1)||!HdfSbufWriteUint32(data.pointer,0)||
           HdfSbufWriteRemoteService(data.pointer,service)!=0||
           !HdfSbufWriteString(data.pointer,"l410-linux native allocator")||
           !HdfSbufWriteString(data.pointer,"ohos.hdi.display.buffer.v1_4.IAllocator"))return -1001;
    }
    int result=manager->dispatcher->Dispatch(manager,code,data.pointer,reply.pointer);
    if(code==3&&result==0){
        auto *existing=HdfSbufReadRemoteService(reply.pointer);
        if(!existing)return -1002;
        HdfRemoteServiceRecycle(existing);
    }
    return result;
}
int main(int argc,char **argv)
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc!=4||std::strcmp(argv[2],"l410_linux_allocator_service")!=0)return 64;
    void *library=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);
    if(!library){std::printf("ALLOCATOR_STUB_LOAD_ERROR %s\n",dlerror());return 2;}
    HdfRemoteService *manager=HdfRemoteServiceGet(5100);
    if(!manager||!HdfRemoteServiceSetInterfaceDesc(manager,"HDI.IServiceManager.V1_0"))return 3;
    if(std::strcmp(argv[3],"--query")==0){
        int rc=transaction(manager,3,argv[2]);
        std::printf("ORIGINAL_HDF_QUERY status=%d\n",rc);HdfRemoteServiceRecycle(manager);
        return rc==0?0:4;
    }
    if(std::strcmp(argv[3],"--client")==0){
        using GetName=OHOS::sptr<OHOS::HDI::HdiBase> (*)(const std::string &,bool);
        auto get=resolve<GetName>(library,"_ZN4OHOS3HDI7Display6Buffer4V1_410IAllocator3GetERKNSt3__h12basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb");
        if(!get){HdfRemoteServiceRecycle(manager);return 12;}
        auto proxy=get(argv[2],false);
        // Original proxy Get enforces real GetVersion(major1/minor>=4).
        // A null/error is preserved; no forced version or synthetic result.
        std::printf("ORIGINAL_ALLOCATOR_14_IPC_GET validated_proxy=%d\n",proxy!=nullptr);
        HdfRemoteServiceRecycle(manager);return proxy?0:13;
    }
    if(std::strcmp(argv[3],"--serve")!=0){HdfRemoteServiceRecycle(manager);return 64;}
    if(transaction(manager,3,argv[2])==0){std::puts("INSTANCE_ALREADY_EXISTS_NO_REPLACEMENT");HdfRemoteServiceRecycle(manager);return 5;}
    using Get=OHOS::sptr<OHOS::HDI::HdiBase> (*)(bool);
    auto get=resolve<Get>(library,"_ZN4OHOS3HDI7Display6Buffer4V1_410IAllocator3GetEb");
    if(!get){HdfRemoteServiceRecycle(manager);return 6;}
    auto implementation=get(true);
    if(!implementation){HdfRemoteServiceRecycle(manager);return 7;}
    auto &collector=OHOS::HDI::ObjectCollector::GetInstance();
    auto object=collector.GetOrNewObject(implementation,u"ohos.hdi.display.buffer.v1_4.IAllocator");
    if(!object){HdfRemoteServiceRecycle(manager);return 8;}
    HdfRemoteService *service=HdfRemoteAdapterBind(object);
    if(!service){collector.RemoveObject(implementation);HdfRemoteServiceRecycle(manager);return 9;}
    bool threads=OHOS::IPCSkeleton::SetMaxWorkThreadNum(4);
    std::signal(SIGTERM,stop);std::signal(SIGINT,stop);
    int rc=transaction(manager,1,argv[2],service);
    std::printf("ORIGINAL_HDF_ALLOCATOR_REGISTER status=%d threads=%d pid=%d\n",rc,threads,getpid());
    int result=rc==0?0:10;
    if(rc==0){
        std::puts("ORIGINAL_ALLOCATOR_IPC_INSTANCE_READY");
        while(!stop_requested)sleep(1);
        int removed=transaction(manager,8,argv[2]);
        std::printf("ORIGINAL_HDF_ALLOCATOR_REMOVE status=%d\n",removed);
        if(removed)result=11;
    }
    release_bound_holder(service);collector.RemoveObject(implementation);
    HdfRemoteServiceRecycle(manager);
    std::puts("ORIGINAL_ALLOCATOR_STUB_AND_IMPL_RELEASE");
    return result;
}
