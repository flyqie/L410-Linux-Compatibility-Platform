#ifndef L410_WAYLAND_EGL_BACKEND_H
#define L410_WAYLAND_EGL_BACKEND_H
struct wl_shm_buffer;
int l410_egl_compose_shm(struct wl_shm_buffer *shm,int x,int y,int source_width,int source_height,int fd,int width,int height,int stride,int size,int format);
int l410_egl_compose_pixels(const void *pixels,int pixel_stride,
    int source_width,int source_height,int target_fd,int width,int height,
    int target_stride,int size,int format);
struct wl_display;struct wl_resource;
int l410_egl_open(struct wl_display *server);
void *l410_egl_import(struct wl_resource *buffer,int *width,int *height);
int l410_egl_compose(void *source,int source_width,int source_height,int target_fd,int width,int height,int stride,int size,int format,int invert_y);
void l410_egl_release(void *image);
void l410_egl_close(struct wl_display *server);
#endif
