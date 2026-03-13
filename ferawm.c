/*
 * ferawm 0.2.0 — The FERAWM Window Manager
 * Every app gets a BONE. The app fills the BONE.
 * Config uses BOXES — organized, readable, expandable.
 *
 * Build:   make
 * Install: sudo make install
 * Config:  ~/.config/fera/ferawm.conf
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>

#define GRID_COLS     3
#define GRID_ROWS     3
#define GRID_MAX      (GRID_COLS * GRID_ROWS)

#define FERAWM_VERSION "0.2.2"

static int verbose = 0;
#define LOG(...)        do { if (verbose) printf(__VA_ARGS__); } while(0)
#define LOG_ALWAYS(...) printf(__VA_ARGS__)

#define ANIM_CALCULATE  0
#define ANIM_LIVE       1
#define ANIM_GHOST      2
#define ANIM_PIXEL      3

typedef enum {
    ACTION_NONE, ACTION_KILL_WINDOW, ACTION_KILL_WM,
    ACTION_FULLSCREEN, ACTION_CYCLE_FOCUS,
    ACTION_SPAWN, ACTION_RELOAD, ACTION_RESTART,
    ACTION_WS_SWITCH,
    ACTION_WS_MOVE,
    ACTION_TILE_TOGGLE,
    ACTION_TILE_FLOAT,
    ACTION_TILE_MOVE_LEFT,
    ACTION_TILE_MOVE_RIGHT,
    ACTION_TILE_MOVE_UP,
    ACTION_TILE_MOVE_DOWN,
    ACTION_FOCUS_LEFT,
    ACTION_FOCUS_RIGHT,
    ACTION_FOCUS_UP,
    ACTION_FOCUS_DOWN,
} Action;

#define MAX_KEYBINDS 64
typedef struct {
    unsigned int mod;
    KeySym       sym;
    Action       action;
    char         spawn_cmd[256];
    int          ws_num;
} Keybind;

#define MAX_VARS 32
typedef struct { char name[32]; char value[128]; } CfgVar;

typedef struct {
    unsigned long bone_color, bone_unfocus;
    int           bone_gap, animate_tab, border_x, border_y;
    int           inner_gap, tile_mode, mouse_focus;
    unsigned int  modifier;
    char          xkb_layout[64], xkb_options[128];
    float         mouse_sensitivity, touchpad_sensitivity;
    int           mouse_natural_scroll, touchpad_natural_scroll;
    char          mouse_accel[32], touchpad_accel[32];
    Keybind       keybinds[MAX_KEYBINDS];
    int           keybind_count;
    char          autostart_once[32][256];
    int           autostart_once_count;
    char          autostart_restart[32][256];
    int           autostart_restart_count;
    CfgVar        vars[MAX_VARS];
    int           var_count;
} Config;

static Config cfg = {
    .bone_color              = 0x7c6af7,
    .bone_unfocus            = 0x2a2a3a,
    .bone_gap                = 20,
    .inner_gap               = 10,
    .tile_mode               = 0,
    .mouse_focus      = 0,
    .animate_tab             = ANIM_LIVE,
    .border_x                = 0,
    .border_y                = 0,
    .modifier                = Mod4Mask,
    .xkb_layout              = "us",
    .xkb_options             = "",
    .mouse_sensitivity       = 0.0f,
    .mouse_natural_scroll    = 0,
    .mouse_accel             = "flat",
    .touchpad_sensitivity    = 0.5f,
    .touchpad_natural_scroll = 1,
    .touchpad_accel          = "flat",
    .keybind_count           = 0,
    .autostart_once_count    = 0,
    .autostart_restart_count = 0,
    .var_count               = 0,
};

static char **g_argv;

#define MAX_BONES 64
typedef struct {
    Window bone, app;
    int x, y, w, h, focused, fullscreen;
    int pre_fs_x, pre_fs_y, pre_fs_w, pre_fs_h;
    int workspace;
    int grid_cell;   /* -1 = floating */
} Bone;

#define NUM_WORKSPACES 4
#define PARK_X (-32000)
#define PARK_Y (-32000)

static Display *dpy;
static Window   root;
static int      screen;
static Bone     bones[MAX_BONES];
static int      bone_count = 0, mod_held = 0, ghost_active = 0;
static int      current_ws = 0;
static int      dragging = 0, resizing = 0;
static Bone    *drag_bone = NULL;
static int      drag_sx, drag_sy, drag_bx, drag_by, drag_bw, drag_bh;
static int      inotify_fd = -1;

/* ══════════════════════════════════════════════════════════════════════════
 * CONFIG — BOX PARSER + VARIABLE SYSTEM
 * ══════════════════════════════════════════════════════════════════════════ */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

static const char *var_get(const char *name) {
    for (int i = 0; i < cfg.var_count; i++)
        if (strcmp(cfg.vars[i].name, name) == 0) return cfg.vars[i].value;
    return NULL;
}

static void var_set(const char *name, const char *val) {
    for (int i = 0; i < cfg.var_count; i++) {
        if (strcmp(cfg.vars[i].name, name) == 0) {
            strncpy(cfg.vars[i].value, val, sizeof(cfg.vars[0].value)-1); return;
        }
    }
    if (cfg.var_count >= MAX_VARS) return;
    strncpy(cfg.vars[cfg.var_count].name,  name, sizeof(cfg.vars[0].name)-1);
    strncpy(cfg.vars[cfg.var_count].value, val,  sizeof(cfg.vars[0].value)-1);
    cfg.var_count++;
}

static void expand_vars(const char *in, char *out, size_t outsz) {
    size_t oi = 0, i = 0, len = strlen(in);
    while (i < len && oi < outsz - 1) {
        if (in[i] == '$') {
            char vname[32] = {0}; size_t vi = 0; i++;
            while (i < len && (isalnum((unsigned char)in[i]) || in[i]=='_') && vi < sizeof(vname)-1)
                vname[vi++] = in[i++];
            const char *v = var_get(vname);
            if (v) {
                size_t vl = strlen(v);
                if (oi + vl < outsz - 1) { memcpy(out+oi, v, vl); oi += vl; }
            } else {
                out[oi++] = '$';
                size_t nl = strlen(vname);
                if (oi + nl < outsz - 1) { memcpy(out+oi, vname, nl); oi += nl; }
            }
        } else { out[oi++] = in[i++]; }
    }
    out[oi] = '\0';
}

static unsigned int parse_modifier_name(const char *s) {
    if (strcmp(s,"alt")   == 0) return Mod1Mask;
    if (strcmp(s,"super") == 0) return Mod4Mask;
    if (strcmp(s,"ctrl")  == 0) return ControlMask;
    if (strcmp(s,"shift") == 0) return ShiftMask;
    fprintf(stderr,"[FERAWM] Unknown modifier '%s', defaulting to super\n",s);
    return Mod4Mask;
}

static KeySym parse_key(const char *s) {
    if (strlen(s)==1 && isalpha((unsigned char)s[0])) {
        char b[2]={(char)tolower((unsigned char)s[0]),0}; return XStringToKeysym(b);
    }
    if (strcmp(s,"tab")    ==0) return XK_Tab;
    if (strcmp(s,"space")  ==0) return XK_space;
    if (strcmp(s,"return") ==0) return XK_Return;
    if (strcmp(s,"escape") ==0) return XK_Escape;
    if (strcmp(s,"left")   ==0) return XK_Left;
    if (strcmp(s,"right")  ==0) return XK_Right;
    if (strcmp(s,"up")     ==0) return XK_Up;
    if (strcmp(s,"down")   ==0) return XK_Down;
    return XStringToKeysym(s);
}

