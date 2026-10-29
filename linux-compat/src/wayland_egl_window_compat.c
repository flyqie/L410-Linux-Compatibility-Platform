/* Stable libwayland-egl API, backed by the selected Mali driver's window ABI.
 * Never construct or inspect wl_egl_window ourselves. Process-private library. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void *library;
static void *(*create)(void *,int,int);
static void (*destroy)(void *),(*resize)(void *,int,int,int,int),(*size)(void *,int *,int *);
static void initialize(void) {
 library=dlopen("/opt/l410/mali/libmali.so.0.18.0",RTLD_NOW|RTLD_GLOBAL);
 if(!library){fprintf(stderr,"WAYLAND_WINDOW_ABI_LOAD %s\n",dlerror());return;}
 create=dlsym(library,"wl_egl_window_create");destroy=dlsym(library,"wl_egl_window_destroy");
 resize=dlsym(library,"wl_egl_window_resize");size=dlsym(library,"wl_egl_window_get_attached_size");
 if(!create||!destroy||!resize||!size){fprintf(stderr,"WAYLAND_WINDOW_ABI_SYMBOL_MISSING\n");create=NULL;}
}
void *wl_egl_window_create(void *surface,int width,int height) {
 pthread_once(&once,initialize);return create?create(surface,width,height):NULL;
}
void wl_egl_window_destroy(void *window){pthread_once(&once,initialize);if(create)destroy(window);}
void wl_egl_window_resize(void *window,int width,int height,int dx,int dy){pthread_once(&once,initialize);if(create)resize(window,width,height,dx,dy);}
void wl_egl_window_get_attached_size(void *window,int *width,int *height){pthread_once(&once,initialize);if(create)size(window,width,height);}
