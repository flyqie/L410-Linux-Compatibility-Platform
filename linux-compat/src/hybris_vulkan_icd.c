/* Private Vulkan loader adapter. No replacement of the host Vulkan libraries. */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "l410_mali_job_abi.h"
extern void *android_dlopen(const char *,int);
extern void *android_dlsym(void *,const char *);
extern char *android_dlerror(void);
static pthread_once_t once=PTHREAD_ONCE_INIT;
static PFN_vkGetInstanceProcAddr real_gipa;
static void initialize(void){
 hybris_set_hook_callback(l410_hook);
 void *lib=android_dlopen("/opt/l410/vulkan-bionic/libGLES_mali.so",RTLD_NOW);
 if(lib)real_gipa=(PFN_vkGetInstanceProcAddr)android_dlsym(lib,"vkGetInstanceProcAddr");
 else fprintf(stderr,"L410 Vulkan load: %s\n",android_dlerror());
}
#ifdef L410_X11_WSI
#include "vulkan_x11_wsi.h"
#endif
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,const char *name){
 pthread_once(&once,initialize);
#ifdef L410_X11_WSI
 PFN_vkVoidFunction wrapped=wsi_proc(name);if(wrapped)return wrapped;
 /* Legacy loader owns Linux surface allocation/destruction. Never forward these
  * to Android, which interprets surface pointers as Android native windows. */
 if(!strcmp(name,"vkCreateXcbSurfaceKHR")||!strcmp(name,"vkCreateXlibSurfaceKHR")||!strcmp(name,"vkDestroySurfaceKHR"))return NULL;
#endif
 return real_gipa?real_gipa(instance,name):NULL;
}
VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version){if(*version>1)*version=1;return VK_SUCCESS;}
