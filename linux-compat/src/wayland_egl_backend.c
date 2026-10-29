#include <wayland-server.h>
/* Import the client's wl_buffer as EGLImage. No readback or CPU pixel access. */
#include "wayland_egl_backend.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
static void *library,*drm_query_compat,*wayland_client_compat,*gbm,*display;
static int fd=-1,initialized,bound;
static void *render_context,*render_surface;
static unsigned render_program,shm_texture;
static int shm_width,shm_height,unpack_subimage;
static unsigned long shm_frames,shm_allocations,shm_upload_calls;
struct compose_profile { unsigned frames; double upload_ms,finish_ms,total_ms; };
static struct compose_profile profiles[2];
static double monotonic_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1000000.0;}
static void renderer_close(void);
static void *(*getproc)(const char *);
static void (*destroy_gbm)(void *);
static unsigned (*terminate)(void *),(*unbind)(void *,void *);
static unsigned (*query_buffer)(void *,void *,int,int *),(*destroy_image)(void *,void *);
static void *(*create_image)(void *,void *,unsigned,void *,const int *);
static int (*egl_error)(void);
void l410_egl_close(struct wl_display *server){
 renderer_close();
 if(bound&&unbind)unbind(display,server);
 if(initialized&&terminate)terminate(display);
 if(gbm&&destroy_gbm)destroy_gbm(gbm);
 if(fd>=0)close(fd);
 if(library)dlclose(library);
 if(wayland_client_compat)dlclose(wayland_client_compat);
 if(drm_query_compat)dlclose(drm_query_compat);
 library=drm_query_compat=wayland_client_compat=gbm=display=NULL;fd=-1;bound=initialized=0;
}
int l410_egl_open(struct wl_display *server){
 const char *stage="drm-query-compat";
 drm_query_compat=dlopen("/opt/l410/mali/libdrm-query-compat.so",RTLD_NOW|RTLD_GLOBAL);if(!drm_query_compat)goto fail;
 stage="wayland-client-compat";
 wayland_client_compat=dlopen("/opt/l410/mali/libwayland-client.so.0",RTLD_NOW|RTLD_GLOBAL);if(!wayland_client_compat)goto fail;
 stage="mali";
 library=dlopen("/opt/l410/mali/libmali.so.0.18.0",RTLD_NOW|RTLD_GLOBAL);if(!library)goto fail;
 getproc=dlsym(library,"eglGetProcAddress");
 void *(*create_gbm)(int)=dlsym(library,"gbm_create_device");destroy_gbm=dlsym(library,"gbm_device_destroy");
 unsigned (*initialize)(void *,int *,int *)=dlsym(library,"eglInitialize");
 const char *(*query)(void *,int)=dlsym(library,"eglQueryString");
 terminate=dlsym(library,"eglTerminate");egl_error=dlsym(library,"eglGetError");
 stage="entry-points";
 if(!getproc||!create_gbm||!destroy_gbm||!initialize||!query||!terminate||!egl_error)goto fail;
 stage="platform-entry";
 void *(*platform)(unsigned,void *,const int *)=getproc("eglGetPlatformDisplayEXT");if(!platform)goto fail;
 stage="render-node";
 fd=open("/dev/dri/renderD128",O_RDWR|O_CLOEXEC);if(fd<0)goto fail;
 stage="gbm-device";
 gbm=create_gbm(fd);if(!gbm)goto fail;
 stage="egl-display";
 display=platform(0x31d7,gbm,NULL);int major,minor;
 if(!display)goto fail;
 stage="egl-initialize";
 if(!initialize(display,&major,&minor))goto fail;
 initialized=1;const char *extensions=query(display,0x3055);
 stage="wayland-extension";
 if(!extensions||!strstr(extensions,"EGL_WL_bind_wayland_display"))goto fail;
 unsigned (*bind)(void *,void *)=getproc("eglBindWaylandDisplayWL");
 unbind=getproc("eglUnbindWaylandDisplayWL");query_buffer=getproc("eglQueryWaylandBufferWL");
 create_image=getproc("eglCreateImageKHR");destroy_image=getproc("eglDestroyImageKHR");
 stage="wayland-bind";
 if(!bind||!unbind||!query_buffer||!create_image||!destroy_image||!bind(display,server))goto fail;
 bound=1;fprintf(stderr,"WAYLAND_EGL_BOUND %d.%d\n",major,minor);return 0;
fail:
 {const char *reason=dlerror();fprintf(stderr,"WAYLAND_EGL_INIT_FAILED stage=%s loader=%s initialized=%d egl=0x%x errno=%d\n",stage,reason?reason:"none",initialized,egl_error?egl_error():0,errno);}l410_egl_close(server);return -1;
}
void *l410_egl_import(struct wl_resource *buffer,int *width,int *height){
 if(!bound||!query_buffer(display,buffer,0x3057,width)||!query_buffer(display,buffer,0x3056,height))return NULL;
 const int attributes[]={0x3038};
 void *image=create_image(display,NULL,0x31d5,buffer,attributes);/* EGL_WAYLAND_BUFFER_WL */
 if(!image)fprintf(stderr,"WAYLAND_EGL_IMPORT_FAILED 0x%x\n",egl_error());
 return image;
}
void l410_egl_release(void *image){if(image&&initialized)destroy_image(display,image);}

