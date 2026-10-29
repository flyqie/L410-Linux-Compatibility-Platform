/* Experimental Vulkan 1.0 X11 WSI. Hardware render, synchronous CPU readback.
 * Deliberately bounded: one instance/device, graphics queue, FIFO, RGBA8,
 * no stereo, protected surfaces, device groups or modern WSI extensions.
 * Loaded only through the opt-in candidate ICD JSON. */
#include <vulkan/vk_icd.h>
#include <X11/Xutil.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
static VkInstance wi;
static VkDevice wd;
static VkPhysicalDevice wp;
static PFN_vkGetDeviceProcAddr gdpa;
static VkQueue wq;
static uint32_t wf;
static VkPhysicalDeviceMemoryProperties memory_types;
#define REAL_I(n) ((PFN_##n)real_gipa(wi,#n))
#define REAL_D(n) ((PFN_##n)gdpa(wd,#n))
typedef struct WSwap {
 VkImage images[3]; VkDeviceMemory image_memory[3];
 VkBuffer buffer; VkDeviceMemory buffer_memory;
 VkCommandPool pool; VkCommandBuffer command; VkFence fence;
 void *mapped; Display *display; Window window; GC gc; XImage *ximage;
 uint32_t width,height,count,next; int acquired[3]; uint64_t frames;
 struct WSwap *link;
} WSwap;
static WSwap *swaps;
static PFN_vkVoidFunction wsi_proc(const char *);
static VkResult copy_list(uint32_t *count,void *out,const void *items,uint32_t available,size_t size){
 if(!out){*count=available;return VK_SUCCESS;}
 uint32_t n=*count<available?*count:available;memcpy(out,items,n*size);*count=n;
 return n<available?VK_INCOMPLETE:VK_SUCCESS;
}
static VkResult w_vkEnumerateInstanceExtensionProperties(const char *layer,uint32_t *count,VkExtensionProperties *out){
 if(layer)return VK_ERROR_LAYER_NOT_PRESENT;
 const VkExtensionProperties items[]={{VK_KHR_SURFACE_EXTENSION_NAME,25},{VK_KHR_XCB_SURFACE_EXTENSION_NAME,6},{VK_KHR_XLIB_SURFACE_EXTENSION_NAME,6}};
 return copy_list(count,out,items,3,sizeof(*items));
}
static VkResult w_vkCreateInstance(const VkInstanceCreateInfo *info,const VkAllocationCallbacks *allocator,VkInstance *out){
 if(wi)return VK_ERROR_TOO_MANY_OBJECTS;
 if(info->pApplicationInfo&&info->pApplicationInfo->apiVersion>VK_API_VERSION_1_0)return VK_ERROR_INCOMPATIBLE_DRIVER;
 VkInstanceCreateInfo ci=*info;ci.enabledExtensionCount=0;ci.ppEnabledExtensionNames=NULL;
 VkResult r=((PFN_vkCreateInstance)real_gipa(NULL,"vkCreateInstance"))(&ci,allocator,out);
 if(!r)wi=*out;
 return r;
}
static void w_vkDestroyInstance(VkInstance instance,const VkAllocationCallbacks *allocator){REAL_I(vkDestroyInstance)(instance,allocator);wi=VK_NULL_HANDLE;}
static VkResult w_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical,const char *layer,uint32_t *count,VkExtensionProperties *out){
 if(layer)return VK_ERROR_LAYER_NOT_PRESENT;
 const VkExtensionProperties item={VK_KHR_SWAPCHAIN_EXTENSION_NAME,68};return copy_list(count,out,&item,1,sizeof(item));
}
static VkResult w_vkCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo *info,const VkAllocationCallbacks *allocator,VkDevice *out){
 if(wd)return VK_ERROR_TOO_MANY_OBJECTS;
 /* Presentation submits on the first enabled graphics family. */
 uint32_t n=0;REAL_I(vkGetPhysicalDeviceQueueFamilyProperties)(physical,&n,NULL);
 VkQueueFamilyProperties *q=calloc(n,sizeof(*q));if(!q)return VK_ERROR_OUT_OF_HOST_MEMORY;
 REAL_I(vkGetPhysicalDeviceQueueFamilyProperties)(physical,&n,q);wf=UINT32_MAX;
 for(uint32_t j=0;j<info->queueCreateInfoCount;j++){uint32_t f=info->pQueueCreateInfos[j].queueFamilyIndex;if(f<n&&(q[f].queueFlags&VK_QUEUE_GRAPHICS_BIT)){wf=f;break;}}free(q);
 if(wf==UINT32_MAX)return VK_ERROR_FEATURE_NOT_PRESENT;
 VkDeviceCreateInfo ci=*info;ci.enabledExtensionCount=0;ci.ppEnabledExtensionNames=NULL;
 VkResult r=REAL_I(vkCreateDevice)(physical,&ci,allocator,out);if(r)return r;
 wd=*out;wp=physical;gdpa=(PFN_vkGetDeviceProcAddr)real_gipa(wi,"vkGetDeviceProcAddr");
 REAL_I(vkGetPhysicalDeviceMemoryProperties)(physical,&memory_types);
 REAL_D(vkGetDeviceQueue)(wd,wf,0,&wq);return VK_SUCCESS;
}
static PFN_vkVoidFunction w_vkGetDeviceProcAddr(VkDevice device,const char *name){PFN_vkVoidFunction p=wsi_proc(name);return p?p:gdpa(device,name);}
static void w_vkDestroyDevice(VkDevice device,const VkAllocationCallbacks *allocator){REAL_D(vkDestroyDevice)(device,allocator);wd=VK_NULL_HANDLE;wq=VK_NULL_HANDLE;}
static int surface_window(VkSurfaceKHR surface,Window *window){
 VkIcdSurfaceBase *s=(void*)(uintptr_t)surface;if(!s)return 0;
 if(s->platform==VK_ICD_WSI_PLATFORM_XCB){*window=((VkIcdSurfaceXcb*)s)->window;return 1;}
 if(s->platform==VK_ICD_WSI_PLATFORM_XLIB){*window=((VkIcdSurfaceXlib*)s)->window;return 1;}return 0;
}
static VkBool32 graphics_family(VkPhysicalDevice p,uint32_t f){
 uint32_t n=0;REAL_I(vkGetPhysicalDeviceQueueFamilyProperties)(p,&n,NULL);if(f>=n)return VK_FALSE;
 VkQueueFamilyProperties *q=calloc(n,sizeof(*q));if(!q)return VK_FALSE;
 REAL_I(vkGetPhysicalDeviceQueueFamilyProperties)(p,&n,q);VkBool32 yes=(q[f].queueFlags&VK_QUEUE_GRAPHICS_BIT)!=0;free(q);return yes;
}
static VkBool32 w_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice p,uint32_t f,xcb_connection_t *c,xcb_visualid_t v){return graphics_family(p,f);}
static VkBool32 w_vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice p,uint32_t f,Display *d,VisualID v){return graphics_family(p,f);}
static VkResult w_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice p,uint32_t f,VkSurfaceKHR s,VkBool32 *out){Window w;*out=surface_window(s,&w)&&graphics_family(p,f);return VK_SUCCESS;}
static VkResult w_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice p,VkSurfaceKHR s,VkSurfaceCapabilitiesKHR *out){
 Window w;if(!surface_window(s,&w))return VK_ERROR_SURFACE_LOST_KHR;
 Display *d=XOpenDisplay(NULL);if(!d)return VK_ERROR_SURFACE_LOST_KHR;
 XWindowAttributes a;int ok=XGetWindowAttributes(d,w,&a);XCloseDisplay(d);if(!ok)return VK_ERROR_SURFACE_LOST_KHR;
 *out=(VkSurfaceCapabilitiesKHR){.minImageCount=2,.maxImageCount=3,.currentExtent={a.width,a.height},.minImageExtent={1,1},.maxImageExtent={4096,4096},.maxImageArrayLayers=1,.supportedTransforms=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,.currentTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,.supportedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,.supportedUsageFlags=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};return VK_SUCCESS;
}
static VkResult w_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice p,VkSurfaceKHR s,uint32_t *n,VkSurfaceFormatKHR *out){const VkSurfaceFormatKHR a={VK_FORMAT_R8G8B8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};return copy_list(n,out,&a,1,sizeof(a));}
static VkResult w_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice p,VkSurfaceKHR s,uint32_t *n,VkPresentModeKHR *out){const VkPresentModeKHR a=VK_PRESENT_MODE_FIFO_KHR;return copy_list(n,out,&a,1,sizeof(a));}
static uint32_t memory_index(uint32_t bits,VkMemoryPropertyFlags flags){for(uint32_t i=0;i<memory_types.memoryTypeCount;i++)if((bits&(1u<<i))&&(memory_types.memoryTypes[i].propertyFlags&flags)==flags)return i;return UINT32_MAX;}
static void release_swap(WSwap *s){
 if(!s)return;
 if(s->mapped)REAL_D(vkUnmapMemory)(wd,s->buffer_memory);
 if(s->fence)REAL_D(vkDestroyFence)(wd,s->fence,NULL);
 if(s->pool)REAL_D(vkDestroyCommandPool)(wd,s->pool,NULL);
 if(s->buffer)REAL_D(vkDestroyBuffer)(wd,s->buffer,NULL);
 if(s->buffer_memory)REAL_D(vkFreeMemory)(wd,s->buffer_memory,NULL);
 for(uint32_t i=0;i<s->count;i++){if(s->images[i])REAL_D(vkDestroyImage)(wd,s->images[i],NULL);if(s->image_memory[i])REAL_D(vkFreeMemory)(wd,s->image_memory[i],NULL);}
 if(s->ximage)XDestroyImage(s->ximage);
 if(s->gc)XFreeGC(s->display,s->gc);
 if(s->display)XCloseDisplay(s->display);
 free(s);
}
static VkResult w_vkCreateSwapchainKHR(VkDevice device,const VkSwapchainCreateInfoKHR *info,const VkAllocationCallbacks *allocator,VkSwapchainKHR *out){
 if(info->imageFormat!=VK_FORMAT_R8G8B8A8_UNORM||info->imageArrayLayers!=1||info->minImageCount>3||info->flags||info->presentMode!=VK_PRESENT_MODE_FIFO_KHR||info->imageSharingMode!=VK_SHARING_MODE_EXCLUSIVE)return VK_ERROR_FEATURE_NOT_PRESENT;
 if(!info->imageExtent.width||!info->imageExtent.height||info->imageExtent.width>4096||info->imageExtent.height>4096)return VK_ERROR_INITIALIZATION_FAILED;
 WSwap *s=calloc(1,sizeof(*s));if(!s)return VK_ERROR_OUT_OF_HOST_MEMORY;VkResult r=VK_ERROR_SURFACE_LOST_KHR;
 s->count=info->minImageCount<2?2:info->minImageCount;s->width=info->imageExtent.width;s->height=info->imageExtent.height;
 if(!surface_window(info->surface,&s->window))goto fail;
 s->display=XOpenDisplay(NULL);if(!s->display)goto fail;
 XWindowAttributes a;if(!XGetWindowAttributes(s->display,s->window,&a)||a.depth!=24)goto fail;
 s->gc=XCreateGC(s->display,s->window,0,NULL);
 s->ximage=XCreateImage(s->display,a.visual,a.depth,ZPixmap,0,calloc(s->width*s->height,4),s->width,s->height,32,0);
 if(!s->gc||!s->ximage||!s->ximage->data||s->ximage->bits_per_pixel!=32){r=VK_ERROR_OUT_OF_HOST_MEMORY;goto fail;}
#define W_CHECK(call) do{r=(call);if(r){fprintf(stderr,"L410 WSI %s: %d\n",#call,r);goto fail;}}while(0)
 VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=info->imageFormat,.extent={s->width,s->height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=info->imageUsage|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
 for(uint32_t i=0;i<s->count;i++){
  W_CHECK(REAL_D(vkCreateImage)(wd,&ii,NULL,&s->images[i]));VkMemoryRequirements mr;REAL_D(vkGetImageMemoryRequirements)(wd,s->images[i],&mr);
  uint32_t mt=memory_index(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);if(mt==UINT32_MAX){r=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;}
  VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mt};
  W_CHECK(REAL_D(vkAllocateMemory)(wd,&ai,NULL,&s->image_memory[i]));W_CHECK(REAL_D(vkBindImageMemory)(wd,s->images[i],s->image_memory[i],0));
 }
 VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=(VkDeviceSize)s->width*s->height*4,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
 W_CHECK(REAL_D(vkCreateBuffer)(wd,&bi,NULL,&s->buffer));VkMemoryRequirements mr;REAL_D(vkGetBufferMemoryRequirements)(wd,s->buffer,&mr);
 uint32_t mt=memory_index(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);if(mt==UINT32_MAX){r=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;}
 VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mt};W_CHECK(REAL_D(vkAllocateMemory)(wd,&ai,NULL,&s->buffer_memory));W_CHECK(REAL_D(vkBindBufferMemory)(wd,s->buffer,s->buffer_memory,0));W_CHECK(REAL_D(vkMapMemory)(wd,s->buffer_memory,0,VK_WHOLE_SIZE,0,&s->mapped));
 VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=wf};W_CHECK(REAL_D(vkCreateCommandPool)(wd,&pi,NULL,&s->pool));
 VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=s->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};W_CHECK(REAL_D(vkAllocateCommandBuffers)(wd,&ca,&s->command));
 VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};W_CHECK(REAL_D(vkCreateFence)(wd,&fi,NULL,&s->fence));
 s->link=swaps;swaps=s;*out=(VkSwapchainKHR)(uintptr_t)s;fprintf(stderr,"L410 WSI swapchain %ux%u images=%u GPU render / CPU X11 present\n",s->width,s->height,s->count);return VK_SUCCESS;
