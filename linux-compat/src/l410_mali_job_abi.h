/* Existing L410 r18 adaptation, scoped to this glibc compatibility process.
 * Mate30 submits 48-byte atoms; L410 adds an 8-byte throttle/padding tail.
 * Evidence/source: hmos-devshim/l410-vulkan-r18-icd-v1 runtime compat. */
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
static int l410_job_ioctl(int fd,unsigned long request,...){
 va_list va;va_start(va,request);void *arg=va_arg(va,void*);va_end(va);
 struct job_submit {uint64_t addr;uint32_t count,stride;};
 const struct job_submit *job=arg;
 if(request!=0x40108002UL||!job||job->stride!=48||!job->count||job->count>256||!job->addr)return syscall(SYS_ioctl,fd,request,arg);
 uint8_t *atoms=calloc(job->count,56);if(!atoms){errno=ENOMEM;return -1;}
 for(uint32_t i=0;i<job->count;i++)memcpy(atoms+i*56,(const void*)(uintptr_t)(job->addr+i*48),48);
 struct job_submit translated={(uint64_t)(uintptr_t)atoms,job->count,56};
 int result=syscall(SYS_ioctl,fd,request,&translated);free(atoms);return result;
}
#include "l410_ahb_trace.h"
static void *l410_hook(const char *symbol,const char *requester){(void)requester;if(getenv("L410_AHB_TRACE")&&!strcmp(symbol,"AHardwareBuffer_allocate"))return (void*)l410_trace_ahb_allocate;return strcmp(symbol,"ioctl")==0?(void*)l410_job_ioctl:NULL;}
extern void hybris_set_hook_callback(void *(*callback)(const char*,const char*));
