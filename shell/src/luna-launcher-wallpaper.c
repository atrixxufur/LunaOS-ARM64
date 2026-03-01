/*
 * luna-launcher.c — LunaOS app launcher (arm64)
 * luna-wallpaper.c — LunaOS wallpaper (arm64)
 *
 * Both respect display_scale for Retina rendering.
 * Launcher uses GCD blocks for app launching on arm64.
 */

/* ─────────────────────────────────────────────────────────────────────────────
 * LAUNCHER
 * ───────────────────────────────────────────────────────────────────────────*/
#include "luna-shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>

static const struct luna_app BUILTIN_APPS[] = {
    {"Terminal",    "foot",         "terminal",    "System",  0xFF2D8CFF},
    {"Files",       "luna-files",   "file-manager","Files",   0xFF4CAF50},
    {"Text Editor", "luna-editor",  "text-editor", "Utility", 0xFFFF9800},
    {"Settings",    "luna-settings","preferences", "System",  0xFF607D8B},
};
#define N_BUILTIN ((int)(sizeof(BUILTIN_APPS)/sizeof(BUILTIN_APPS[0])))

struct luna_launcher {
    struct luna_shell *shell;
    struct wl_surface            *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_shm_pool *shm_pool;
    struct wl_buffer   *buffer;
    uint32_t *pixels;
    int stride, width, height, scale;
    size_t size; int shm_fd;
    bool configured, visible;
    int cols, cell_w, cell_h, icon_size;
    int hover_idx;
};

static void lfill(uint32_t *px,int s,int x,int y,int w,int h,uint32_t c){
    for(int r=y;r<y+h;r++) for(int col=x;col<x+w;col++) px[r*(s/4)+col]=c;
}
static void ltext(uint32_t *px,int s,int x,int y,int fw,
                  const char *t,uint32_t c,int fs){
    int len=(int)strlen(t);
    for(int i=0;i<len&&i<30;i++)
        lfill(px,s,x+i*(fs/2),y+fs/4,fs/2-1,fs/2,c);
    (void)fw;
}

static void launcher_render(struct luna_launcher *l){
    if(!l->configured||!l->pixels) return;
    int w=l->width*l->scale, h=l->height*l->scale, sc=l->scale;
    lfill(l->pixels,l->stride,0,0,w,h,0xE6050510);
    int fsize=14*sc;
    ltext(l->pixels,l->stride,(w-12*(fsize/2))/2,20*sc,w,"Applications",0xFFEEEEEE,fsize);
    int gy=72*sc, gx=(w-l->cols*l->cell_w*sc)/2;
    int total=l->shell->app_count+N_BUILTIN;
    for(int i=0;i<total;i++){
        const struct luna_app *app=i<N_BUILTIN?&BUILTIN_APPS[i]:&l->shell->apps[i-N_BUILTIN];
        int col=i%l->cols, row=i/l->cols;
        int cx=gx+col*l->cell_w*sc, cy=gy+row*l->cell_h*sc;
        if(cy+l->cell_h*sc>h) break;
        bool hov=(l->hover_idx==i);
        uint32_t bg=app->accent_color;
        if(hov){ uint8_t r=((bg>>16)&0xFF)+30,g=((bg>>8)&0xFF)+30,b=(bg&0xFF)+30;
            bg=0xFF000000|((uint32_t)r<<16)|((uint32_t)g<<8)|b; }
        int is=l->icon_size*sc;
        lfill(l->pixels,l->stride,cx+(l->cell_w*sc-is)/2,cy,is,is,bg);
        char ini[2]={app->name[0],0};
        ltext(l->pixels,l->stride,cx+(l->cell_w*sc-20*sc)/2,cy+(is-fsize)/2,
              20*sc,ini,0xFFFFFFFF,fsize*2);
        ltext(l->pixels,l->stride,cx,cy+is+4*sc,l->cell_w*sc,
              app->name,0xFFDDDDDD,11*sc);
    }
    ltext(l->pixels,l->stride,(w-26*(11*sc/2))/2,h-20*sc,w,
          "Super or Esc to close",0x55EEEEEE,11*sc);
    wl_surface_attach(l->surface,l->buffer,0,0);
    wl_surface_damage_buffer(l->surface,0,0,w,h);
    wl_surface_set_buffer_scale(l->surface,l->scale);
    wl_surface_commit(l->surface);
}