/* GPU composition into a host-owned DMA-BUF. Source is the client's committed
 * EGLImage. No pixel readback, CPU copy, capture or transport of image data. */
#define GLFN(ret,name,args) static ret (*name) args
GLFN(void,glGenTextures,(int,unsigned*)); GLFN(void,glDeleteTextures,(int,const unsigned*));
GLFN(void,glBindTexture,(unsigned,unsigned)); GLFN(void,glTexParameteri,(unsigned,unsigned,int));
GLFN(void,glGenFramebuffers,(int,unsigned*)); GLFN(void,glDeleteFramebuffers,(int,const unsigned*));
GLFN(void,glBindFramebuffer,(unsigned,unsigned)); GLFN(void,glFramebufferTexture2D,(unsigned,unsigned,unsigned,unsigned,int));
GLFN(unsigned,glCheckFramebufferStatus,(unsigned)); GLFN(unsigned,glGetError,(void));
GLFN(void,glViewport,(int,int,int,int)); GLFN(void,glFinish,(void));
GLFN(unsigned,glCreateShader,(unsigned)); GLFN(void,glShaderSource,(unsigned,int,const char*const*,const int*));
GLFN(void,glCompileShader,(unsigned)); GLFN(void,glGetShaderiv,(unsigned,unsigned,int*)); GLFN(void,glDeleteShader,(unsigned));
GLFN(unsigned,glCreateProgram,(void)); GLFN(void,glAttachShader,(unsigned,unsigned));
GLFN(void,glBindAttribLocation,(unsigned,unsigned,const char*)); GLFN(void,glLinkProgram,(unsigned));
GLFN(void,glGetProgramiv,(unsigned,unsigned,int*)); GLFN(void,glDeleteProgram,(unsigned)); GLFN(void,glUseProgram,(unsigned));
GLFN(void,glVertexAttribPointer,(unsigned,int,unsigned,unsigned char,int,const void*));
GLFN(void,glEnableVertexAttribArray,(unsigned)); GLFN(void,glDrawArrays,(unsigned,int,int));
GLFN(void,glTexImage2D,(unsigned,int,int,int,int,int,unsigned,unsigned,const void*));
GLFN(void,glTexSubImage2D,(unsigned,int,int,int,int,int,unsigned,unsigned,const void*));
GLFN(void,glPixelStorei,(unsigned,int)); GLFN(const unsigned char*,glGetString,(unsigned));
GLFN(void,glActiveTexture,(unsigned)); GLFN(int,glGetUniformLocation,(unsigned,const char*));
GLFN(void,glUniform1i,(int,int)); GLFN(void,glUniform1f,(int,float)); GLFN(void,glUniform2f,(int,float,float));
GLFN(void,glClearColor,(float,float,float,float)); GLFN(void,glClear,(unsigned));
GLFN(void,glGetShaderInfoLog,(unsigned,int,int*,char*));
static void (*image_texture)(unsigned,void*);
static unsigned (*make_current)(void*,void*,void*,void*);
static unsigned (*destroy_context)(void*,void*),(*destroy_surface)(void*,void*);
static void renderer_close(void){
 if(render_context){
  if(shm_texture&&glDeleteTextures)glDeleteTextures(1,&shm_texture);
  fprintf(stderr,"WAYLAND_SHM_UPLOAD frames=%lu allocations=%lu calls=%lu unpack_subimage=%d\n",shm_frames,shm_allocations,shm_upload_calls,unpack_subimage);
  if(render_program&&glDeleteProgram)glDeleteProgram(render_program);
  if(make_current)make_current(display,NULL,NULL,NULL);
  if(render_surface&&destroy_surface)destroy_surface(display,render_surface);
  if(destroy_context)destroy_context(display,render_context);
 }
 render_context=render_surface=NULL;render_program=shm_texture=0;shm_width=shm_height=unpack_subimage=0;shm_frames=shm_allocations=shm_upload_calls=0;
}
static unsigned compile_shader(unsigned type,const char *source){
 unsigned shader=glCreateShader(type);if(!shader)return 0;
 glShaderSource(shader,1,&source,NULL);glCompileShader(shader);int ok=0;glGetShaderiv(shader,0x8b81 /* GL_COMPILE_STATUS */,&ok);
 if(!ok){
  char log[1024]={0};int length=0;
  if(glGetShaderInfoLog){glGetShaderInfoLog(shader,sizeof(log)-1,&length,log);fprintf(stderr,"WAYLAND_SHADER_ERROR type=0x%x %s\n",type,log);}
  glDeleteShader(shader);return 0;
 }
 return shader;
}
static int renderer_open(void){
 if(render_program)return 0;
 static const char *failed;
#define LOAD(name) do { name=dlsym(library,#name);if(!name){failed=#name;goto fail;} } while(0)
 LOAD(glGenTextures);LOAD(glDeleteTextures);LOAD(glBindTexture);LOAD(glTexParameteri);
 LOAD(glGenFramebuffers);LOAD(glDeleteFramebuffers);LOAD(glBindFramebuffer);LOAD(glFramebufferTexture2D);
 LOAD(glCheckFramebufferStatus);LOAD(glGetError);LOAD(glViewport);LOAD(glFinish);
 LOAD(glCreateShader);LOAD(glShaderSource);LOAD(glCompileShader);LOAD(glGetShaderiv);LOAD(glDeleteShader);
 LOAD(glCreateProgram);LOAD(glAttachShader);LOAD(glBindAttribLocation);LOAD(glLinkProgram);LOAD(glGetProgramiv);
 LOAD(glDeleteProgram);LOAD(glUseProgram);LOAD(glVertexAttribPointer);LOAD(glEnableVertexAttribArray);LOAD(glDrawArrays);
 LOAD(glPixelStorei);LOAD(glGetString);
 LOAD(glTexImage2D);LOAD(glTexSubImage2D);LOAD(glActiveTexture);LOAD(glGetUniformLocation);LOAD(glUniform1i);LOAD(glUniform1f);LOAD(glUniform2f);
 LOAD(glClearColor);LOAD(glClear);LOAD(glGetShaderInfoLog);
#undef LOAD
 image_texture=getproc("glEGLImageTargetTexture2DOES");if(!image_texture){failed="glEGLImageTargetTexture2DOES";goto fail;}
 unsigned (*bindapi)(unsigned)=dlsym(library,"eglBindAPI");
 unsigned (*choose)(void*,const int*,void**,int,int*)=dlsym(library,"eglChooseConfig");
 void *(*context)(void*,void*,void*,const int*)=dlsym(library,"eglCreateContext");
 void *(*pbuffer)(void*,void*,const int*)=dlsym(library,"eglCreatePbufferSurface");
 make_current=dlsym(library,"eglMakeCurrent");destroy_context=dlsym(library,"eglDestroyContext");destroy_surface=dlsym(library,"eglDestroySurface");
 if(!bindapi||!choose||!context||!pbuffer||!make_current||!destroy_context||!destroy_surface){failed="egl entry points";goto fail;}
 int attrs[]={0x3033,1,0x3040,4,0x3024,8,0x3023,8,0x3022,8,0x3021,8,0x3038};void *config=NULL;int count=0;
 if(!bindapi(0x30a0)||!choose(display,attrs,&config,1,&count)||!count){failed="eglChooseConfig";goto fail;}
 int ca[]={0x3098,2,0x3038},pa[]={0x3057,1,0x3056,1,0x3038};
 render_context=context(display,config,NULL,ca);if(!render_context){failed="eglCreateContext";goto fail;}
 render_surface=pbuffer(display,config,pa);
 if(!render_surface){failed="eglCreatePbufferSurface";goto fail;}
 if(!make_current(display,render_surface,render_surface,render_context)){failed="eglMakeCurrent";goto fail;}
 const char *gl_extensions=(const char*)glGetString(0x1f03);unpack_subimage=gl_extensions&&strstr(gl_extensions,"GL_EXT_unpack_subimage")!=NULL;
 fprintf(stderr,"WAYLAND_SHM_UNPACK_SUBIMAGE %d\n",unpack_subimage);
 unsigned vs=compile_shader(0x8b31,"attribute vec2 p; varying vec2 q; void main(){gl_Position=vec4(p,0.,1.);q=p*.5+.5;}");
 /* Texel coordinates, so the quad maps onto texel centres instead of texture
  * edges; linear filtering at a texture edge would otherwise bleed the
  * cleared border into the outermost pixel line of the window. */
 unsigned fs=compile_shader(0x8b30,
  "precision mediump float;"
  "varying vec2 q;"
  "uniform sampler2D source;"
  "uniform float bgra; uniform float opaque;"
  "uniform float src_x; uniform float src_y; uniform float src_w; uniform float src_h;"
  "uniform float tex_w; uniform float tex_h; uniform float box_half_x; uniform float box_half_y;"
  "uniform float flip;"
  "void main(){"
  " if(abs(q.x-0.5)>box_half_x||abs(q.y-0.5)>box_half_y)discard;"
  /* The vertical convention of the composition target decides this, so it stays
   * an explicit input rather than a hardcoded mapping. */
  " float v=mix(q.y,1.0-q.y,flip);"
  " vec2 t=vec2(src_x+q.x*src_w,src_y+v*src_h);"
  " t=clamp(t,vec2(0.5),vec2(tex_w-0.5,tex_h-0.5));"
  " vec4 c=texture2D(source,t/vec2(tex_w,tex_h));"
  " c=mix(c,c.bgra,bgra);"
  " c.a=mix(c.a,1.0,opaque);"
  " gl_FragColor=c;"
  "}");
 if(!vs||!fs){if(vs)glDeleteShader(vs);if(fs)glDeleteShader(fs);failed="shader compile";goto fail;}
 render_program=glCreateProgram();glAttachShader(render_program,vs);glAttachShader(render_program,fs);
 glBindAttribLocation(render_program,0,"p");glLinkProgram(render_program);glDeleteShader(vs);glDeleteShader(fs);
 int ok=0;glGetProgramiv(render_program,0x8b82,&ok);if(!ok){failed="program link";goto fail;}return 0;
fail:
 fprintf(stderr,"WAYLAND_RENDERER_FAILED %s egl=0x%x\n",failed?failed:"unknown",egl_error?egl_error():0);
 renderer_close();return -1;
}
static int compose_buffer(void *source,struct wl_shm_buffer *shm,
 const void *pixels,int pixel_stride,int source_x,int source_y,
 int source_width,int source_height,int target_fd,int width,int height,
 int stride,int size,int format,int invert_y){
#define L410_COMPOSE_REJECT(stage) do { fprintf(stderr,"L410_COMPOSE_REJECT %s source=%dx%d offset=%d,%d target=%dx%d stride=%d size=%d format=%d shm=%d pixels=%d\n",stage,source_width,source_height,source_x,source_y,width,height,stride,size,format,shm!=NULL,pixels!=NULL); } while(0)
 const int cpu_upload=shm!=NULL||pixels!=NULL;
 if((!source&&!cpu_upload)||target_fd<0||width<1||height<1||width>3840||height>2160||stride<(int64_t)width*4||size<(int64_t)stride*height||format!=12){L410_COMPOSE_REJECT("arguments");return -1;}
 if(renderer_open()){L410_COMPOSE_REJECT("renderer");return -1;}
 const int profile=getenv("L410_GPU_BENCH")!=NULL;
 const double begin_ms=profile?monotonic_ms():0;
 double upload_ms=0,finish_ms=0;
 int attrs[]={0x3057,width,0x3056,height,0x3271,0x34324241,0x3272,target_fd,0x3273,0,0x3274,stride,0x3038};
 void *target=create_image(display,NULL,0x3270,NULL,attrs);if(!target){L410_COMPOSE_REJECT("target-image");return -1;}
 unsigned textures[2]={0},frame=0;int result=-1,upload_width=0;
 int sample_x=source_x,sample_y=source_y;
 if(cpu_upload){if(!shm_texture)glGenTextures(1,&shm_texture);textures[0]=shm_texture;glGenTextures(1,&textures[1]);}else glGenTextures(2,textures);
 glActiveTexture(0x84c0);glBindTexture(0x0de1,textures[1]);image_texture(0x0de1,target);
 glGenFramebuffers(1,&frame);glBindFramebuffer(0x8d40,frame);glFramebufferTexture2D(0x8d40,0x8ce0,0x0de1,textures[1],0);
 if(glCheckFramebufferStatus(0x8d40)!=0x8cd5){L410_COMPOSE_REJECT("framebuffer");goto done;}
 glBindTexture(0x0de1,textures[0]);
 if(cpu_upload){
  const double upload_begin_ms=profile?monotonic_ms():0;
  if(source_width<1||source_height<1||source_width>3840||source_height>2160){L410_COMPOSE_REJECT("upload-source");goto done;}
  int sw=source_width,sh=source_height,ss=pixel_stride;uint32_t sf=WL_SHM_FORMAT_ARGB8888;
  const unsigned char *bytes=pixels;
  if(shm){sw=wl_shm_buffer_get_width(shm);sh=wl_shm_buffer_get_height(shm);
   ss=wl_shm_buffer_get_stride(shm);sf=wl_shm_buffer_get_format(shm);}
  if(source_x<0||source_y<0||(int64_t)source_x+source_width>sw||(int64_t)source_y+source_height>sh||ss<(int64_t)sw*4||(sf!=WL_SHM_FORMAT_ARGB8888&&sf!=WL_SHM_FORMAT_XRGB8888)||(!shm&&!bytes)){L410_COMPOSE_REJECT("upload-region");goto done;}
  int upload_width_local=ss==(int64_t)sw*4&&sw<=3840?sw:source_width;
  upload_width=upload_width_local;
  /* The upload starts at source_y. When packing just the crop width it also
   * starts at source_x; those offsets must not be applied a second time in
   * texture coordinates. A full-width upload still needs source_x. */
  sample_x=upload_width==source_width?0:source_x;
  sample_y=0;
  if(shm_width!=upload_width||shm_height!=source_height){
   glTexImage2D(0x0de1,0,0x1908,upload_width,source_height,0,0x1908,0x1401,NULL);
   if(glGetError()!=0){shm_width=shm_height=0;goto done;}
   shm_width=upload_width;shm_height=source_height;shm_allocations++;
  }
  shm_frames++;
  if(shm){wl_shm_buffer_begin_access(shm);bytes=wl_shm_buffer_get_data(shm);}
  const unsigned char *start=bytes+(size_t)source_y*ss+(size_t)(upload_width==source_width?source_x:0)*4;
  if(ss==upload_width*4||(unpack_subimage&&ss%4==0)){
   if(unpack_subimage)glPixelStorei(0x0cf2,ss/4); /* GL_UNPACK_ROW_LENGTH_EXT */
   glTexSubImage2D(0x0de1,0,0,0,upload_width,source_height,0x1908,0x1401,start);shm_upload_calls++;
   if(unpack_subimage)glPixelStorei(0x0cf2,0);
  }else for(int row=0;row<source_height;row++){glTexSubImage2D(0x0de1,0,0,row,source_width,1,0x1908,0x1401,start+(size_t)row*ss);shm_upload_calls++;}
  if(shm)wl_shm_buffer_end_access(shm);
  if(profile)upload_ms=monotonic_ms()-upload_begin_ms;
 }else image_texture(0x0de1,source);
 glTexParameteri(0x0de1,0x2801,0x2601);glTexParameteri(0x0de1,0x2800,0x2601);
 glTexParameteri(0x0de1,0x2802,0x812f);glTexParameteri(0x0de1,0x2803,0x812f);
 glUseProgram(render_program);glUniform1i(glGetUniformLocation(render_program,"source"),0);
 glUniform1f(glGetUniformLocation(render_program,"bgra"),shm?1.f:0.f);
 glUniform1f(glGetUniformLocation(render_program,"opaque"),shm&&wl_shm_buffer_get_format(shm)==WL_SHM_FORMAT_XRGB8888?1.f:0.f);
 /* A configure acknowledgement does not guarantee the attached buffer has
  * the new extent. Keep its pixels at 1:1 size instead of resampling stale
  * content across the resized native target. GL clips an oversized viewport;
  * the unpainted portion of a larger target stays at the clear colour until
  * the client submits a matching buffer. */
 int vx=0,vy=0,vw=source_width<width?source_width:width;
 int vh=source_height<height?source_height:height;
 glUniform1f(glGetUniformLocation(render_program,"src_x"),(float)sample_x);
 glUniform1f(glGetUniformLocation(render_program,"src_y"),(float)sample_y);
 glUniform1f(glGetUniformLocation(render_program,"src_w"),(float)vw);
 glUniform1f(glGetUniformLocation(render_program,"src_h"),(float)vh);
 glUniform1f(glGetUniformLocation(render_program,"tex_w"),(float)(cpu_upload?upload_width:source_width));
 glUniform1f(glGetUniformLocation(render_program,"flip"),invert_y?1.f:0.f);
 glUniform1f(glGetUniformLocation(render_program,"tex_h"),(float)source_height);
 glUniform1f(glGetUniformLocation(render_program,"box_half_x"),.5f);
 glUniform1f(glGetUniformLocation(render_program,"box_half_y"),.5f);
 /* Composition geometry is logged once per change, so an unexplained band in the
  * window can be attributed to the source rectangle or to the fit. */
 static int seen_w=-1,seen_h=-1,seen_sx=-1,seen_sy=-1,seen_sw=-1,seen_sh=-1,seen_vx=-1,seen_vy=-1,seen_vw=-1,seen_vh=-1;
 if(width!=seen_w||height!=seen_h||source_x!=seen_sx||source_y!=seen_sy||source_width!=seen_sw||
    source_height!=seen_sh||vx!=seen_vx||vy!=seen_vy||vw!=seen_vw||vh!=seen_vh){
  seen_w=width;seen_h=height;seen_sx=source_x;seen_sy=source_y;seen_sw=source_width;seen_sh=source_height;
  seen_vx=vx;seen_vy=vy;seen_vw=vw;seen_vh=vh;
  fprintf(stderr,"COMPOSE_GEOMETRY target=%dx%d source=%d,%d %dx%d sample=%d,%d viewport=%d,%d %dx%d flip=%d shm=%d buffer=%dx%d upload=%dx%d\n",
   width,height,source_x,source_y,source_width,source_height,sample_x,sample_y,vx,vy,vw,vh,invert_y,shm!=NULL,
   shm?wl_shm_buffer_get_width(shm):source_width,shm?wl_shm_buffer_get_height(shm):source_height,
   upload_width,source_height);
 }
 static const float quad[]={-1,-1,1,-1,-1,1,1,1};
 glClearColor(.05f,.05f,.06f,1.f);glClear(0x4000);
 glViewport(vx,vy,vw,vh);glVertexAttribPointer(0,2,0x1406,0,0,quad);glEnableVertexAttribArray(0);glDrawArrays(5,0,4);
 /* Native fence fd export is absent on this driver. Finish is conservative
  * completion of source reads/target writes, not scanout completion. */
 const double finish_begin_ms=profile?monotonic_ms():0;
 glFinish();if(profile)finish_ms=monotonic_ms()-finish_begin_ms;
 if(glGetError()==0)result=0;
 if(profile&&result==0){
  struct compose_profile *p=&profiles[cpu_upload!=0];
  p->frames++;p->upload_ms+=upload_ms;p->finish_ms+=finish_ms;p->total_ms+=monotonic_ms()-begin_ms;
  if(p->frames==30){
   fprintf(stderr,"GPU_COMPOSE_TIMING kind=%s samples=%u target=%dx%d upload_ms=%.3f finish_ms=%.3f total_ms=%.3f\n",
    shm?"shm":pixels?"pixels":"egl-image",p->frames,width,height,p->upload_ms/p->frames,p->finish_ms/p->frames,p->total_ms/p->frames);
   *p=(struct compose_profile){0};
  }
 }
done:
 glBindFramebuffer(0x8d40,0);glDeleteFramebuffers(1,&frame);if(cpu_upload)glDeleteTextures(1,&textures[1]);else glDeleteTextures(2,textures);destroy_image(display,target);
 return result;
}

/* Imported EGLImage: source extent is the buffer's own pixel size, target is
 * the host NativeWindow buffer, which may differ while a resize is settling. */
int l410_egl_compose(void *source,int source_width,int source_height,int fd,int width,int height,int stride,int size,int format,int invert_y){return compose_buffer(source,NULL,NULL,0,0,0,source_width,source_height,fd,width,height,stride,size,format,invert_y);}
int l410_egl_compose_shm(struct wl_shm_buffer *shm,int x,int y,int source_width,int source_height,int fd,int width,int height,int stride,int size,int format){return compose_buffer(NULL,shm,NULL,0,x,y,source_width,source_height,fd,width,height,stride,size,format,0);}
int l410_egl_compose_pixels(const void *pixels,int pixel_stride,
 int source_width,int source_height,int target_fd,int width,int height,
 int target_stride,int size,int format){return compose_buffer(NULL,NULL,pixels,
 pixel_stride,0,0,source_width,source_height,target_fd,width,height,
 target_stride,size,format,0);}