fail:release_swap(s);return r;
#undef W_CHECK
}
static void w_vkDestroySwapchainKHR(VkDevice d,VkSwapchainKHR swap,const VkAllocationCallbacks *a){
 WSwap *s=(void*)(uintptr_t)swap;if(!s)return;REAL_D(vkDeviceWaitIdle)(d);
 WSwap **p=&swaps;while(*p&&*p!=s)p=&(*p)->link;if(*p)*p=s->link;
 fprintf(stderr,"L410 WSI destroyed frames=%llu\n",(unsigned long long)s->frames);release_swap(s);
}
static VkResult w_vkGetSwapchainImagesKHR(VkDevice d,VkSwapchainKHR swap,uint32_t *n,VkImage *out){WSwap *s=(void*)(uintptr_t)swap;return copy_list(n,out,s->images,s->count,sizeof(VkImage));}
static VkResult w_vkAcquireNextImageKHR(VkDevice d,VkSwapchainKHR swap,uint64_t timeout,VkSemaphore semaphore,VkFence fence,uint32_t *out){
 WSwap *s=(void*)(uintptr_t)swap;for(uint32_t i=0;i<s->count;i++){uint32_t k=(s->next+i)%s->count;if(s->acquired[k])continue;
 VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.signalSemaphoreCount=semaphore?1:0,.pSignalSemaphores=&semaphore};
 VkResult r=REAL_D(vkQueueSubmit)(wq,1,&submit,fence);if(r)return r;s->acquired[k]=1;s->next=(k+1)%s->count;*out=k;return VK_SUCCESS;}return timeout?VK_TIMEOUT:VK_NOT_READY;
}
static VkImageLayout native_layout(VkImageLayout layout){return layout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR?VK_IMAGE_LAYOUT_GENERAL:layout;}
static void w_vkCmdPipelineBarrier(VkCommandBuffer command,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t mc,const VkMemoryBarrier *mb,uint32_t bc,const VkBufferMemoryBarrier *bb,uint32_t ic,const VkImageMemoryBarrier *ib){
 if(!ic){REAL_D(vkCmdPipelineBarrier)(command,src,dst,flags,mc,mb,bc,bb,0,NULL);return;}
 /* VLA avoids a fallible allocation in a void Vulkan command. */
 VkImageMemoryBarrier translated[ic];memcpy(translated,ib,sizeof(translated));
 for(uint32_t i=0;i<ic;i++){translated[i].oldLayout=native_layout(ib[i].oldLayout);translated[i].newLayout=native_layout(ib[i].newLayout);}
 REAL_D(vkCmdPipelineBarrier)(command,src,dst,flags,mc,mb,bc,bb,ic,translated);
}
static VkResult w_vkCreateRenderPass(VkDevice d,const VkRenderPassCreateInfo *info,const VkAllocationCallbacks *a,VkRenderPass *out){
 VkRenderPassCreateInfo ci=*info;VkAttachmentDescription attachments[info->attachmentCount?info->attachmentCount:1];
 for(uint32_t i=0;i<info->attachmentCount;i++){attachments[i]=info->pAttachments[i];attachments[i].initialLayout=native_layout(attachments[i].initialLayout);attachments[i].finalLayout=native_layout(attachments[i].finalLayout);}ci.pAttachments=attachments;return REAL_D(vkCreateRenderPass)(d,&ci,a,out);
}
static VkResult w_vkQueuePresentKHR(VkQueue queue,const VkPresentInfoKHR *info){
 if(queue!=wq)return VK_ERROR_FEATURE_NOT_PRESENT;
 VkPipelineStageFlags stages[info->waitSemaphoreCount?info->waitSemaphoreCount:1];
 for(uint32_t j=0;j<info->waitSemaphoreCount;j++)stages[j]=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
 VkResult result=VK_SUCCESS;
 for(uint32_t k=0;k<info->swapchainCount;k++){
  WSwap *s=(void*)(uintptr_t)info->pSwapchains[k];uint32_t index=info->pImageIndices[k];VkResult r=VK_SUCCESS;
  if(index>=s->count||!s->acquired[index])return VK_ERROR_INITIALIZATION_FAILED;
#define P_CHECK(call) do{r=(call);if(r)goto done;}while(0)
  P_CHECK(REAL_D(vkResetCommandBuffer)(s->command,0));P_CHECK(REAL_D(vkResetFences)(wd,1,&s->fence));
  VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};P_CHECK(REAL_D(vkBeginCommandBuffer)(s->command,&bi));
  VkImageMemoryBarrier ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=s->images[index],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
  REAL_D(vkCmdPipelineBarrier)(s->command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&ib);
  VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={s->width,s->height,1}};
  REAL_D(vkCmdCopyImageToBuffer)(s->command,s->images[index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s->buffer,1,&copy);
  ib.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;ib.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT;ib.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;ib.newLayout=VK_IMAGE_LAYOUT_GENERAL;
  VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
  REAL_D(vkCmdPipelineBarrier)(s->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,NULL,1,&ib);P_CHECK(REAL_D(vkEndCommandBuffer)(s->command));
  VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=k?0:info->waitSemaphoreCount,.pWaitSemaphores=info->pWaitSemaphores,.pWaitDstStageMask=stages,.commandBufferCount=1,.pCommandBuffers=&s->command};
  P_CHECK(REAL_D(vkQueueSubmit)(queue,1,&submit,s->fence));P_CHECK(REAL_D(vkWaitForFences)(wd,1,&s->fence,VK_TRUE,5000000000ULL));
  unsigned char *src=s->mapped,*dst=(void*)s->ximage->data;
  for(uint32_t y=0;y<s->height;y++)for(uint32_t x=0;x<s->width;x++){uint32_t si=(y*s->width+x)*4,di=y*s->ximage->bytes_per_line+x*4;dst[di]=src[si+2];dst[di+1]=src[si+1];dst[di+2]=src[si];dst[di+3]=255;}
  XPutImage(s->display,s->window,s->gc,s->ximage,0,0,0,0,s->width,s->height);XSync(s->display,False);s->frames++;
  XWindowAttributes a;if(XGetWindowAttributes(s->display,s->window,&a)&&(a.width!=(int)s->width||a.height!=(int)s->height))r=VK_SUBOPTIMAL_KHR;
  /* FIFO emulation caps presentation to 60Hz; compositor runs independently. */
  usleep(16667);