static void ll_configure(void*data,struct zwlr_layer_surface_v1*ls,
                          uint32_t serial,uint32_t w,uint32_t h){
    struct luna_launcher*l=data;
    zwlr_layer_surface_v1_ack_configure(ls,serial);
    l->width=w; l->height=h; l->configured=true;
    l->scale=l->shell->config.display_scale;
    if(l->scale<1) l->scale=1;
    l->icon_size=l->shell->config.launcher_icon_size;
    l->cell_w=l->icon_size+24; l->cell_h=l->icon_size+36;
    l->cols=(w-64)/l->cell_w; if(l->cols<1) l->cols=1;
    if(l->pixels){munmap(l->pixels,l->size);wl_buffer_destroy(l->buffer);
        wl_shm_pool_destroy(l->shm_pool);close(l->shm_fd);}
    int pw=w*l->scale,ph=h*l->scale;
    l->stride=pw*4; l->size=(size_t)(l->stride*ph);
    char name[64]; snprintf(name,64,"/luna-launcher-arm64-%d",getpid());
    l->shm_fd=shm_open(name,O_RDWR|O_CREAT|O_EXCL,0600);
    shm_unlink(name); ftruncate(l->shm_fd,(off_t)l->size);
    l->pixels=mmap(NULL,l->size,PROT_READ|PROT_WRITE,MAP_SHARED,l->shm_fd,0);
    l->shm_pool=wl_shm_create_pool(l->shell->shm,l->shm_fd,(int32_t)l->size);
    l->buffer=wl_shm_pool_create_buffer(l->shm_pool,0,pw,ph,l->stride,WL_SHM_FORMAT_ARGB8888);
    if(l->visible) launcher_render(l);
}
static void ll_closed(void*d,struct zwlr_layer_surface_v1*ls){(void)d;(void)ls;}
static const struct zwlr_layer_surface_v1_listener ll_listener={
    .configure=ll_configure,.closed=ll_closed};

void luna_app_launch(struct luna_shell *shell, const struct luna_app *app){
    (void)shell;
    pid_t pid=fork(); if(pid==0){
        setenv("WAYLAND_DISPLAY",getenv("WAYLAND_DISPLAY")?:"wayland-0",1);
        setenv("XDG_RUNTIME_DIR",getenv("XDG_RUNTIME_DIR")?:"/run/user/501",1);
        execl("/bin/sh","sh","-c",app->exec,NULL); _exit(1);
    }
}

int luna_apps_load(struct luna_shell *shell, const char *dir){
    DIR*d=opendir(dir); if(!d) return 0;
    struct luna_app*apps=calloc(16,sizeof(*apps)); int count=0,cap=16;
    struct dirent*ent;
    while((ent=readdir(d))){
        if(!strstr(ent->d_name,".desktop")) continue;
        if(count>=cap){cap*=2;apps=realloc(apps,cap*sizeof(*apps));}
        struct luna_app*a=&apps[count]; memset(a,0,sizeof(*a));
        a->accent_color=0xFF607D8B;
        char path[512]; snprintf(path,512,"%s/%s",dir,ent->d_name);
        FILE*f=fopen(path,"r"); if(!f) continue;
        char line[512]; bool in=false;
        while(fgets(line,512,f)){
            line[strcspn(line,"\n")]=0;
            if(!strcmp(line,"[Desktop Entry]")){in=true;continue;}
            if(line[0]=='['){in=false;continue;} if(!in) continue;
            if(!strncmp(line,"Name=",5))   strncpy(a->name,line+5,63);
            if(!strncmp(line,"Exec=",5))   strncpy(a->exec,line+5,255);
            if(!strncmp(line,"Icon=",5))   strncpy(a->icon_name,line+5,63);
        }
        fclose(f);
        char*pct=strchr(a->exec,'%'); if(pct&&pct>a->exec) *(pct-1)=0;
        if(a->name[0]&&a->exec[0]) count++;
    }
    closedir(d); shell->apps=apps; shell->app_count=count; return count;
}

void luna_launcher_show(struct luna_launcher*l){
    if(l->visible) return; l->visible=true; l->shell->launcher_visible=true;
    wl_surface_commit(l->surface); launcher_render(l);
}
void luna_launcher_hide(struct luna_launcher*l){
    if(!l->visible) return; l->visible=false; l->shell->launcher_visible=false;
    wl_surface_attach(l->surface,NULL,0,0); wl_surface_commit(l->surface);
}

