#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Decimal settings need the baseline glibc ABI, not host C23 redirects. */
extern unsigned long l410_strtoul(const char *,char **,int) __asm__("strtoul");
extern unsigned long long l410_strtoull(const char *,char **,int) __asm__("strtoull");
#define strtoul l410_strtoul
#define strtoull l410_strtoull

static pid_t command_pid;
static pid_t init_pid;
static pid_t owner_pid;
static unsigned long long owner_start;
/* Watch process identity, not PR_SET_PDEATHSIG against the short-lived Python
 * request thread that forked us. A terminated broker cannot leave GUI children. */
static int owner_alive(void) {
    if(!owner_pid)return 1;
    char pathbuf[80],buf[4096];snprintf(pathbuf,sizeof(pathbuf),"/proc/%d/stat",owner_pid);
    int fd=open(pathbuf,O_RDONLY|O_CLOEXEC);if(fd<0)return 0;
    ssize_t n=read(fd,buf,sizeof(buf)-1);close(fd);if(n<=0)return 0;buf[n]=0;
    char *rest=strrchr(buf,')');if(!rest)return 0;
    char *save=NULL,*field=strtok_r(rest+1," ",&save);int number=3;
    while(field){
        if(number==3 && (*field=='Z'||*field=='X'))return 0;
        if(number==22){char *end;errno=0;unsigned long long value=strtoull(field,&end,10);return !errno && !*end && value==owner_start;}
        field=strtok_r(NULL," ",&save);number++;
    }
    return 0;
}
static unsigned long memory_mib=2048,pids_limit=512;
static const char *shared_mode="readwrite";
static unsigned long number_setting(const char *name,unsigned long fallback,unsigned long min,unsigned long max){
    const char *s=getenv(name);if(!s)return fallback;char *end;errno=0;unsigned long value=strtoul(s,&end,10);
    if(errno||!*s||*end||value<min||value>max){fprintf(stderr,"invalid %s\n",name);exit(2);}return value;
}
static void parent_forward(int sig) { if(init_pid>0)kill(init_pid,sig); }
static void die(const char *s) { perror(s); exit(1); }
static void mk(const char *s,mode_t m) { if(mkdir(s,m)&&errno!=EEXIST)die(s); }
static void put(const char *p,const char *value){
    int fd=open(p,O_WRONLY|O_CLOEXEC);if(fd<0)die(p);
    size_t n=strlen(value);if(write(fd,value,n)!=(ssize_t)n)die(p);close(fd);
}
static void cgroup_paths(char *mem,char *pids,size_t size){
    mk("/dev/memcg/l410-linux",0755);mk("/dev/pids/l410-linux",0755);
    snprintf(mem,size,"/dev/memcg/l410-linux/run-%d",getpid());
    snprintf(pids,size,"/dev/pids/l410-linux/run-%d",getpid());
    mk(mem,0755);mk(pids,0755);
    char p[256],value[32];snprintf(value,sizeof(value),"%lu",memory_mib*1024*1024);
    snprintf(p,sizeof(p),"%s/memory.limit_in_bytes",mem);put(p,value);
    snprintf(value,sizeof(value),"%lu",pids_limit);snprintf(p,sizeof(p),"%s/pids.max",pids);put(p,value);
}
static void path(char *out,size_t size,const char *root,const char *suffix) {
    if(snprintf(out,size,"%s%s",root,suffix)>=(int)size) { errno=ENAMETOOLONG; die("path"); }
}
static void tmp(const char *root,const char *suffix,const char *opts) {
    char p[1024]; path(p,sizeof(p),root,suffix); mk(p,0755);
    if(mount("tmpfs",p,"tmpfs",MS_NOSUID|MS_NODEV,opts))die(p);
}
static void bind_immutable_runtime(const char *root,const char *source,const char *suffix,int directory) {
    struct stat st;char target[1024];
    if(stat(source,&st))die(source);
    if((directory&&!S_ISDIR(st.st_mode))||(!directory&&!S_ISREG(st.st_mode))){errno=EINVAL;die(source);}
    path(target,sizeof(target),root,suffix);
    if(directory)mk(target,0555);
    else {
        int fd=open(target,O_CREAT|O_RDWR|O_CLOEXEC,0600);
        if(fd<0)die(target);
        close(fd);
    }
    if(mount(source,target,NULL,MS_BIND,NULL))die(source);
    if(mount(NULL,target,NULL,MS_REMOUNT|MS_BIND|MS_RDONLY|MS_NOSUID|MS_NODEV,NULL))die(source);
}
static void bind_reviewed_linux_runtime(const char *root) {
    static const char *directories[]={
        "/opt/l410/anco-host-record/lib",
        "/opt/l410/diagnostics/host-dependency-r1-20261006",
        "/opt/l410/fonts",
        "/opt/l410/hybris",
        "/opt/l410/platforms",
        "/opt/l410/protocol/gralloc-sp68",
        "/opt/l410/vulkan-bionic"
    };
    static const char *asset_files[]={
        "/opt/l410/anco-host-record/anco-bundle-decode",
        "/opt/l410/anco-host-record/decode-record",
        "/opt/l410/anco-host-record/libjson-glib-1.0.so.0",
        "/opt/l410/mali/libGL.so.1",
        "/opt/l410/mali/libc_secshared.so",
        "/opt/l410/mali/libdrm-query-compat.so"
    };
    static const struct { const char *source; const char *target; } reused[]={
        {"/vendor/l410-kylin-runtime/usr/lib/aarch64-linux-gnu/libffi.so.7.1.0","/opt/l410/mali/libffi.so.7"},
        {"/vendor/l410-kylin-runtime/usr/lib/aarch64-linux-gnu/libmali.so.0.18.0","/opt/l410/mali/libmali.so.0.18.0"},
        {"/vendor/l410-kylin-runtime/usr/lib/aarch64-linux-gnu/libwayland-client.so.0.3.0","/opt/l410/mali/libwayland-client.so.0"},
        {"/vendor/l410-kylin-runtime/usr/lib/aarch64-linux-gnu/libwayland-server.so.0.1.0","/opt/l410/mali/libwayland-server.so.0"}
    };
    const char *asset="/vendor/l410/linux-rootfs-runtime-v1";
    char source[1024];
    for(size_t i=0;i<sizeof(directories)/sizeof(directories[0]);i++){
        if(snprintf(source,sizeof(source),"%s%s",asset,directories[i])>=(int)sizeof(source)){errno=ENAMETOOLONG;die("immutable runtime source");}
        bind_immutable_runtime(root,source,directories[i],1);
    }
    for(size_t i=0;i<sizeof(asset_files)/sizeof(asset_files[0]);i++){
        if(snprintf(source,sizeof(source),"%s%s",asset,asset_files[i])>=(int)sizeof(source)){errno=ENAMETOOLONG;die("immutable runtime source");}
        bind_immutable_runtime(root,source,asset_files[i],0);
    }
    for(size_t i=0;i<sizeof(reused)/sizeof(reused[0]);i++)
        bind_immutable_runtime(root,reused[i].source,reused[i].target,0);
}
static void forward(int sig) { if(command_pid>0)kill(-command_pid,sig); }
static void reduce_caps(int bootstrap) {
    const int denied[]={CAP_SYS_ADMIN,CAP_SYS_MODULE,CAP_SYS_RAWIO,CAP_SYS_BOOT,
        CAP_SYS_TIME,CAP_SYS_PTRACE,CAP_NET_ADMIN,CAP_NET_RAW,CAP_MKNOD,
        CAP_MAC_ADMIN,CAP_MAC_OVERRIDE,CAP_SYSLOG,CAP_SYS_NICE,CAP_SYS_RESOURCE};
    struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct d[2];
    if(syscall(SYS_capget,&h,d))die("capget");
    for(size_t i=0;i<sizeof(denied)/sizeof(denied[0]);i++) {
        int c=denied[i]; unsigned bit=1U<<(c%32);
        if(bootstrap && c==CAP_MKNOD)continue;
        if(prctl(PR_CAPBSET_DROP,c,0,0,0)&&errno!=EINVAL)die("cap bounding drop");
        d[c/32].effective&=~bit; d[c/32].permitted&=~bit; d[c/32].inheritable&=~bit;
    }
    if(syscall(SYS_capset,&h,d))die("capset");
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))die("no new privs");
}
int main(int argc,char **argv) {
    if(argc<3) { fprintf(stderr,"usage: enter ROOT COMMAND [ARGS...]\n"); return 2; }
    const char *owner=getenv("L410_OWNER_PID"),*stamp=getenv("L410_OWNER_START");
    if(owner||stamp){
        if(!owner||!stamp){fprintf(stderr,"incomplete owner identity\n");return 2;}
        owner_pid=(pid_t)number_setting("L410_OWNER_PID",0,2,2147483647);
        char *end;errno=0;owner_start=strtoull(stamp,&end,10);
        if(errno||!*stamp||*end||!owner_start||!owner_alive()){fprintf(stderr,"owner identity unavailable\n");return 2;}
    }
    int gpu=argc>3 && !strcmp(argv[2],"--gpu");
    /* Linux-local selection of the ORIGINAL AnCo hardware/IPC ABI. It does
     * not start an Android container or alter any host service/API. */
    int property_prepare=argc>3 && !strcmp(argv[2],"--anco-native-property-prepare");
    int native_prepare=property_prepare || (argc>3 && !strcmp(argv[2],"--anco-native-prepare"));
    int anco_native=(argc>3 && !strcmp(argv[2],"--anco-native")) || native_prepare;
#ifdef L410_GRALLOC_DIAGNOSTIC
    /* Separate diagnostic binary only; normal app launcher has no ION option. */
    int binder_diag=argc>3 && !strcmp(argv[2],"--gpu-ion-binder-probe");
    int ion=argc>3 && (!strcmp(argv[2],"--gpu-ion-probe") || binder_diag);
    gpu|=ion;
#endif
    int builder=argc>3 && !strcmp(argv[2],"--builder");
    int command_index=(gpu||builder||anco_native)?3:2;
    int prepare_index=0;
    if(native_prepare) {
        /* Linux-local init-before-app ordering. The original Bionic bootstrap
         * runs in private namespaces before application capability reduction.
         * No host API, property service or synthetic property is introduced. */
        int separator=3;
        while(separator<argc && strcmp(argv[separator],"--"))separator++;
        if(separator==3 || separator>=argc-1) {
            fprintf(stderr,"usage: enter ROOT %s PREPARE [ARGS...] -- COMMAND [ARGS...]\n",argv[2]);
            return 2;
        }
        prepare_index=3;
        argv[separator]=NULL;
        command_index=separator+1;
    }
    memory_mib=number_setting("L410_MEMORY_MIB",2048,512,4096);
    pids_limit=number_setting("L410_PIDS_LIMIT",512,64,1024);
    const char *configured_shared=getenv("L410_SHARED_MODE");
    if(configured_shared)shared_mode=configured_shared;
    else if(anco_native)shared_mode="off";
    if(strcmp(shared_mode,"readwrite")&&strcmp(shared_mode,"readonly")&&strcmp(shared_mode,"off")){fprintf(stderr,"invalid shared mode\n");return 2;}
    char *root=realpath(argv[1],NULL); if(!root)die("root realpath");
    if(strncmp(root,"/data/vendor/l410-linux/rootfs",30)!=0 ||
       (root[30] && root[30]!='-')) { fprintf(stderr,"unexpected root\n"); return 2; }
    if(unshare(CLONE_NEWNS|CLONE_NEWIPC|CLONE_NEWUTS|CLONE_NEWPID))die("unshare");
    if(mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL))die("private mounts");
    char memcg[128],pidcg[128];cgroup_paths(memcg,pidcg,sizeof(memcg));
    int gate[2];if(pipe2(gate,O_CLOEXEC))die("pipe");
    pid_t init=fork(); if(init<0)die("fork init");
    if(init) {
        init_pid=init;
        signal(SIGTERM,parent_forward); signal(SIGINT,parent_forward); signal(SIGHUP,parent_forward);
        close(gate[0]);
        char p[256],value[32];snprintf(value,sizeof(value),"%d",init);
        snprintf(p,sizeof(p),"%s/cgroup.procs",memcg);put(p,value);
        snprintf(p,sizeof(p),"%s/cgroup.procs",pidcg);put(p,value);
        if(write(gate[1],"1",1)!=1)die("gate write");
        close(gate[1]);
        int st;
        for(;;){
            pid_t got=waitpid(init,&st,owner_pid?WNOHANG:0);
            if(got==init)break;
            if(got<0 && errno!=EINTR)die("wait init");
            if(owner_pid && !owner_alive()){
                fprintf(stderr,"BROKER_OWNER_EXIT_CLEANUP\n");
                kill(init,SIGKILL);
                while(waitpid(init,&st,0)<0)if(errno!=EINTR)die("wait orphan init");
                break;
            }
            if(owner_pid)usleep(250000);
        }
        if(rmdir(pidcg))perror("remove pids cgroup");
        if(rmdir(memcg))perror("remove memory cgroup");
        return WIFEXITED(st)?WEXITSTATUS(st):128+WTERMSIG(st);
    }
    /* This parent is the persistent single-threaded enter supervisor. Gate EOF
     * also handles its death before the prctl is installed. */
    if(prctl(PR_SET_PDEATHSIG,SIGKILL,0,0,0))die("namespace parent death");
    close(gate[1]);char ready;if(read(gate[0],&ready,1)!=1)die("gate read");close(gate[0]);
    if(sethostname("l410-linux",10))die("hostname");
    /* Fixed compatibility binaries and GPU userspace are installed in the
     * verified vendor image.  Bind them at their established guest paths;
     * generated catalogs, titles and other state stay writable in the rootfs. */
    bind_reviewed_linux_runtime(root);
    tmp(root,"/dev","size=4m,mode=0755");
    const char *devices[]={"null","zero","full","random","urandom","tty"};
    for(size_t i=0;i<sizeof(devices)/sizeof(devices[0]);i++) {
        char src[80], dst[1024]; snprintf(src,sizeof(src),"/dev/%s",devices[i]);
        path(dst,sizeof(dst),root,src);
        int fd=open(dst,O_CREAT|O_RDWR,0600);if(fd<0)die(dst);close(fd);
        if(mount(src,dst,NULL,MS_BIND,NULL))die(dst);
    }
    if(anco_native) {
        /* Original config.zip: kernel_mm ashmem; communications binder and
         * hwbinder; graphic ion/mali0 (SourceMustExist:false). No input device
         * injection, framebuffer, TEE or unrelated hardware in this mode.
         * Existing host nodes retain their real major/minor/mode via bind. */
        static const char *native_devices[]={
            "/dev/ashmem","/dev/binder","/dev/hwbinder","/dev/ion","/dev/mali0"
        };
        for(size_t i=0;i<sizeof(native_devices)/sizeof(native_devices[0]);i++) {
            const char *src=native_devices[i];struct stat st;char dst[1024];
            if(stat(src,&st)) {
                if(i>=3 && errno==ENOENT)continue;
                die(src);
            }
            if(!S_ISCHR(st.st_mode)){errno=ENODEV;die(src);}
            path(dst,sizeof(dst),root,src);
            int fd=open(dst,O_CREAT|O_RDWR|O_CLOEXEC,0600);
            if(fd<0)die(dst);
            close(fd);
            if(mount(src,dst,NULL,MS_BIND,NULL))die(dst);
        }
        /* The L410 glibc Mali userspace opens the DRM render node before it
         * imports the original AnCo gralloc DMA-BUF as an EGLImage. Keep the
         * existing --anco-native ABI and expose only that render node. */
        {
            const char *src="/dev/dri/renderD128";struct stat st;
            char dir[1024],dst[1024];
            if(stat(src,&st)||!S_ISCHR(st.st_mode))die(src);
            path(dir,sizeof(dir),root,"/dev/dri");mk(dir,0755);
            path(dst,sizeof(dst),root,src);
            int fd=open(dst,O_CREAT|O_RDWR|O_CLOEXEC,0600);
            if(fd<0)die(dst);
            close(fd);
            if(mount(src,dst,NULL,MS_BIND,NULL))die(dst);
        }
        /* Original common HWC endpoint, under its original guest path view.
         * Share only the graphics broker directory, not the full host /data.
         * Socket communication does not require filesystem content writes. */
        const char *graphic_src="/data/virt_service/virt_comm/rgm_hmos/uds/virt_graphic_broker";
        struct stat graphic_st;
        if(!stat(graphic_src,&graphic_st) && S_ISDIR(graphic_st.st_mode)) {
            char graphic_dst[1024];
            path(graphic_dst,sizeof(graphic_dst),root,"/dev/cross");mk(graphic_dst,0755);
            path(graphic_dst,sizeof(graphic_dst),root,"/dev/cross/uds");mk(graphic_dst,0755);
            path(graphic_dst,sizeof(graphic_dst),root,"/dev/cross/uds/virt_graphic_broker");mk(graphic_dst,0755);
            if(mount(graphic_src,graphic_dst,NULL,MS_BIND,NULL))die("original graphic UDS bind");
            if(mount(NULL,graphic_dst,NULL,MS_BIND|MS_REMOUNT|MS_RDONLY,NULL))die("original graphic UDS readonly");
        }
    } else {
        char dir[1024],dst[1024];
        if(gpu) {
            path(dir,sizeof(dir),root,"/dev/dri");mk(dir,0755);
            path(dst,sizeof(dst),root,"/dev/dri/renderD128");
            int fd=open(dst,O_CREAT|O_RDWR,0600);if(fd<0)die("render target");close(fd);
            if(mount("/dev/dri/renderD128",dst,NULL,MS_BIND,NULL))die("render bind");
        }
        /* 设备直通照卓易通（anco）容器配置的清单来。它的 hostconfig.json 里逐条写着
         * graphic: /dev/mali0 /dev/hvgr0 /dev/graphics/dpu_res /dev/graphics/fb0..2
         *          /dev/ion /dev/aae_cmdlist /dev/aae_gfx /dev/aae_resource
         * multimodalinput: /dev/input /dev/uinput
         * 语义也照抄：宿主上没有的（SourceMustExist:false）就跳过，不让整次启动失败。
         * 注意：**无条件接**（不再只挂在 --gpu 上）。应用是由已经装好的那个系统服务拉起的，
         * 服务二进制换不掉，只有把这件事放进 enter 才能保证任何启动路径都拿到设备；
         * 输入设备不在容器里，真键盘就永远进不来（这正是之前"无法接受键盘输入"的原因）。 */
        static const char *anco_devices[]={
            "/dev/mali0","/dev/hvgr0","/dev/graphics/dpu_res",
            "/dev/graphics/fb0","/dev/graphics/fb1","/dev/graphics/fb2",
            "/dev/ion","/dev/aae_cmdlist","/dev/aae_gfx","/dev/aae_resource",
            "/dev/input","/dev/uinput"
        };
        for(size_t i=0;i<sizeof(anco_devices)/sizeof(anco_devices[0]);i++) {
            const char *src=anco_devices[i];
            struct stat st;
            if(stat(src,&st))continue;                 /* 宿主没有 → 跳过 */
            path(dst,sizeof(dst),root,src);
            if(S_ISDIR(st.st_mode)) {
                mk(dst,0755);
                if(mount(src,dst,NULL,MS_BIND,NULL))continue;
                continue;
            }
            char parent[1024];snprintf(parent,sizeof(parent),"%s",dst);
            char *slash=strrchr(parent,'/');
            if(slash){*slash='\0';mk(parent,0755);}
            int node=open(dst,O_CREAT|O_RDWR,0600);
            if(node<0)continue;
            close(node);
            if(mount(src,dst,NULL,MS_BIND,NULL))continue;
        }
#ifdef L410_GRALLOC_DIAGNOSTIC
        if(ion) {
            struct stat st;
            if(stat("/dev/ion",&st)||!S_ISCHR(st.st_mode))die("ion device");
            path(dst,sizeof(dst),root,"/dev/ion");
            int ion_target=open(dst,O_CREAT|O_RDWR,0600);
            if(ion_target<0)die("ion target");
            close(ion_target);
            if(mount("/dev/ion",dst,NULL,MS_BIND,NULL))die("ion bind");
        }
        if(binder_diag) {
            const char *binder_nodes[]={"/dev/binder","/dev/hwbinder","/dev/vndbinder"};
            for(size_t i=0;i<sizeof(binder_nodes)/sizeof(binder_nodes[0]);i++) {
                const char *src=binder_nodes[i];
                struct stat st;
                if(stat(src,&st)||!S_ISCHR(st.st_mode))die("binder device");
                path(dst,sizeof(dst),root,src);
                int binder_target=open(dst,O_CREAT|O_RDWR,0600);
                if(binder_target<0)die("binder target");
                close(binder_target);
                if(mount(src,dst,NULL,MS_BIND,NULL))die("binder bind");
            }
        }
#endif
    }
    char p[1024]; path(p,sizeof(p),root,"/dev/pts");mk(p,0755);
    if(property_prepare) {
        /* Original config maps this exact host socket to /dev/PropBridge.
         * Solely the finite privileged bootstrap receives it; ordinary apps
         * and the other native modes do not inherit this host write endpoint.
         * The caller owns checking the original unique guest connection. */
        const char *src="/dev/unix/socket/PropBridge";struct stat st;
        if(stat(src,&st))die("original PropBridge stat");
        if(!S_ISSOCK(st.st_mode)){errno=ENOTSOCK;die("original PropBridge type");}
        path(p,sizeof(p),root,"/dev/PropBridge");
        int fd=open(p,O_CREAT|O_RDWR|O_CLOEXEC,0600);
        if(fd<0)die("original PropBridge target");
        close(fd);
        if(mount(src,p,NULL,MS_BIND,NULL))die("original PropBridge bind");
        path(p,sizeof(p),root,"/dev/pts");
    }
    if(mount("devpts",p,"devpts",MS_NOSUID|MS_NOEXEC,"newinstance,ptmxmode=0666,mode=0620,gid=5"))die("devpts");
    path(p,sizeof(p),root,"/dev/ptmx");if(symlink("pts/ptmx",p))die("ptmx link");
    tmp(root,"/dev/shm","size=256m,mode=1777");
    tmp(root,"/run","size=32m,mode=0755");
    if(builder) {
        char dst[1024];
        path(dst,sizeof(dst),root,"/run/l410-builder");mk(dst,0700);
        if(mount("/vendor/l410-linux/entry-builder",dst,NULL,MS_BIND,NULL))die("immutable builder tools bind");
        if(mount(NULL,dst,NULL,MS_REMOUNT|MS_BIND|MS_RDONLY|MS_NOSUID|MS_NODEV,NULL))die("readonly builder tools");
        path(dst,sizeof(dst),root,"/run/l410-builder/private");
        if(mount("/data/vendor/l410-linux/entry-builder/private",dst,NULL,MS_BIND,NULL))die("private builder inputs bind");
        if(mount(NULL,dst,NULL,MS_REMOUNT|MS_BIND|MS_RDONLY|MS_NOSUID|MS_NODEV,NULL))die("readonly private builder inputs");
        path(dst,sizeof(dst),root,"/run/l410-build");mk(dst,0700);
        if(mount("/data/vendor/l410-linux/entry-build-work",dst,NULL,MS_BIND,NULL))die("builder work bind");
    }
    tmp(root,"/tmp","size=128m,mode=1777");
    path(p,sizeof(p),root,"/proc");mk(p,0555);
    if(mount("proc",p,"proc",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL))die("proc");
    if(anco_native) {
        /* Original isulad default_spec: uname sysname Linux + SystemContainer
         * selects systemcontainer_config.json: readonly sysfs, no selinuxfs
         * bind. Do not force the distinct IsOtherKernel/HM template here. */
        path(p,sizeof(p),root,"/sys");mk(p,0555);
        if(mount("sysfs",p,"sysfs",MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV,NULL))die("original Linux readonly sysfs");
        fprintf(stderr,"ORIGINAL_LINUX_TEMPLATE_READONLY_SYSFS_MOUNTED_NO_SELINUX_BIND\n");
    }
    path(p,sizeof(p),root,"/home/linux");mk(p,0755);
    path(p,sizeof(p),root,"/home/linux/Shared");mk(p,0755);
    if(!strcmp(shared_mode,"off")){
        if(mount("tmpfs",p,"tmpfs",MS_RDONLY|MS_NOSUID|MS_NODEV,"size=1m,mode=0555"))die("disabled shared folder");
    }else{
        /* Expose only the native File Manager document directory.  The old
         * source contains engine binaries and deployment artifacts. */
        if(mount("/data/service/el2/100/hmdfs/account/files/Docs/Huawei Share/Linux 兼容共享",p,NULL,MS_BIND,NULL))die("shared folder");
        if(!strcmp(shared_mode,"readonly")&&mount(NULL,p,NULL,MS_REMOUNT|MS_BIND|MS_RDONLY|MS_NOSUID|MS_NODEV,NULL))die("readonly shared folder");
    }
    if(chdir(root)||chroot(".")||chdir("/"))die("chroot");
    if(symlink("/proc/self/fd","/dev/fd") ||
       symlink("/proc/self/fd/0","/dev/stdin") ||
       symlink("/proc/self/fd/1","/dev/stdout") ||
       symlink("/proc/self/fd/2","/dev/stderr"))die("fd symlinks");
    clearenv();
    setenv("PATH","/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",1);
    setenv("HOME","/root",1);setenv("TERM","xterm-256color",1);
    setenv("LANG","C.UTF-8",1);setenv("container","l410-linux",1);
    setenv("DEBIAN_FRONTEND","noninteractive",1);
    signal(SIGTERM,forward); signal(SIGINT,forward); signal(SIGHUP,forward);
    if(prepare_index) {
        command_pid=fork();if(command_pid<0)die("fork native preparation");
        if(!command_pid) {
            setpgid(0,0);
            execvp(argv[prepare_index],argv+prepare_index);
            die("exec native preparation");
        }
        int preparation_status;pid_t prepared;
        do { prepared=waitpid(command_pid,&preparation_status,0); }
        while(prepared<0 && errno==EINTR);
        if(prepared<0)die("wait native preparation");
        command_pid=0;
        if(!WIFEXITED(preparation_status) || WEXITSTATUS(preparation_status))
            return WIFEXITED(preparation_status)?WEXITSTATUS(preparation_status):128+WTERMSIG(preparation_status);
        /* Preparation is one-shot, not a privileged daemon. Reject leftovers
         * before starting the application, which still drops SYS_ADMIN. */
        int remaining;pid_t child=waitpid(-1,&remaining,WNOHANG);
        if(child!= -1 || errno!=ECHILD) {
            fprintf(stderr,"native preparation left child processes\n");
            return 1;
        }
        if(property_prepare) {
            if(umount("/dev/PropBridge"))die("original PropBridge unmount");
            if(unlink("/dev/PropBridge"))die("original PropBridge target removal");
        }
    }
    reduce_caps(argc==4 && !strcmp(argv[2],"/debootstrap/debootstrap") && !strcmp(argv[3],"--second-stage"));
    /* 容器内输入服务：输入以 /dev/input 设备事件进入容器（照卓易通的形态）。
       放在应用之前起；任何启动路径（系统服务、脚本、手动）都因此拿到输入，
       不再依赖某个特定的服务二进制或启动脚本。 */
    if(!anco_native) {
        pid_t input_pid=fork();
        if(input_pid==0) {
            setpgid(0,0);
            int null_fd=open("/dev/null",O_RDWR);
            if(null_fd>=0){dup2(null_fd,0);if(null_fd>2)close(null_fd);}
            int log_fd=open("/tmp/l410-inputd.log",O_WRONLY|O_CREAT|O_APPEND,0644);
            if(log_fd>=0){dup2(log_fd,1);dup2(log_fd,2);if(log_fd>2)close(log_fd);}
            execl("/home/linux/Shared/l410-engine/l410-inputd","l410-inputd",(char *)NULL);
            _exit(127);
        }
        if(input_pid>0) {
            /* 给输入服务一点时间把 socket 建起来，应用侧后端启动时才能连上 */
            struct timespec pause={0,250*1000*1000};
            nanosleep(&pause,NULL);
        }
    }
    command_pid=fork();if(command_pid<0)die("fork command");
    if(!command_pid) { setpgid(0,0);execvp(argv[command_index],argv+command_index);die("exec"); }
    int status=1,st; pid_t got;
    while((got=waitpid(-1,&st,0))>0 || errno==EINTR) {
        if(got==command_pid) { status=WIFEXITED(st)?WEXITSTATUS(st):128+WTERMSIG(st);break; }
    }
    /* Exiting PID 1 kills any remaining processes in this private PID namespace. */
    return status;
}