static Action parse_action(const char *s) {
    if (strcmp(s,"kill_window")==0) return ACTION_KILL_WINDOW;
    if (strcmp(s,"kill_wm")   ==0) return ACTION_KILL_WM;
    if (strcmp(s,"fullscreen") ==0) return ACTION_FULLSCREEN;
    if (strcmp(s,"cycle_focus")==0) return ACTION_CYCLE_FOCUS;
    if (strncmp(s,"spawn",5)  ==0) return ACTION_SPAWN;
    if (strcmp(s,"reload")    ==0) return ACTION_RELOAD;
    if (strcmp(s,"restart")   ==0) return ACTION_RESTART;
    if (strncmp(s,"ws_switch",9)==0) return ACTION_WS_SWITCH;
    if (strncmp(s,"ws_move",7)  ==0) return ACTION_WS_MOVE;
    if (strcmp(s,"tile_toggle") ==0) return ACTION_TILE_TOGGLE;
    if (strcmp(s,"tile_float")  ==0) return ACTION_TILE_FLOAT;
    if (strcmp(s,"tile_left")   ==0) return ACTION_TILE_MOVE_LEFT;
    if (strcmp(s,"tile_right")  ==0) return ACTION_TILE_MOVE_RIGHT;
    if (strcmp(s,"tile_up")     ==0) return ACTION_TILE_MOVE_UP;
    if (strcmp(s,"tile_down")   ==0) return ACTION_TILE_MOVE_DOWN;
    if (strcmp(s,"focus_left")  ==0) return ACTION_FOCUS_LEFT;
    if (strcmp(s,"focus_right") ==0) return ACTION_FOCUS_RIGHT;
    if (strcmp(s,"focus_up")    ==0) return ACTION_FOCUS_UP;
    if (strcmp(s,"focus_down")  ==0) return ACTION_FOCUS_DOWN;
    return ACTION_NONE;
}