struct luna_launcher *luna_launcher_create(struct luna_shell *shell){
    struct luna_launcher*l=calloc(1,sizeof(*l));
    l->shell=shell; l->shm_fd=-1; l->hover_idx=-1;
    l->surface=wl_compositor_create_surface(shell->compositor);
    l->layer_surface=zwlr_layer_shell_v1_get_layer_surface(
        shell->layer_shell,l->surface,NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,"luna-launcher");
    zwlr_layer_surface_v1_set_anchor(l->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM|
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(l->layer_surface,-1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(l->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
    zwlr_layer_surface_v1_add_listener(l->layer_surface,&ll_listener,l);
    return l;
}
void luna_launcher_destroy(struct luna_launcher*l){
    if(l->pixels) munmap(l->pixels,l->size);
    if(l->buffer) wl_buffer_destroy(l->buffer);
    if(l->shm_pool) wl_shm_pool_destroy(l->shm_pool);
    if(l->shm_fd>=0) close(l->shm_fd);
    zwlr_layer_surface_v1_destroy(l->layer_surface);
    wl_surface_destroy(l->surface); free(l);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * WALLPAPER
 * ───────────────────────────────────────────────────────────────────────────*/

struct luna_wallpaper {
    struct luna_shell  *shell;
    struct luna_output *output;
    struct wl_surface            *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_shm_pool *shm_pool;
    struct wl_buffer   *buffer;
    uint32_t *pixels;
    int stride, width, height, scale;
    size_t size; int shm_fd;
    bool configured;
};

static void wp_render_gradient(struct luna_wallpaper*wp, uint32_t ca, uint32_t cb){
    int pw=wp->width*wp->scale, ph=wp->height*wp->scale;
    uint8_t r0=(ca>>16)&0xFF,g0=(ca>>8)&0xFF,b0=ca&0xFF;
    uint8_t r1=(cb>>16)&0xFF,g1=(cb>>8)&0xFF,b1=cb&0xFF;
    for(int y=0;y<ph;y++){
        float t=(float)y/(float)(ph-1);
        uint32_t c=0xFF000000|
            ((uint32_t)(uint8_t)(r0+t*(r1-r0))<<16)|
            ((uint32_t)(uint8_t)(g0+t*(g1-g0))<<8)|
            (uint8_t)(b0+t*(b1-b0));
        for(int x=0;x<pw;x++) wp->pixels[y*(wp->stride/4)+x]=c;
    }
}

static void wp_configure(void*data,struct zwlr_layer_surface_v1*ls,
                          uint32_t serial,uint32_t w,uint32_t h){
    struct luna_wallpaper*wp=data;
    zwlr_layer_surface_v1_ack_configure(ls,serial);
    wp->width=w; wp->height=h;
    wp->scale=wp->output->scale>0?wp->output->scale:1;
    int pw=w*wp->scale, ph=h*wp->scale;
    if(wp->pixels){munmap(wp->pixels,wp->size);wl_buffer_destroy(wp->buffer);
        wl_shm_pool_destroy(wp->shm_pool);close(wp->shm_fd);}
    wp->stride=pw*4; wp->size=(size_t)(wp->stride*ph);
    char name[64]; snprintf(name,64,"/luna-wp-arm64-%d-%s",getpid(),wp->output->name);
    wp->shm_fd=shm_open(name,O_RDWR|O_CREAT|O_EXCL,0600);
    shm_unlink(name); ftruncate(wp->shm_fd,(off_t)wp->size);
    wp->pixels=mmap(NULL,wp->size,PROT_READ|PROT_WRITE,MAP_SHARED,wp->shm_fd,0);
    wp->shm_pool=wl_shm_create_pool(wp->shell->shm,wp->shm_fd,(int32_t)wp->size);
    wp->buffer=wl_shm_pool_create_buffer(wp->shm_pool,0,pw,ph,wp->stride,WL_SHM_FORMAT_ARGB8888);
    wp_render_gradient(wp,wp->shell->config.wallpaper_color_a,
                          wp->shell->config.wallpaper_color_b);
    wl_surface_attach(wp->surface,wp->buffer,0,0);
    wl_surface_damage_buffer(wp->surface,0,0,pw,ph);
    wl_surface_set_buffer_scale(wp->surface,wp->scale);
    wl_surface_commit(wp->surface);
    wp->configured=true;
}
static void wp_closed(void*d,struct zwlr_layer_surface_v1*ls){(void)d;(void)ls;}
static const struct zwlr_layer_surface_v1_listener wp_listener={
    .configure=wp_configure,.closed=wp_closed};

struct luna_wallpaper *luna_wallpaper_create(struct luna_shell *shell,
                                              struct luna_output *output){
    struct luna_wallpaper*wp=calloc(1,sizeof(*wp));
    wp->shell=shell; wp->output=output; wp->shm_fd=-1;
    wp->surface=wl_compositor_create_surface(shell->compositor);
    wp->layer_surface=zwlr_layer_shell_v1_get_layer_surface(
        shell->layer_shell,wp->surface,output->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,"luna-wallpaper");
    zwlr_layer_surface_v1_set_anchor(wp->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM|
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(wp->layer_surface,-1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(wp->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(wp->layer_surface,&wp_listener,wp);
    wl_surface_commit(wp->surface);
    return wp;
}
void luna_wallpaper_destroy(struct luna_wallpaper*wp){
    if(wp->pixels) munmap(wp->pixels,wp->size);
    if(wp->buffer) wl_buffer_destroy(wp->buffer);
    if(wp->shm_pool) wl_shm_pool_destroy(wp->shm_pool);
    if(wp->shm_fd>=0) close(wp->shm_fd);
    zwlr_layer_surface_v1_destroy(wp->layer_surface);
    wl_surface_destroy(wp->surface); free(wp);
}
