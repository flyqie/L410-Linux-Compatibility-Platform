// Actual L410 native query/load check. Never simulates a reply or GPU result.
#include <cstdio>
#include <cstdint>
#include <dlfcn.h>
extern "C" int query(void *,uint64_t,uint32_t *,uint32_t *)
    asm("_ZN4OHOS5Rosen12RSInterfaces22GetRogScreenResolutionEmRjS2_");
int main(int argc,char **argv)
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    uint32_t width=0x12345678,height=0x76543210;
    int result=query(nullptr,0,&width,&height);
    std::printf("REAL_ROG_QUERY result=%d width=%u height=%u\n",result,width,height);
    if (argc>1) {
        void *library=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);
        if (!library) { std::printf("ORIGINAL_GRAPHIC_LOAD_ERROR %s\n",dlerror()); return 3; }
        std::puts("ORIGINAL_GRAPHIC_RTLD_NOW_OK_NOT_QUEUE_ACCEPTANCE");
        // Retain provider until process exit, no service object lifecycle claim.
    }
    return 0;
}