done:s->acquired[index]=0;if(info->pResults)info->pResults[k]=r;if(r!=VK_SUCCESS)result=r;if(r<0)return r;
#undef P_CHECK
 }return result;
}
static PFN_vkVoidFunction wsi_proc(const char *name){
#define WRAP(n) if(!strcmp(name,#n))return (PFN_vkVoidFunction)w_##n
 WRAP(vkEnumerateInstanceExtensionProperties);WRAP(vkCreateInstance);WRAP(vkDestroyInstance);
 WRAP(vkEnumerateDeviceExtensionProperties);WRAP(vkCreateDevice);WRAP(vkDestroyDevice);WRAP(vkGetDeviceProcAddr);
 WRAP(vkGetPhysicalDeviceXcbPresentationSupportKHR);WRAP(vkGetPhysicalDeviceXlibPresentationSupportKHR);
 WRAP(vkGetPhysicalDeviceSurfaceSupportKHR);WRAP(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);WRAP(vkGetPhysicalDeviceSurfaceFormatsKHR);WRAP(vkGetPhysicalDeviceSurfacePresentModesKHR);
 WRAP(vkCreateSwapchainKHR);WRAP(vkDestroySwapchainKHR);WRAP(vkGetSwapchainImagesKHR);WRAP(vkAcquireNextImageKHR);WRAP(vkQueuePresentKHR);WRAP(vkCmdPipelineBarrier);WRAP(vkCreateRenderPass);
#undef WRAP
 return NULL;
}