static void parse_keybind(char *val) {
    if (cfg.keybind_count >= MAX_KEYBINDS) return;
    char expanded[512]; expand_vars(val, expanded, sizeof(expanded));
    char *eq = strchr(expanded,'='); if (!eq) return;
    *eq = '\0';
    char *combo = trim(expanded), *action_str = trim(eq+1), *spawn_cmd = NULL;
    char *eq2 = strchr(action_str,'=');
    if (eq2) { *eq2='\0'; spawn_cmd=trim(eq2+1); action_str=trim(action_str); }
    unsigned int mod = 0; KeySym sym = NoSymbol;
    char buf[256]; strncpy(buf,combo,sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
    char *tok = strtok(buf,"+");
    while (tok) {
        tok = trim(tok);
        if      (strcmp(tok,"modifier")==0) mod |= cfg.modifier;
        else if (strcmp(tok,"shift")   ==0) mod |= ShiftMask;
        else if (strcmp(tok,"ctrl")    ==0) mod |= ControlMask;
        else if (strcmp(tok,"alt")     ==0) mod |= Mod1Mask;
        else if (strcmp(tok,"super")   ==0) mod |= Mod4Mask;
        else sym = parse_key(tok);
        tok = strtok(NULL,"+");
    }
    if (sym == NoSymbol) return;
    Action act = parse_action(action_str);
    cfg.keybinds[cfg.keybind_count].mod    = mod;
    cfg.keybinds[cfg.keybind_count].sym    = sym;
    cfg.keybinds[cfg.keybind_count].action = act;
    cfg.keybinds[cfg.keybind_count].ws_num = -1;
    cfg.keybinds[cfg.keybind_count].spawn_cmd[0] = '\0';
    if (act==ACTION_SPAWN && spawn_cmd)
        strncpy(cfg.keybinds[cfg.keybind_count].spawn_cmd, spawn_cmd,
                sizeof(cfg.keybinds[0].spawn_cmd)-1);
    if ((act==ACTION_WS_SWITCH || act==ACTION_WS_MOVE) && spawn_cmd)
        cfg.keybinds[cfg.keybind_count].ws_num = atoi(spawn_cmd) - 1;
    cfg.keybind_count++;
}

static void parse_csv(const char *val, char arr[][256], int *count, int max) {
    *count = 0;
    char buf[1024]; strncpy(buf,val,sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
    char *tok = strtok(buf,",");
    while (tok && *count < max) {
        char *t = trim(tok);
        if (t[0]) { strncpy(arr[*count],t,255); arr[*count][255]='\0'; (*count)++; }
        tok = strtok(NULL,",");
    }
}

/* Box IDs */
#define BOX_NONE        0
#define BOX_GENERAL     1
#define BOX_INPUT       2
#define BOX_INPUT_MOUSE 3
#define BOX_INPUT_TOUCH 4
#define BOX_AUTOSTART   5

static void kv_in_box(int box, char *key, char *val) {
    switch (box) {
    case BOX_GENERAL:
        if      (!strcmp(key,"bone_color"))   cfg.bone_color   = strtoul(val,NULL,16);
        else if (!strcmp(key,"bone_unfocus")) cfg.bone_unfocus = strtoul(val,NULL,16);
        else if (!strcmp(key,"bone_gap"))     cfg.bone_gap     = atoi(val);
        else if (!strcmp(key,"inner_gap"))    cfg.inner_gap    = atoi(val);
        else if (!strcmp(key,"tile_mode"))    cfg.tile_mode    = atoi(val);
        else if (!strcmp(key,"mouse_focus")) cfg.mouse_focus = (!strcmp(val,"true")||!strcmp(val,"1"));
        else if (!strcmp(key,"animate_tab"))  cfg.animate_tab  = atoi(val);
        else if (!strcmp(key,"border_size"))  sscanf(val,"%d %d",&cfg.border_x,&cfg.border_y);
        break;
    case BOX_INPUT:
        if      (!strcmp(key,"modifier"))    cfg.modifier = parse_modifier_name(val);
        else if (!strcmp(key,"xkb_layout"))  strncpy(cfg.xkb_layout, val, sizeof(cfg.xkb_layout)-1);
        else if (!strcmp(key,"xkb_options")) strncpy(cfg.xkb_options,val, sizeof(cfg.xkb_options)-1);
        break;
    case BOX_INPUT_MOUSE:
        if      (!strcmp(key,"sensitivity"))    cfg.mouse_sensitivity    = atof(val);
        else if (!strcmp(key,"natural_scroll")) cfg.mouse_natural_scroll = (!strcmp(val,"true")||!strcmp(val,"1"));
        else if (!strcmp(key,"accel_profile"))  strncpy(cfg.mouse_accel,val,sizeof(cfg.mouse_accel)-1);
        break;
    case BOX_INPUT_TOUCH:
        if      (!strcmp(key,"sensitivity"))    cfg.touchpad_sensitivity    = atof(val);
        else if (!strcmp(key,"natural_scroll")) cfg.touchpad_natural_scroll = (!strcmp(val,"true")||!strcmp(val,"1"));
        else if (!strcmp(key,"accel_profile"))  strncpy(cfg.touchpad_accel,val,sizeof(cfg.touchpad_accel)-1);
        break;
    case BOX_AUTOSTART:
        if      (!strcmp(key,"once"))    parse_csv(val,cfg.autostart_once,   &cfg.autostart_once_count,   32);
        else if (!strcmp(key,"restart")) parse_csv(val,cfg.autostart_restart,&cfg.autostart_restart_count,32);
        break;
    default: break;
    }
}

static void load_config(void) {
    char path[512];
    const char *home = getenv("HOME"); if (!home) home="/root";
    snprintf(path,sizeof(path),"%s/.config/fera/ferawm.conf",home);
    FILE *f = fopen(path,"r");
    if (!f) f = fopen("/etc/fera/ferawm.conf","r");
    if (!f) { LOG("[FERAWM] No config, using defaults\n"); return; }

    char line[1024];
    int box=BOX_NONE, stack_top=0;
    int box_stack[8]={0};

    /* pass 1: vars + settings (skip keybinds) */
    while (fgets(line,sizeof(line),f)) {
        char *hash=strchr(line,'#'); if(hash)*hash='\0';
        char *t=trim(line); if(!t[0]) continue;

        /* $var = value */
        if (t[0]=='$') {
            char *eq=strchr(t,'='); if(!eq) continue;
            *eq='\0'; var_set(trim(t+1),trim(eq+1)); continue;
        }
        /* open box */
        if (strchr(t,'{')) {
            char bname[64]={0}; char *brace=strchr(t,'{');
            size_t nl=brace-t; if(nl>0&&nl<sizeof(bname)){strncpy(bname,t,nl);trim(bname);}
            if(stack_top<7) box_stack[stack_top++]=box;
            if      (!strcmp(bname,"general"))  box=BOX_GENERAL;
            else if (!strcmp(bname,"input"))    box=BOX_INPUT;
            else if (!strcmp(bname,"mouse"))    box=BOX_INPUT_MOUSE;
            else if (!strcmp(bname,"touchpad")) box=BOX_INPUT_TOUCH;
            else if (!strcmp(bname,"autostart"))box=BOX_AUTOSTART;
            continue;
        }
        /* close box */
        if (strchr(t,'}')) { box=(stack_top>0)?box_stack[--stack_top]:BOX_NONE; continue; }

        char *eq=strchr(t,'='); if(!eq) continue;
        *eq='\0';
        char key[128],val[512];
        strncpy(key,trim(t),    sizeof(key)-1); key[sizeof(key)-1]='\0';
        strncpy(val,trim(eq+1), sizeof(val)-1); val[sizeof(val)-1]='\0';
        if (!strcmp(key,"keybind")) continue; /* pass 2 */
        kv_in_box(box,key,val);
    }

    /* pass 2: keybinds (modifier now known) */
    rewind(f); box=BOX_NONE; stack_top=0;
    while (fgets(line,sizeof(line),f)) {
        char *hash=strchr(line,'#'); if(hash)*hash='\0';
        char *t=trim(line); if(!t[0]||t[0]=='$') continue;
        if (strchr(t,'{')) { if(stack_top<7) box_stack[stack_top++]=box; continue; }
        if (strchr(t,'}')) { box=(stack_top>0)?box_stack[--stack_top]:BOX_NONE; continue; }
        char *eq=strchr(t,'='); if(!eq) continue;
        *eq='\0';
        char key[128],val[512];
        strncpy(key,trim(t),    sizeof(key)-1); key[sizeof(key)-1]='\0';
        strncpy(val,trim(eq+1), sizeof(val)-1); val[sizeof(val)-1]='\0';
        if (!strcmp(key,"keybind")) parse_keybind(val);
    }
    fclose(f);
    LOG("[FERAWM] Config loaded — modifier=0x%x keybinds=%d vars=%d\n",
        cfg.modifier,cfg.keybind_count,cfg.var_count);
}

/* ══════════════════════════════════════════════════════════════════════════
 * XINPUT — apply input box settings via xinput/setxkbmap
 * ══════════════════════════════════════════════════════════════════════════ */
static void apply_xinput(void) {
    char cmd[512];
    /* touchpad natural scroll */
    snprintf(cmd,sizeof(cmd),
        "xinput list --name-only 2>/dev/null | grep -i 'touchpad\\|trackpad' | while read d; do "
        "xinput set-prop \"$d\" 'libinput Natural Scrolling Enabled' %d 2>/dev/null; done",
        cfg.touchpad_natural_scroll?1:0);
    if(fork()==0){setsid();execl("/bin/sh","sh","-c",cmd,NULL);exit(0);}
    /* mouse natural scroll */
    snprintf(cmd,sizeof(cmd),
        "xinput list --name-only 2>/dev/null | grep -iv 'touchpad\\|trackpad\\|keyboard' | grep -i 'mouse\\|pointer' | while read d; do "
        "xinput set-prop \"$d\" 'libinput Natural Scrolling Enabled' %d 2>/dev/null; done",
        cfg.mouse_natural_scroll?1:0);
    if(fork()==0){setsid();execl("/bin/sh","sh","-c",cmd,NULL);exit(0);}
    /* xkb */
    if (cfg.xkb_options[0])
        snprintf(cmd,sizeof(cmd),"setxkbmap -layout '%s' -option '%s'",cfg.xkb_layout,cfg.xkb_options);
    else
        snprintf(cmd,sizeof(cmd),"setxkbmap -layout '%s'",cfg.xkb_layout);
    if(fork()==0){setsid();execl("/bin/sh","sh","-c",cmd,NULL);exit(0);}
    LOG("[FERAWM] xinput applied\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * AUTOSTART
 * ══════════════════════════════════════════════════════════════════════════ */
static int is_process_running(const char *cmd) {
    char bin[256]; strncpy(bin,cmd,sizeof(bin)-1); bin[sizeof(bin)-1]='\0';
    char *sp=strchr(bin,' '); if(sp)*sp='\0';
    char *slash=strrchr(bin,'/'); const char *name=slash?slash+1:bin;
    DIR *d=opendir("/proc"); if(!d) return 0;
    struct dirent *e;
    while((e=readdir(d))){
        if(!isdigit((unsigned char)e->d_name[0])) continue;
        char cp[320]; snprintf(cp,sizeof(cp),"/proc/%s/comm",e->d_name);
        FILE *fp=fopen(cp,"r"); if(!fp) continue;
        char comm[256]={0};
        if(fgets(comm,sizeof(comm),fp)){
            char *nl=strchr(comm,'\n'); if(nl)*nl='\0';
            if(!strcmp(comm,name)){fclose(fp);closedir(d);return 1;}
        }
        fclose(fp);
    }
    closedir(d); return 0;
}

static void autostart_once_run(void) {
    for(int i=0;i<cfg.autostart_once_count;i++){
        if(is_process_running(cfg.autostart_once[i]))
            LOG("[FERAWM] autostart_once skip: %s\n",cfg.autostart_once[i]);
        else {
            LOG("[FERAWM] autostart_once spawn: %s\n",cfg.autostart_once[i]);
            if(fork()==0){setsid();execl("/bin/sh","sh","-c",cfg.autostart_once[i],NULL);exit(0);}
        }
    }
}

static void autostart_restart_run(void) {
    for(int i=0;i<cfg.autostart_restart_count;i++){
        char bin[256]; strncpy(bin,cfg.autostart_restart[i],sizeof(bin)-1); bin[sizeof(bin)-1]='\0';
        char *sp=strchr(bin,' '); if(sp)*sp='\0';
        char *slash=strrchr(bin,'/'); const char *name=slash?slash+1:bin;
        char kc[320]; snprintf(kc,sizeof(kc),"pkill -x '%s'",name);
        if(system(kc)){}
        usleep(100000);
        if(fork()==0){setsid();execl("/bin/sh","sh","-c",cfg.autostart_restart[i],NULL);exit(0);}
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * WM CORE
 * ══════════════════════════════════════════════════════════════════════════ */
static int is_mod_key(KeySym sym) {
    switch(cfg.modifier){
        case Mod1Mask:    return sym==XK_Alt_L    ||sym==XK_Alt_R;
        case Mod4Mask:    return sym==XK_Super_L  ||sym==XK_Super_R;
        case ControlMask: return sym==XK_Control_L||sym==XK_Control_R;
        case ShiftMask:   return sym==XK_Shift_L  ||sym==XK_Shift_R;
        default: return 0;
    }
}

static void grab_modifier_keys(void) {
    KeySym l,r;
    switch(cfg.modifier){
        case Mod4Mask:    l=XK_Super_L;  r=XK_Super_R;   break;
        case ControlMask: l=XK_Control_L;r=XK_Control_R; break;
        case ShiftMask:   l=XK_Shift_L;  r=XK_Shift_R;   break;
        default:          l=XK_Alt_L;    r=XK_Alt_R;      break;
    }
    KeyCode kc;
    kc=XKeysymToKeycode(dpy,l);if(kc)XGrabKey(dpy,kc,AnyModifier,root,True,GrabModeAsync,GrabModeAsync);
    kc=XKeysymToKeycode(dpy,r);if(kc)XGrabKey(dpy,kc,AnyModifier,root,True,GrabModeAsync,GrabModeAsync);
}

static void grab_keybinds(void) {
    for(int i=0;i<cfg.keybind_count;i++){
        KeyCode kc=XKeysymToKeycode(dpy,cfg.keybinds[i].sym);
        if(kc)XGrabKey(dpy,kc,cfg.keybinds[i].mod,root,True,GrabModeAsync,GrabModeAsync);
    }
}

static Bone *by_app(Window w)   {for(int i=0;i<bone_count;i++)if(bones[i].app==w) return &bones[i];return NULL;}
static Bone *by_bone(Window w)  {for(int i=0;i<bone_count;i++)if(bones[i].bone==w)return &bones[i];return NULL;}
static Bone *by_either(Window w){Bone *b=by_bone(w);return b?b:by_app(w);}
static Bone *focused_bone(void) {for(int i=0;i<bone_count;i++)if(bones[i].focused)return &bones[i];return NULL;}

static void snap_app(Bone *b) {
    if(!b->app)return;
    int aw=b->w-cfg.border_x*2, ah=b->h-cfg.border_y*2;
    if(aw<1)aw=1;
    if(ah<1)ah=1;
    XMoveResizeWindow(dpy,b->app,cfg.border_x,cfg.border_y,aw,ah);
    XSync(dpy,False);
}

static void ghost_hide(void) {
    for(int i=0;i<bone_count;i++){
        XMoveResizeWindow(dpy,bones[i].app,-1,-1,1,1);
        unsigned long col=bones[i].focused?cfg.bone_color:cfg.bone_unfocus;
        XSetWindowBackground(dpy,bones[i].bone,col);
        XClearWindow(dpy,bones[i].bone);
        XMapWindow(dpy,bones[i].bone);
    }
    XSync(dpy,False); ghost_active=1;
}
static void ghost_show(void) {
    for(int i=0;i<bone_count;i++) snap_app(&bones[i]);
    XSync(dpy,False); ghost_active=0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * EWMH
 * ══════════════════════════════════════════════════════════════════════════ */
static Atom NET_SUPPORTED, NET_CLIENT_LIST, NET_CLIENT_LIST_STACKING,
            NET_NUMBER_OF_DESKTOPS,
            NET_CURRENT_DESKTOP, NET_ACTIVE_WINDOW, NET_WM_DESKTOP,
            NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_NAME,
            NET_SUPPORTING_WM_CHECK, UTF8_STRING;

static void ewmh_init(void) {
    NET_SUPPORTED            = XInternAtom(dpy,"_NET_SUPPORTED",False);
    NET_CLIENT_LIST          = XInternAtom(dpy,"_NET_CLIENT_LIST",False);
    NET_CLIENT_LIST_STACKING = XInternAtom(dpy,"_NET_CLIENT_LIST_STACKING",False);
    NET_NUMBER_OF_DESKTOPS   = XInternAtom(dpy,"_NET_NUMBER_OF_DESKTOPS",False);
    NET_CURRENT_DESKTOP      = XInternAtom(dpy,"_NET_CURRENT_DESKTOP",False);
    NET_ACTIVE_WINDOW        = XInternAtom(dpy,"_NET_ACTIVE_WINDOW",False);
    NET_WM_DESKTOP           = XInternAtom(dpy,"_NET_WM_DESKTOP",False);
    NET_WM_STATE             = XInternAtom(dpy,"_NET_WM_STATE",False);
    NET_WM_STATE_FULLSCREEN  = XInternAtom(dpy,"_NET_WM_STATE_FULLSCREEN",False);
    NET_WM_NAME              = XInternAtom(dpy,"_NET_WM_NAME",False);
    NET_SUPPORTING_WM_CHECK  = XInternAtom(dpy,"_NET_SUPPORTING_WM_CHECK",False);
    UTF8_STRING              = XInternAtom(dpy,"UTF8_STRING",False);

    Window wc = XCreateSimpleWindow(dpy,root,0,0,1,1,0,0,0);
    XChangeProperty(dpy,wc,NET_WM_NAME,UTF8_STRING,8,PropModeReplace,(unsigned char*)"ferawm",6);
    XChangeProperty(dpy,wc,NET_SUPPORTING_WM_CHECK,XA_WINDOW,32,PropModeReplace,(unsigned char*)&wc,1);
    XChangeProperty(dpy,root,NET_SUPPORTING_WM_CHECK,XA_WINDOW,32,PropModeReplace,(unsigned char*)&wc,1);
    XChangeProperty(dpy,root,NET_WM_NAME,UTF8_STRING,8,PropModeReplace,(unsigned char*)"ferawm",5);

    Atom supported[] = {
        NET_SUPPORTED, NET_CLIENT_LIST, NET_CLIENT_LIST_STACKING,
        NET_NUMBER_OF_DESKTOPS,
        NET_CURRENT_DESKTOP, NET_ACTIVE_WINDOW, NET_WM_DESKTOP,
        NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_NAME,
        NET_SUPPORTING_WM_CHECK,
    };
    XChangeProperty(dpy,root,NET_SUPPORTED,XA_ATOM,32,PropModeReplace,
        (unsigned char*)supported, sizeof(supported)/sizeof(Atom));

    long nd = NUM_WORKSPACES;
    XChangeProperty(dpy,root,NET_NUMBER_OF_DESKTOPS,XA_CARDINAL,32,PropModeReplace,(unsigned char*)&nd,1);
    long cd = 0;
    XChangeProperty(dpy,root,NET_CURRENT_DESKTOP,XA_CARDINAL,32,PropModeReplace,(unsigned char*)&cd,1);
}

static void ewmh_update_client_list(void) {
    Window wins[MAX_BONES]; int n=0;
    for(int i=0;i<bone_count;i++) wins[n++]=bones[i].app;
    XChangeProperty(dpy,root,NET_CLIENT_LIST,XA_WINDOW,32,PropModeReplace,(unsigned char*)wins,n);
    XChangeProperty(dpy,root,NET_CLIENT_LIST_STACKING,XA_WINDOW,32,PropModeReplace,(unsigned char*)wins,n);
}
static void ewmh_update_current_desktop(void) {
    long cd=current_ws;
    XChangeProperty(dpy,root,NET_CURRENT_DESKTOP,XA_CARDINAL,32,PropModeReplace,(unsigned char*)&cd,1);
}
static void ewmh_update_active_window(Window w) {
    XChangeProperty(dpy,root,NET_ACTIVE_WINDOW,XA_WINDOW,32,PropModeReplace,(unsigned char*)&w,1);
}
static void ewmh_set_wm_desktop(Window w, int ws) {
    long d=ws;
    XChangeProperty(dpy,w,NET_WM_DESKTOP,XA_CARDINAL,32,PropModeReplace,(unsigned char*)&d,1);
}
static void ewmh_set_fullscreen(Window w, int fs) {
    if(fs) XChangeProperty(dpy,w,NET_WM_STATE,XA_ATOM,32,PropModeReplace,(unsigned char*)&NET_WM_STATE_FULLSCREEN,1);
    else   XDeleteProperty(dpy,w,NET_WM_STATE);
}

/* ── strut / usable area ─────────────────────────────────────────────────── */
typedef struct { int left, right, top, bottom; } Strut;

static Strut get_struts(void) {
    Strut s = {0,0,0,0};
    Window rr, pr, *ch; unsigned int n;
    if (!XQueryTree(dpy,root,&rr,&pr,&ch,&n) || !ch) return s;
    Atom strut     = XInternAtom(dpy,"_NET_WM_STRUT",False);
    Atom strut_p   = XInternAtom(dpy,"_NET_WM_STRUT_PARTIAL",False);
    for (unsigned int i = 0; i < n; i++) {
        Atom type; int fmt; unsigned long nitems, after;
        unsigned char *data = NULL;
        /* prefer _NET_WM_STRUT_PARTIAL, fall back to _NET_WM_STRUT */
        if (XGetWindowProperty(dpy,ch[i],strut_p,0,12,False,XA_CARDINAL,
                &type,&fmt,&nitems,&after,&data)==Success && data && nitems>=4) {
            long *v=(long*)data;
            if(v[0]>s.left)   s.left   = v[0];
            if(v[1]>s.right)  s.right  = v[1];
            if(v[2]>s.top)    s.top    = v[2];
            if(v[3]>s.bottom) s.bottom = v[3];
            XFree(data); data=NULL;
        } else if (XGetWindowProperty(dpy,ch[i],strut,0,4,False,XA_CARDINAL,
                &type,&fmt,&nitems,&after,&data)==Success && data && nitems>=4) {
            long *v=(long*)data;
            if(v[0]>s.left)   s.left   = v[0];
            if(v[1]>s.right)  s.right  = v[1];
            if(v[2]>s.top)    s.top    = v[2];
            if(v[3]>s.bottom) s.bottom = v[3];
            XFree(data);
        }
        if (data) XFree(data);
    }
    XFree(ch);
    return s;
}

static void usable_area(int *ux, int *uy, int *uw, int *uh) {
    Strut s = get_struts();
    *ux = s.left;
    *uy = s.top;
    *uw = DisplayWidth(dpy,screen)  - s.left - s.right;
    *uh = DisplayHeight(dpy,screen) - s.top  - s.bottom;
}

/* ══════════════════════════════════════════════════════════════════════════
 * TILING — positional, reflows on every change
 * cell = index in tiled order (0 = first, 1 = second, ...)
 * grid_cell: -1 = floating, 0+ = tiled order index
 * ══════════════════════════════════════════════════════════════════════════ */

/* rebuild tiled order indices so they're 0,1,2,... with no gaps */
static void tile_reindex(void) {
    int idx = 0;
    for (int i = 0; i < bone_count; i++)
        if (bones[i].workspace == current_ws && bones[i].grid_cell >= 0)
            bones[i].grid_cell = idx++;
}

static int tile_count(int ws) {
    int n = 0;
    for (int i = 0; i < bone_count; i++)
        if (bones[i].workspace == ws && bones[i].grid_cell >= 0) n++;
    return n;
}

/* given N tiled windows and a 0-based index, compute its pixel rect */
static void tile_rect_for(int idx, int n, int *ox, int *oy, int *ow, int *oh) {
    int ux, uy, uw, uh;
    usable_area(&ux, &uy, &uw, &uh);
    int og = cfg.bone_gap, ig = cfg.inner_gap;

    int rows = (n + 2) / 3;
    int row  = idx / 3;
    int col  = idx % 3;

    int wins_in_row;
    int last_row_start = (rows - 1) * 3;
    if (row == rows - 1)
        wins_in_row = n - last_row_start;
    else
        wins_in_row = 3;

    int ch = (uh - og*2 - ig*(rows-1)) / rows;
    int cw = (uw - og*2 - ig*(wins_in_row-1)) / wins_in_row;

    *ox = ux + og + col*(cw+ig);
    *oy = uy + og + row*(ch+ig);
    *ow = cw;
    *oh = ch;
}

static void tile_apply_all(void) {
    int n = tile_count(current_ws);
    if (n == 0) return;
    for (int i = 0; i < bone_count; i++) {
        Bone *b = &bones[i];
        if (b->workspace != current_ws || b->grid_cell < 0) continue;
        int x, y, w, h;
        tile_rect_for(b->grid_cell, n, &x, &y, &w, &h);
        b->x = x; b->y = y; b->w = w; b->h = h;
        XMoveResizeWindow(dpy, b->bone, b->x, b->y, b->w, b->h);
        snap_app(b);
    }
}

static void tile_toggle_mode(void) {
    cfg.tile_mode = !cfg.tile_mode;
    if (cfg.tile_mode) {
        int idx = 0;
        for (int i = 0; i < bone_count; i++) {
            if (bones[i].workspace != current_ws) continue;
            if (bones[i].grid_cell >= 0) continue;
            if (idx < GRID_MAX) bones[i].grid_cell = idx++;
        }
        tile_apply_all();
    }
    LOG("[FERAWM] tile_mode=%d\n", cfg.tile_mode);
}

static void tile_toggle_float(Bone *b) {
    if (!b) return;
    if (b->grid_cell >= 0) {
        b->grid_cell = -1;
        tile_reindex();
        tile_apply_all();
    } else if (cfg.tile_mode) {
        int n = tile_count(current_ws);
        if (n < GRID_MAX) {
            b->grid_cell = n;
            tile_apply_all();
        }
    }
}

static void focus_dir(int dcol, int drow);
static void focus_bone_ex(Bone *b, int do_warp);
static void focus_bone(Bone *b);

static void focus_dir(int dcol, int drow) {
    Bone *b = focused_bone(); if (!b || b->grid_cell < 0) return;
    int n = tile_count(current_ws);
    int rows = (n + 2) / 3;
    int cur_row = b->grid_cell / 3;
    int cur_col = b->grid_cell % 3;
    int new_row = cur_row + drow;
    int new_col = cur_col + dcol;
    if (new_row < 0 || new_row >= rows) return;
    int wins_in_new_row = (new_row == rows-1) ? (n - (rows-1)*3) : 3;
    if (new_col < 0 || new_col >= wins_in_new_row) new_col = wins_in_new_row - 1;
    int target = new_row * 3 + new_col;
    if (target < 0 || target >= n) return;
    for (int i = 0; i < bone_count; i++) {
        if (bones[i].workspace == current_ws && bones[i].grid_cell == target) {
            focus_bone_ex(&bones[i], 1);
            return;
        }
    }
}

static void tile_move(Bone *b, int dcol, int drow) {
    if (!b || b->grid_cell < 0) return;
    int n = tile_count(current_ws);
    int rows = (n + 2) / 3;
    int cur_row = b->grid_cell / 3;
    int cur_col = b->grid_cell % 3;
    int new_row = cur_row + drow;
    int new_col = cur_col + dcol;
    if (new_row < 0 || new_row >= rows) return;
    /* cols in new row */
    int wins_in_new_row = (new_row == rows-1) ? (n - (rows-1)*3) : 3;
    if (new_col < 0 || new_col >= wins_in_new_row) return;
    int target = new_row * 3 + new_col;
    if (target < 0 || target >= n) return;
    /* swap */
    for (int i = 0; i < bone_count; i++) {
        if (bones[i].workspace == current_ws && bones[i].grid_cell == target) {
            bones[i].grid_cell = b->grid_cell;
            break;
        }
    }
    b->grid_cell = target;
    tile_apply_all();
    if (cfg.mouse_focus)
        XWarpPointer(dpy,None,b->bone,0,0,0,0,b->w/2,b->h/2);
}

/* on drag release — snap to nearest cell */
static void tile_drop_snap(Bone *b) {
    if (!b || !cfg.tile_mode || b->grid_cell < 0) return;
    int cx = b->x + b->w/2;
    int cy = b->y + b->h/2;
    int n = tile_count(current_ws);
    int best = b->grid_cell, best_dist = 999999;
    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        tile_rect_for(i, n, &x, &y, &w, &h);
        int cellcx = x + w/2, cellcy = y + h/2;
        int dist = (cx-cellcx)*(cx-cellcx) + (cy-cellcy)*(cy-cellcy);
        if (dist < best_dist) { best_dist = dist; best = i; }
    }
    if (best != b->grid_cell) {
        for (int i = 0; i < bone_count; i++) {
            if (bones[i].workspace == current_ws && bones[i].grid_cell == best) {
                bones[i].grid_cell = b->grid_cell;
                break;
            }
        }
        b->grid_cell = best;
    }
    tile_apply_all();
}

static void grab_all(void) {
    for(int i=0;i<bone_count;i++){
        XGrabButton(dpy,AnyButton,AnyModifier,bones[i].bone,True,
            ButtonPressMask|ButtonReleaseMask|PointerMotionMask,GrabModeAsync,GrabModeAsync,None,None);
        XGrabButton(dpy,AnyButton,AnyModifier,bones[i].app,True,
            ButtonPressMask|ButtonReleaseMask|PointerMotionMask,GrabModeAsync,GrabModeAsync,None,None);
    }
}
static void ungrab_all(void) {
    for(int i=0;i<bone_count;i++){
        XUngrabButton(dpy,AnyButton,AnyModifier,bones[i].bone);
        XUngrabButton(dpy,AnyButton,AnyModifier,bones[i].app);
    }
    XUngrabPointer(dpy,CurrentTime);
}

static int xerror_handler(Display *d,XErrorEvent *e){
    if(e->error_code==BadWindow||e->error_code==BadMatch)return 0;
    char msg[64]; XGetErrorText(d,e->error_code,msg,sizeof(msg));
    fprintf(stderr,"[FERAWM] X error: %s (opcode %d)\n",msg,e->request_code);
    return 0;
}

static int is_dock_window(Window w) {
    Atom wtype = XInternAtom(dpy,"_NET_WM_WINDOW_TYPE",False);
    Atom dock  = XInternAtom(dpy,"_NET_WM_WINDOW_TYPE_DOCK",False);
    Atom type; int fmt; unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy,w,wtype,0,32,False,XA_ATOM,
            &type,&fmt,&n,&after,&data)==Success && data) {
        Atom *atoms = (Atom*)data;
        for (unsigned long i = 0; i < n; i++) {
            if (atoms[i] == dock) { XFree(data); return 1; }
        }
        XFree(data);
    }
    return 0;
}

static void raise_docks(void) {
    Window rr, pr, *ch; unsigned int n;
    if (!XQueryTree(dpy,root,&rr,&pr,&ch,&n) || !ch) return;
    for (unsigned int i = 0; i < n; i++)
        if (is_dock_window(ch[i])) XRaiseWindow(dpy,ch[i]);
    XFree(ch);
}

static void focus_bone_ex(Bone *b, int do_warp){
    for(int i=0;i<bone_count;i++){
        bones[i].focused=0;
        XSetWindowBackground(dpy,bones[i].bone,cfg.bone_unfocus);
        XClearWindow(dpy,bones[i].bone);
    }
    if(!b){XSetInputFocus(dpy,root,RevertToPointerRoot,CurrentTime);
        ewmh_update_active_window(None);return;}
    b->focused=1;
    XSetWindowBackground(dpy,b->bone,cfg.bone_color);
    XClearWindow(dpy,b->bone);
    XSetInputFocus(dpy,b->app,RevertToPointerRoot,CurrentTime);
    XRaiseWindow(dpy,b->bone);
    ewmh_update_active_window(b->app);
    if (cfg.mouse_focus && do_warp)
        XWarpPointer(dpy,None,b->bone,0,0,0,0,b->w/2,b->h/2);
    raise_docks();
}
static void focus_bone(Bone *b){ focus_bone_ex(b, 0); }

static void cycle_focus(void){
    int cur=-1, first=-1, next=-1;
    for(int i=0;i<bone_count;i++){
        if(bones[i].workspace!=current_ws) continue;
        if(first<0) first=i;
        if(bones[i].focused) cur=i;
        else if(cur>=0 && next<0) next=i;
    }
    if(first<0) return;
    focus_bone_ex(&bones[next>=0?next:first], 1);
}

static void toggle_fullscreen(Bone *b){
    if(!b)return;
    if(!b->fullscreen){
        b->pre_fs_x=b->x;b->pre_fs_y=b->y;b->pre_fs_w=b->w;b->pre_fs_h=b->h;
        b->x=0;b->y=0;b->w=DisplayWidth(dpy,screen);b->h=DisplayHeight(dpy,screen);
        b->fullscreen=1;
    } else {
        b->x=b->pre_fs_x;b->y=b->pre_fs_y;b->w=b->pre_fs_w;b->h=b->pre_fs_h;
        b->fullscreen=0;
    }
    XMoveResizeWindow(dpy,b->bone,b->x,b->y,b->w,b->h);
    snap_app(b); XRaiseWindow(dpy,b->bone);
    ewmh_set_fullscreen(b->app, b->fullscreen);
}

static void create_bone(Window app){
    if(bone_count>=MAX_BONES)return;
    XWindowAttributes wa; XGetWindowAttributes(dpy,app,&wa);
    int bw=(wa.width>10)?wa.width:400, bh=(wa.height>10)?wa.height:300;
    int bx=cfg.bone_gap+(bone_count*30%200), by=cfg.bone_gap+(bone_count*30%150);
    Window bone=XCreateSimpleWindow(dpy,root,bx,by,bw,bh,0,0,cfg.bone_unfocus);
    XSelectInput(dpy,bone,SubstructureRedirectMask|SubstructureNotifyMask|
        ButtonPressMask|ButtonReleaseMask|PointerMotionMask|EnterWindowMask|ExposureMask);
    XAddToSaveSet(dpy,app);
    XReparentWindow(dpy,app,bone,0,0);
    XMoveResizeWindow(dpy,app,0,0,bw,bh);
    XMapWindow(dpy,bone);XMapWindow(dpy,app);XSync(dpy,False);
    bones[bone_count]=(Bone){.bone=bone,.app=app,.x=bx,.y=by,.w=bw,.h=bh,.workspace=current_ws,.grid_cell=-1};
    if (cfg.tile_mode) {
        int n = tile_count(current_ws);
        if (n < GRID_MAX) bones[bone_count].grid_cell = n;
    }
    bone_count++;
    if (cfg.tile_mode) tile_apply_all();
    ewmh_set_wm_desktop(app, current_ws);
    ewmh_update_client_list();
    if(mod_held)grab_all();
    LOG("[FERAWM] Bone %lu %dx%d @ %d,%d\n",app,bw,bh,bx,by);
}

static void remove_bone(Bone *b){
    int was_focused=b->focused;
    int had_cell=b->grid_cell;
    XDestroyWindow(dpy,b->bone);
    int idx=b-bones;
    memmove(&bones[idx],&bones[idx+1],(bone_count-idx-1)*sizeof(Bone));
    bone_count--;
    if(was_focused)focus_bone(bone_count>0?&bones[0]:NULL);
    ewmh_update_client_list();
    if(cfg.tile_mode && had_cell>=0){ tile_reindex(); tile_apply_all(); }
}

static void do_spawn(const char *cmd){
    if(fork()==0){setsid();execl("/bin/sh","sh","-c",cmd,NULL);exit(0);}
}

static void close_window(Bone *b){
    if(!b)return;
    Atom wp=XInternAtom(dpy,"WM_PROTOCOLS",False);
    Atom wd=XInternAtom(dpy,"WM_DELETE_WINDOW",False);
    Atom *proto; int n;
    if(XGetWMProtocols(dpy,b->app,&proto,&n)){
        for(int i=0;i<n;i++){
            if(proto[i]==wd){
                XEvent ev={0};
                ev.type=ClientMessage;ev.xclient.window=b->app;
                ev.xclient.message_type=wp;ev.xclient.format=32;
                ev.xclient.data.l[0]=wd;ev.xclient.data.l[1]=CurrentTime;
                XSendEvent(dpy,b->app,False,NoEventMask,&ev);
                XFree(proto);return;
            }
        }
        XFree(proto);
    }
    XDestroyWindow(dpy,b->app);
}

static void adopt_existing_windows(void){
    Window rr,pr,*ch; unsigned int n;
    XQueryTree(dpy,root,&rr,&pr,&ch,&n); if(!ch)return;
    for(unsigned int i=0;i<n;i++){
        XWindowAttributes wa;
        if(!XGetWindowAttributes(dpy,ch[i],&wa))continue;
        if(wa.override_redirect)continue;
        if(wa.map_state!=IsViewable)continue;
        if(by_bone(ch[i])||by_app(ch[i]))continue;
        if(is_dock_window(ch[i]))continue;
        create_bone(ch[i]);
    }
    XFree(ch);
}

static void reload_config(void){
    LOG_ALWAYS("[FERAWM] Reloading config...\n");
    for(int i=0;i<cfg.keybind_count;i++){
        KeyCode kc=XKeysymToKeycode(dpy,cfg.keybinds[i].sym);
        if(kc)XUngrabKey(dpy,kc,cfg.keybinds[i].mod,root);
    }
    cfg.keybind_count=0;cfg.autostart_once_count=0;
    cfg.autostart_restart_count=0;cfg.var_count=0;
    load_config();
    XUngrabKey(dpy,AnyKey,AnyModifier,root);
    grab_modifier_keys();grab_keybinds();
    for(int i=0;i<bone_count;i++)snap_app(&bones[i]);
    for(int i=0;i<bone_count;i++){
        unsigned long col=bones[i].focused?cfg.bone_color:cfg.bone_unfocus;
        XSetWindowBackground(dpy,bones[i].bone,col);XClearWindow(dpy,bones[i].bone);
    }
    apply_xinput();
    LOG_ALWAYS("[FERAWM] Config reloaded.\n");
}
static void exec_restart(void){
    LOG_ALWAYS("[FERAWM] Restarting...\n");
    autostart_restart_run();
    char self[512]={0};
    if(readlink("/proc/self/exe",self,sizeof(self)-1)<0)
        strncpy(self,g_argv[0],sizeof(self)-1);
    XSync(dpy,False);XCloseDisplay(dpy);
    execv(self,g_argv);
    perror("[FERAWM] exec_restart failed");exit(1);
}

static void inotify_setup(void){
    char path[512]; const char *home=getenv("HOME");if(!home)home="/root";
    snprintf(path,sizeof(path),"%s/.config/fera/ferawm.conf",home);
    inotify_fd=inotify_init1(IN_NONBLOCK); if(inotify_fd<0)return;
    inotify_add_watch(inotify_fd,path,IN_CLOSE_WRITE|IN_MODIFY);
}

static void inotify_check(void){
    if(inotify_fd<0)return;
    char buf[512];
    if(read(inotify_fd,buf,sizeof(buf))>0)reload_config();
}

static void bone_park(Bone *b) {
    XMoveWindow(dpy, b->bone, PARK_X, PARK_Y);
}

static void bone_unpark(Bone *b) {
    XMoveWindow(dpy, b->bone, b->x, b->y);
}

static void ws_switch(int ws) {
    if (ws < 0 || ws >= NUM_WORKSPACES || ws == current_ws) return;
    /* park current workspace bones */
    for (int i = 0; i < bone_count; i++)
        if (bones[i].workspace == current_ws) bone_park(&bones[i]);
    current_ws = ws;
    /* unpark new workspace bones */
    int found = -1;
    for (int i = 0; i < bone_count; i++) {
        if (bones[i].workspace == current_ws) {
            bone_unpark(&bones[i]);
            if (found < 0) found = i;
        }
    }
    /* refocus first bone on new workspace */
    focus_bone_ex(found >= 0 ? &bones[found] : NULL, 1);
    ewmh_update_current_desktop();
    XSync(dpy, False);
    LOG("[FERAWM] Switched to workspace %d\n", current_ws + 1);
}

static void ws_move_focused(int ws) {
    if (ws < 0 || ws >= NUM_WORKSPACES) return;
    Bone *b = focused_bone(); if (!b) return;
    if (b->workspace == ws) return;
    b->workspace = ws;
    bone_park(b);
    ewmh_set_wm_desktop(b->app, ws);
    /* focus next visible bone on current workspace */
    int found = -1;
    for (int i = 0; i < bone_count; i++)
        if (bones[i].workspace == current_ws && &bones[i] != b) { found = i; break; }
    focus_bone(found >= 0 ? &bones[found] : NULL);
    XSync(dpy, False);
    LOG("[FERAWM] Moved bone to workspace %d\n", ws + 1);
}

static void run_action(Action a, const char *sc, int ws_num){
    switch(a){
        case ACTION_KILL_WINDOW: close_window(focused_bone()); break;
        case ACTION_KILL_WM:     LOG_ALWAYS("[FERAWM] Goodbye!\n");XCloseDisplay(dpy);exit(0);
        case ACTION_FULLSCREEN:  toggle_fullscreen(focused_bone()); break;
        case ACTION_CYCLE_FOCUS: cycle_focus(); break;
        case ACTION_SPAWN:       if(sc&&sc[0])do_spawn(sc); break;
        case ACTION_RELOAD:      reload_config(); break;
        case ACTION_RESTART:     exec_restart(); break;
        case ACTION_WS_SWITCH:       ws_switch(ws_num); break;
        case ACTION_WS_MOVE:         ws_move_focused(ws_num); break;
        case ACTION_TILE_TOGGLE:     tile_toggle_mode(); break;
        case ACTION_TILE_FLOAT:      tile_toggle_float(focused_bone()); break;
        case ACTION_TILE_MOVE_LEFT:  tile_move(focused_bone(), -1,  0); break;
        case ACTION_TILE_MOVE_RIGHT: tile_move(focused_bone(),  1,  0); break;
        case ACTION_TILE_MOVE_UP:    tile_move(focused_bone(),  0, -1); break;
        case ACTION_TILE_MOVE_DOWN:  tile_move(focused_bone(),  0,  1); break;
        case ACTION_FOCUS_LEFT:      focus_dir(-1,  0); break;
        case ACTION_FOCUS_RIGHT:     focus_dir( 1,  0); break;
        case ACTION_FOCUS_UP:        focus_dir( 0, -1); break;
        case ACTION_FOCUS_DOWN:      focus_dir( 0,  1); break;
        default: break;
    }
}

static void on_client_message(XClientMessageEvent *e) {
    if ((Atom)e->message_type == NET_ACTIVE_WINDOW) {
        Bone *b = by_app(e->window);
        if (!b) b = by_bone(e->window);
        if (b) {
            if (b->workspace != current_ws) ws_switch(b->workspace);
            focus_bone(b);
            XRaiseWindow(dpy, b->bone);
            raise_docks();
        }
    }
}

static void on_map_request(XMapRequestEvent *e){
    if(by_app(e->window)){XMapWindow(dpy,e->window);return;}
    if(is_dock_window(e->window)){XMapWindow(dpy,e->window);return;}
    create_bone(e->window);
}

static void on_destroy(XDestroyWindowEvent *e){
    Bone *b=by_app(e->window);
    if(b){remove_bone(b);return;}
    b=by_bone(e->window);
    if(b){
        int idx=b-bones,wf=b->focused;
        memmove(&bones[idx],&bones[idx+1],(bone_count-idx-1)*sizeof(Bone));
        bone_count--;
        if(wf)focus_bone(bone_count>0?&bones[0]:NULL);
    }
}

static void on_unmap(XUnmapEvent *e){
    if(ghost_active)return;
    Bone *b=by_app(e->window);if(b)XUnmapWindow(dpy,b->bone);
}

static void on_configure_request(XConfigureRequestEvent *e){
    if(!by_app(e->window)){
        XWindowChanges wc={.x=e->x,.y=e->y,.width=e->width,.height=e->height,.border_width=0};
        XConfigureWindow(dpy,e->window,e->value_mask,&wc);
    }
    raise_docks();
}

static void on_key_press(XKeyEvent *e){
    KeySym sym=XLookupKeysym(e,0);
    if(is_mod_key(sym)){
        if(!mod_held){mod_held=1;if(cfg.animate_tab==ANIM_GHOST)ghost_hide();grab_all();}
        return;
    }
    unsigned int clean=e->state&~(LockMask|Mod2Mask|Mod3Mask|Mod5Mask);
    for(int i=0;i<cfg.keybind_count;i++)
        if(sym==cfg.keybinds[i].sym&&clean==cfg.keybinds[i].mod){
            run_action(cfg.keybinds[i].action,cfg.keybinds[i].spawn_cmd,cfg.keybinds[i].ws_num);return;
        }
}

static void on_key_release(XKeyEvent *e){
    KeySym sym=XLookupKeysym(e,0);
    if(is_mod_key(sym)){
        mod_held=dragging=resizing=0;drag_bone=NULL;
        if(cfg.animate_tab==ANIM_GHOST)ghost_show();
        else for(int i=0;i<bone_count;i++)snap_app(&bones[i]);
        ungrab_all();
    }
}

static void on_button_press(XButtonEvent *e){
    Bone *b=by_either(e->window);if(!b)return;
    focus_bone(b);if(!mod_held)return;
    if(e->button==Button1){
        dragging=1;resizing=0;drag_bone=b;
        drag_sx=e->x_root;drag_sy=e->y_root;drag_bx=b->x;drag_by=b->y;
        XGrabPointer(dpy,root,True,PointerMotionMask|ButtonReleaseMask,GrabModeAsync,GrabModeAsync,None,None,CurrentTime);
    }else if(e->button==Button3){
        resizing=1;dragging=0;drag_bone=b;
        drag_sx=e->x_root;drag_sy=e->y_root;drag_bw=b->w;drag_bh=b->h;
        XGrabPointer(dpy,root,True,PointerMotionMask|ButtonReleaseMask,GrabModeAsync,GrabModeAsync,None,None,CurrentTime);
    }
}

static void on_button_release(XButtonEvent *e){
    (void)e;
    if(dragging && drag_bone) tile_drop_snap(drag_bone);
    if(dragging||resizing){XUngrabPointer(dpy,CurrentTime);if(mod_held)grab_all();dragging=resizing=0;drag_bone=NULL;}
}

static void on_motion(XMotionEvent *e){
    if(!drag_bone)return;
    int dx=e->x_root-drag_sx,dy=e->y_root-drag_sy;
    if(dragging){drag_bone->x=drag_bx+dx;drag_bone->y=drag_by+dy;XMoveWindow(dpy,drag_bone->bone,drag_bone->x,drag_bone->y);}
    else if(resizing){
        int nw=drag_bw+dx;if(nw<80)nw=80;
        int nh=drag_bh+dy;if(nh<60)nh=60;
        drag_bone->w=nw;drag_bone->h=nh;
        XResizeWindow(dpy,drag_bone->bone,nw,nh);
        if(cfg.animate_tab==ANIM_LIVE)snap_app(drag_bone);
    }
    if(ghost_active&&cfg.animate_tab==ANIM_GHOST){
        XMoveResizeWindow(dpy,drag_bone->app,-1,-1,1,1);
        unsigned long col=drag_bone->focused?cfg.bone_color:cfg.bone_unfocus;
        XSetWindowBackground(dpy,drag_bone->bone,col);XClearWindow(dpy,drag_bone->bone);
    }
}

static void on_enter(XCrossingEvent *e){
    if (!cfg.mouse_focus || dragging || resizing) return;
    Bone *b=by_either(e->window);if(b)focus_bone(b);
}

int main(int argc,char *argv[]){
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-v")||!strcmp(argv[i],"--version")){printf("ferawm %s\n",FERAWM_VERSION);return 0;}
        else if(!strcmp(argv[i],"-V")||!strcmp(argv[i],"--verbose"))verbose=1;
        else{fprintf(stderr,"ferawm: unknown option '%s'\nusage: ferawm [-v] [-V]\n",argv[i]);return 1;}
    }
    g_argv=argv;
    load_config();
    dpy=XOpenDisplay(NULL);
    if(!dpy){fprintf(stderr,"[FERAWM] Can't open display.\n");return 1;}
    screen=DefaultScreen(dpy);root=RootWindow(dpy,screen);
    ewmh_init();
    XSetErrorHandler(xerror_handler);
    grab_modifier_keys();grab_keybinds();
    inotify_setup();
    adopt_existing_windows();
    autostart_once_run();
    apply_xinput();
    XSelectInput(dpy,root,SubstructureRedirectMask|SubstructureNotifyMask|
        KeyPressMask|KeyReleaseMask|ButtonPressMask|PropertyChangeMask|SubstructureNotifyMask);
    Cursor root_cursor = XCreateFontCursor(dpy, XC_left_ptr);
    XDefineCursor(dpy, root, root_cursor);
    XSync(dpy,False);
    LOG_ALWAYS("[FERAWM] ferawm %s ready\n",FERAWM_VERSION);
    if(verbose){
        const char *mn=cfg.modifier==Mod4Mask?"super":cfg.modifier==ControlMask?"ctrl":cfg.modifier==ShiftMask?"shift":"alt";
        LOG_ALWAYS("[FERAWM] modifier         : %s\n",mn);
        LOG_ALWAYS("[FERAWM] animate_tab      : %d\n",cfg.animate_tab);
        LOG_ALWAYS("[FERAWM] keybinds         : %d\n",cfg.keybind_count);
        LOG_ALWAYS("[FERAWM] border           : %dx%d\n",cfg.border_x,cfg.border_y);
        LOG_ALWAYS("[FERAWM] vars             : %d\n",cfg.var_count);
        LOG_ALWAYS("[FERAWM] autostart_once   : %d\n",cfg.autostart_once_count);
        LOG_ALWAYS("[FERAWM] autostart_restart: %d\n",cfg.autostart_restart_count);
        LOG_ALWAYS("[FERAWM] xkb_layout       : %s\n",cfg.xkb_layout);
        LOG_ALWAYS("[FERAWM] touchpad_scroll  : %s\n",cfg.touchpad_natural_scroll?"natural":"normal");
    }
    XEvent ev;
    while(1){
        inotify_check();
        XNextEvent(dpy,&ev);
        switch(ev.type){
            case MapRequest:       on_map_request(&ev.xmaprequest);             break;
            case ClientMessage:    on_client_message(&ev.xclient);              break;
            case DestroyNotify:    on_destroy(&ev.xdestroywindow);              break;
            case UnmapNotify:      on_unmap(&ev.xunmap);                        break;
            case ConfigureRequest: on_configure_request(&ev.xconfigurerequest); break;
            case KeyPress:         on_key_press(&ev.xkey);                      break;
            case KeyRelease:       on_key_release(&ev.xkey);                    break;
            case ButtonPress:      on_button_press(&ev.xbutton);                break;
            case ButtonRelease:    on_button_release(&ev.xbutton);              break;
            case MotionNotify:     on_motion(&ev.xmotion);                      break;
            case EnterNotify:      on_enter(&ev.xcrossing);                     break;
            case PropertyNotify:
                if (cfg.tile_mode) tile_apply_all();
                break;
        }
    }
    XCloseDisplay(dpy);return 0;
}
