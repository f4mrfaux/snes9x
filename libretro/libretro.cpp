#include "libretro.h"
#include "libretro_core_options.h"

#include "snes9x.h"
#include "memmap.h"
#include "srtc.h"
#include "apu/apu.h"
#include "apu/bapu/snes/snes.hpp"
#include "gfx.h"
#include "snapshot.h"
#include "controls.h"
#include "cheats.h"
#include "movie.h"
#include "display.h"
#include "conffile.h"
#include "crosshairs.h"
#include <stdio.h>
#include <cstdlib>
#include <vector>
#include <string>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include "filter/snes_ntsc.h"
#include "spen_servo.h"

/* --- S-Pen per-game RAM-cursor profile table (compiled-in, no external files) ---
   The SNES Mouse is RELATIVE, so the absolute-pen servo must dead-reckon the game
   cursor UNLESS we know exactly where the game stores its real cursor in WRAM. For
   such games we read that WRAM byte and close the loop (gain-independent, no drift).
   Keyed on the ROM's internal title prefix (Memory.ROMName, space-padded, uppercase).
   Addresses are WRAM offsets: $7Exxxx == Memory.RAM[xxxx]. Add entries only after a
   READ-ONLY RAM search confirms the byte tracks the on-screen cursor on real ROMs. */
typedef struct { const char *title; uint16_t cur_x_addr; uint16_t cur_y_addr; } spen_mouse_profile_t;
/* NOTE: the `title` strings below are BEST-GUESS prefixes of each ROM's SNES header
   internal name (Memory.ROMName). They may need correction: confirm the exact title
   from the [SPEN-PROFILE] log line on a real load. A wrong title just fails to match
   and falls back harmlessly to the dead-reckoning servo, so an incorrect guess is safe.
   "LEMMINGS 2" is listed BEFORE "LEMMINGS" so it is not shadowed by the shorter prefix;
   the resolver also independently picks the LONGEST matching title for robustness. */
static const spen_mouse_profile_t SPEN_MOUSE_PROFILES[] = {
    { "MARIO PAINT",    0x0226, 0x0227 },  /* USA dumps with internal name "MARIO PAINT" */
    { "MARIOPAINT",     0x0226, 0x0227 },  /* Europe dumps (CRC 266B220E etc) — ROM name has no space */
    /* CLOCK TOWER SFX (T-En patched Deluxe, CRC 08E67AFC): auto-finder
     * confirms $017F r=0.96 for Y (top candidate) and $017E r=0.79 for X
     * (top X candidate). Closed-loop RAM-feedback eliminates the dead-
     * reckoning "trailing cursor" drift the user was seeing — without
     * the profile, the deadzone gating freezes the wire delta when the
     * pen moves slowly, and the cursor lags behind. Verified on
     * hardware (Z Fold 5, 2026-05-29). */
    { "CLOCK TOWER",    0x017E, 0x017F },
    { "LEMMINGS 2",     0x0C34, 0x0C35 },  /* snes-mouse-lua (USA); title prefix unverified */
    { "LEMMINGS",       0x0071, 0x0073 },  /* snes-mouse-lua (USA); title prefix unverified */
    { "SIMCITY",        0x01EB, 0x01ED },  /* snes-mouse-lua (USA); title prefix unverified */
    { "DUNGEON MASTER", 0x0034, 0x0036 },  /* snes-mouse-lua; title prefix unverified */
};

/* Resolved per-load: enabled only when the loaded ROM matches a profile above. */
static bool     spen_ram_cursor_enabled = false;
static uint16_t spen_ram_cur_x_addr = 0, spen_ram_cur_y_addr = 0;

#ifdef DEBUG_SPEN_VERBOSE
/* log_cb is defined later in this TU; forward-declare so the finder can log. */
extern retro_log_printf_t log_cb;
/* --- WRAM cursor AUTO-FINDER (debug builds, unprofiled games only) ---------------
   When a game has NO RAM-cursor profile, this correlates every low-WRAM byte against
   the absolute pen target as the user sweeps the S-Pen, so the byte that holds the
   game's real cursor (X and Y) reveals itself as the highest |Pearson r|. The found
   addresses can then be added to SPEN_MOUSE_PROFILES[] above. Read-only; debug only. */
#define SPEN_FINDER_BYTES   0x2000   /* scan WRAM offsets 0x0000..0x1FFF (8KB) */
#define SPEN_FINDER_WINDOW  256      /* report every N accumulated samples, then reset */
typedef struct {
    double n;
    double st, sr, str_, st2, sr2;   /* Σt, Σr, Σt·r, Σt², Σr²  (t=target axis, r=RAM byte) */
    int    rmin, rmax;               /* observed RAM-byte range (skip never-varying bytes) */
} spen_finder_acc_t;

/* X axis correlates against target_x, Y against target_y. ~8192*2*(5 doubles + ...) */
static spen_finder_acc_t spen_finder_x[SPEN_FINDER_BYTES];
static spen_finder_acc_t spen_finder_y[SPEN_FINDER_BYTES];
static int               spen_finder_samples = 0;

static void spen_finder_reset(void)
{
    memset(spen_finder_x, 0, sizeof(spen_finder_x));
    memset(spen_finder_y, 0, sizeof(spen_finder_y));
    for (int i = 0; i < SPEN_FINDER_BYTES; i++) {
        spen_finder_x[i].rmin = spen_finder_y[i].rmin = 255;
        spen_finder_x[i].rmax = spen_finder_y[i].rmax = 0;
    }
    spen_finder_samples = 0;
}

static void spen_finder_accum_one(spen_finder_acc_t *a, double t, int r)
{
    a->n    += 1.0;
    a->st   += t;
    a->sr   += (double)r;
    a->str_ += t * (double)r;
    a->st2  += t * t;
    a->sr2  += (double)r * (double)r;
    if (r < a->rmin) a->rmin = r;
    if (r > a->rmax) a->rmax = r;
}

static double spen_finder_pearson(const spen_finder_acc_t *a)
{
    if (a->n < 2.0) return 0.0;
    double num = a->n * a->str_ - a->st * a->sr;
    double dt  = a->n * a->st2  - a->st * a->st;
    double dr  = a->n * a->sr2  - a->sr * a->sr;
    if (dt <= 0.0 || dr <= 0.0) return 0.0;   /* one axis never varied => no correlation */
    return num / sqrt(dt * dr);
}

/* Find the top-3 |r| byte offsets in one axis' accumulator set. */
static void spen_finder_top3(const spen_finder_acc_t *set, int *off, double *r)
{
    off[0] = off[1] = off[2] = -1;
    r[0] = r[1] = r[2] = 0.0;
    for (int i = 0; i < SPEN_FINDER_BYTES; i++) {
        if (set[i].rmax <= set[i].rmin) continue;   /* byte never changed */
        double rv = spen_finder_pearson(&set[i]);
        double a  = rv < 0.0 ? -rv : rv;
        if (a > (r[0] < 0.0 ? -r[0] : r[0])) {
            off[2] = off[1]; r[2] = r[1];
            off[1] = off[0]; r[1] = r[0];
            off[0] = i;      r[0] = rv;
        } else if (a > (r[1] < 0.0 ? -r[1] : r[1])) {
            off[2] = off[1]; r[2] = r[1];
            off[1] = i;      r[1] = rv;
        } else if (a > (r[2] < 0.0 ? -r[2] : r[2])) {
            off[2] = i;      r[2] = rv;
        }
    }
}

/* Accumulate one frame's (target_x, target_y) against all scanned WRAM bytes, then
   every SPEN_FINDER_WINDOW samples log the best cursor-address candidates and reset. */
static void spen_finder_update(double target_x, double target_y)
{
    for (int off = 0; off < SPEN_FINDER_BYTES; off++) {
        int r = (int)Memory.RAM[off];
        spen_finder_accum_one(&spen_finder_x[off], target_x, r);
        spen_finder_accum_one(&spen_finder_y[off], target_y, r);
    }
    if (++spen_finder_samples >= SPEN_FINDER_WINDOW) {
        int    xo[3], yo[3];
        double xr[3], yr[3];
        spen_finder_top3(spen_finder_x, xo, xr);
        spen_finder_top3(spen_finder_y, yo, yr);
        if (log_cb) {
            log_cb(RETRO_LOG_INFO,
                "[SPEN-FINDER] ROM='%.21s' Xcand: $%04X(r=%.2f,%d-%d) $%04X(r=%.2f,%d-%d) $%04X(r=%.2f,%d-%d) | Ycand: $%04X(r=%.2f,%d-%d) $%04X(r=%.2f,%d-%d) $%04X(r=%.2f,%d-%d)\n",
                Memory.ROMName,
                xo[0] < 0 ? 0 : xo[0], xr[0], xo[0] < 0 ? 0 : spen_finder_x[xo[0]].rmin, xo[0] < 0 ? 0 : spen_finder_x[xo[0]].rmax,
                xo[1] < 0 ? 0 : xo[1], xr[1], xo[1] < 0 ? 0 : spen_finder_x[xo[1]].rmin, xo[1] < 0 ? 0 : spen_finder_x[xo[1]].rmax,
                xo[2] < 0 ? 0 : xo[2], xr[2], xo[2] < 0 ? 0 : spen_finder_x[xo[2]].rmin, xo[2] < 0 ? 0 : spen_finder_x[xo[2]].rmax,
                yo[0] < 0 ? 0 : yo[0], yr[0], yo[0] < 0 ? 0 : spen_finder_y[yo[0]].rmin, yo[0] < 0 ? 0 : spen_finder_y[yo[0]].rmax,
                yo[1] < 0 ? 0 : yo[1], yr[1], yo[1] < 0 ? 0 : spen_finder_y[yo[1]].rmin, yo[1] < 0 ? 0 : spen_finder_y[yo[1]].rmax,
                yo[2] < 0 ? 0 : yo[2], yr[2], yo[2] < 0 ? 0 : spen_finder_y[yo[2]].rmin, yo[2] < 0 ? 0 : spen_finder_y[yo[2]].rmax);
        }
        spen_finder_reset();
    }
}
#endif /* DEBUG_SPEN_VERBOSE */

#define RETRO_DEVICE_JOYPAD_MULTITAP ((1 << 8) | RETRO_DEVICE_JOYPAD)
#define RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE ((1 << 8) | RETRO_DEVICE_LIGHTGUN)
#define RETRO_DEVICE_LIGHTGUN_JUSTIFIER ((2 << 8) | RETRO_DEVICE_LIGHTGUN)
#define RETRO_DEVICE_LIGHTGUN_JUSTIFIER_2 ((3 << 8) | RETRO_DEVICE_LIGHTGUN)
#define RETRO_DEVICE_LIGHTGUN_MACS_RIFLE ((4 << 8) | RETRO_DEVICE_LIGHTGUN)

static int g_screen_gun_width = SNES_WIDTH;
static int g_screen_gun_height = SNES_HEIGHT;

#define RETRO_MEMORY_SNES_BSX_RAM ((1 << 8) | RETRO_MEMORY_SAVE_RAM)
#define RETRO_MEMORY_SNES_BSX_PRAM ((2 << 8) | RETRO_MEMORY_SAVE_RAM)
#define RETRO_MEMORY_SNES_SUFAMI_TURBO_A_RAM ((3 << 8) | RETRO_MEMORY_SAVE_RAM)
#define RETRO_MEMORY_SNES_SUFAMI_TURBO_B_RAM ((4 << 8) | RETRO_MEMORY_SAVE_RAM)
#define RETRO_MEMORY_SNES_GAME_BOY_RAM ((5 << 8) | RETRO_MEMORY_SAVE_RAM)
#define RETRO_MEMORY_SNES_GAME_BOY_RTC ((6 << 8) | RETRO_MEMORY_RTC)

#define RETRO_GAME_TYPE_BSX             0x101 | 0x1000
#define RETRO_GAME_TYPE_BSX_SLOTTED     0x102 | 0x1000
#define RETRO_GAME_TYPE_SUFAMI_TURBO    0x103 | 0x1000
#define RETRO_GAME_TYPE_SUPER_GAME_BOY  0x104 | 0x1000
#define RETRO_GAME_TYPE_MULTI_CART      0x105 | 0x1000


#define SNES_4_3 4.0f / 3.0f

uint16 *screen_buffer = NULL;

char g_rom_dir[1024];
char g_basename[1024];

bool g_geometry_update = false;

int hires_blend = 0;
bool randomize_memory = false;
int disabled_channels = 0;

char retro_system_directory[4096];
char retro_save_directory[4096];

retro_log_printf_t log_cb = NULL;
static retro_video_refresh_t video_cb = NULL;
static retro_audio_sample_t audio_cb = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
static retro_input_poll_t poll_cb = NULL;
static retro_input_state_t input_state_cb = NULL;

static bool libretro_supports_option_categories = false;
static bool libretro_supports_bitmasks = false;

/* S-Pen pointer support constants and variables */
/* Settings are defined by enums below */

/* S-Pen action definitions */
#define SNES9X_SPEN_ACTION_DISABLED       0
#define SNES9X_SPEN_ACTION_LEFT_CLICK     1
#define SNES9X_SPEN_ACTION_RIGHT_CLICK    2
#define SNES9X_SPEN_ACTION_MIDDLE_CLICK   3
#define SNES9X_SPEN_ACTION_TRIGGER        4
#define SNES9X_SPEN_ACTION_RELOAD         5

#define SPEN_HOVER_CURSOR          0
#define SPEN_HOVER_ACTIVE_CURSOR   1
#define SPEN_HOVER_LIGHTGUN_TRACK  2
#define SPEN_HOVER_DISABLED        3

static snes_ntsc_t *snes_ntsc = NULL;
static int blargg_filter = 0;
static uint16 *ntsc_screen_buffer, *snes_ntsc_buffer;

const int MAX_SNES_WIDTH_NTSC = ((SNES_NTSC_OUT_WIDTH(256) + 3) / 4) * 4;

static bool show_lightgun_settings = true;
static bool show_advanced_av_settings = true;

static void extract_basename(char *buf, const char *path, size_t size)
{
    const char *base = strrchr(path, '/');
    if (!base)
        base = strrchr(path, '\\');
    if (!base)
        base = path;

    if (*base == '\\' || *base == '/')
        base++;

    strncpy(buf, base, size - 1);
    buf[size - 1] = '\0';

    char *ext = strrchr(buf, '.');
    if (ext)
        *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
    strncpy(buf, path, size - 1);
    buf[size - 1] = '\0';

    char *base = strrchr(buf, '/');
    if (!base)
        base = strrchr(buf, '\\');

    if (base)
        *base = '\0';
    else
        buf[0] = '\0';
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
    video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
    audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
    audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
    poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
    input_state_cb = cb;
}

enum overscan_mode {
    OVERSCAN_CROP_ON,
    OVERSCAN_CROP_OFF,
    OVERSCAN_CROP_12,
    OVERSCAN_CROP_16,
    OVERSCAN_CROP_AUTO
};
enum aspect_mode {
    ASPECT_RATIO_4_3,
    ASPECT_RATIO_4_3_SCALED,
    ASPECT_RATIO_1_1,
    ASPECT_RATIO_NTSC,
    ASPECT_RATIO_PAL,
    ASPECT_RATIO_AUTO
};
static retro_environment_t environ_cb;
static overscan_mode crop_overscan_mode = OVERSCAN_CROP_ON; // default to crop
static aspect_mode aspect_ratio_mode = ASPECT_RATIO_4_3; // default to 4:3
static bool rom_loaded = false;

enum lightgun_mode
{
	SETTING_GUN_INPUT_LIGHTGUN,
	SETTING_GUN_INPUT_POINTER
};
static lightgun_mode setting_gun_input = SETTING_GUN_INPUT_LIGHTGUN;

enum mouse_mode
{
	SETTING_MOUSE_MODE_RELATIVE,
	SETTING_MOUSE_MODE_ABSOLUTE
};
static mouse_mode setting_mouse_mode = SETTING_MOUSE_MODE_RELATIVE;

// Touchscreen sensitivity vars
static int pointer_pressed = 0;
static const int POINTER_PRESSED_CYCLES = 4;
static int pointer_cycles_after_released = 0;
static int pointer_pressed_last_x = 0;
static int pointer_pressed_last_y = 0;

static bool setting_superscope_reverse_buttons = false;

/* S-Pen action and behavior variables */
static int spen_tap_action = SNES9X_SPEN_ACTION_LEFT_CLICK;
static int spen_barrel_action = SNES9X_SPEN_ACTION_RIGHT_CLICK;
static int spen_hover_behavior = SPEN_HOVER_CURSOR;
static int spen_coordinate_mode = 0; /* 0=absolute, 1=relative */
static int spen_input_mode = 0; /* 0=auto, 1=mouse, 2=lightgun */
static int spen_last_rel_x[2] = {0, 0};
static int spen_last_rel_y[2] = {0, 0};
static bool spen_rel_initialized[2] = {false, false};
static double spen_servo_gain = 1.0;   /* S_assumed (units->px) */
static double spen_servo_kp   = 1.0;   /* responsiveness; 1.0 = zero-lag */
static int    spen_servo_deadzone = 2; /* px */
static double spen_est_x[2] = {0.0, 0.0};
static double spen_est_y[2] = {0.0, 0.0};
static bool   spen_servo_initialized[2] = {false, false};

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    static const struct retro_subsystem_memory_info multi_a_memory[] = {
        { "srm", RETRO_MEMORY_SNES_SUFAMI_TURBO_A_RAM },
    };

    static const struct retro_subsystem_memory_info multi_b_memory[] = {
        { "srm", RETRO_MEMORY_SNES_SUFAMI_TURBO_B_RAM },
    };

    static const struct retro_subsystem_rom_info multicart_roms[] = {
        { "Cart A", "smc|sfc|swc|fig|bs|st", false, false, false, multi_a_memory, 1 },
        { "Cart B", "smc|sfc|swc|fig|bs|st", false, false, false, multi_b_memory, 1 },
    };

    static const struct retro_subsystem_info subsystems[] = {
        { "Multi-Cart Link", "multicart_addon", multicart_roms, 2, RETRO_GAME_TYPE_MULTI_CART },
        {}
    };

    cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO,  (void*)subsystems);

    /* An annoyance: retro_set_environment() can be called
     * multiple times, and depending upon the current frontend
     * state various environment callbacks may be disabled.
     * This means the reported 'categories_supported' status
     * may change on subsequent iterations. We therefore have
     * to record whether 'categories_supported' is true on any
     * iteration, and latch the result */
    bool option_categories = false;
    libretro_set_core_options(environ_cb, &option_categories);
    libretro_supports_option_categories |= option_categories;

    /* If frontend supports core option categories,
     * show/hide toggle options are unused and should
     * themselves be hidden */
    if (libretro_supports_option_categories)
    {
        struct retro_core_option_display option_display;

        option_display.visible = false;
        option_display.key     = "snes9x_show_lightgun_settings";

        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY,
                &option_display);

        option_display.key     = "snes9x_show_advanced_av_settings";

        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY,
                &option_display);
    }

    static const struct retro_controller_description port_1[] = {
        { "None", RETRO_DEVICE_NONE },
        { "SNES Joypad", RETRO_DEVICE_JOYPAD },
        { "SNES Mouse", RETRO_DEVICE_MOUSE },
        { "Multitap", RETRO_DEVICE_JOYPAD_MULTITAP },
    };

    static const struct retro_controller_description port_2[] = {
        { "None", RETRO_DEVICE_NONE },
        { "SNES Joypad", RETRO_DEVICE_JOYPAD },
        { "SNES Mouse", RETRO_DEVICE_MOUSE },
        { "Multitap", RETRO_DEVICE_JOYPAD_MULTITAP },
        { "SuperScope", RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE },
        { "Justifier", RETRO_DEVICE_LIGHTGUN_JUSTIFIER },
        { "M.A.C.S. Rifle", RETRO_DEVICE_LIGHTGUN_MACS_RIFLE },
    };

    static const struct retro_controller_description port_3[] = {
        { "None", RETRO_DEVICE_NONE },
        { "SNES Joypad", RETRO_DEVICE_JOYPAD },
        { "Justifier (2P)", RETRO_DEVICE_LIGHTGUN_JUSTIFIER_2 },
    };

    static const struct retro_controller_description port_extra[] = {
        { "None", RETRO_DEVICE_NONE },
        { "SNES Joypad", RETRO_DEVICE_JOYPAD },
    };

    static const struct retro_controller_info ports[] = {
        { port_1, 4 },
        { port_2, 7 },
        { port_3, 3 },
        { port_extra, 2 },
        { port_extra, 2 },
        { port_extra, 2 },
        { port_extra, 2 },
        { port_extra, 2 },
        {},
    };

    environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

char *get_cursor_color(const char *name)
{
	static char color_names[][32] = {
		"Black", "Black",
		"Black (blend)", "tBlack",
		"25% Grey", "25Grey",
		"25% Grey (blend)", "t25Grey",
		"50% Grey", "50Grey",
		"50% Grey (blend)", "t50Grey",
		"75% Grey", "75Grey",
		"75% Grey (blend)", "t75Grey",
		"White", "White",
		"White (blend)", "tWhite",
		"Red", "Red",
		"Red (blend)", "tRed",
		"Orange", "Orange",
		"Orange (blend)", "tOrange",
		"Yellow", "Yellow",
		"Yellow (blend)", "tYellow",
		"Green", "Green",
		"Green (blend)", "tGreen",
		"Cyan", "Cyan",
		"Cyan (blend)", "tCyan",
		"Sky", "Sky",
		"Sky (blend)", "tSky",
		"Blue", "Blue",
		"Blue (blend)", "tBlue",
		"Violet", "Violet",
		"Violet (blend)", "tViolet",
		"Pink", "MagicPink",
		"Pink (blend)", "tMagicPink",
		"Purple", "Purple",
		"Purple (blend)", "tPurple",
		"\0", "\0"
	};

	int lcv = 0;
	while (color_names[lcv][0]) {
		if (strcmp(color_names[lcv], name) == 0) {
			return color_names[lcv+1];
		}

		lcv += 2;
	}

	return color_names[16]; // White
}

// always ensure this is only called in retro_run
void update_geometry(void)
{
    struct retro_system_av_info av_info;
    retro_get_system_av_info(&av_info);
    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
    g_screen_gun_width = av_info.geometry.base_width;
    g_screen_gun_height = av_info.geometry.base_height;
    g_geometry_update = false;
}

static void update_variables(void)
{
    char key[256];
    struct retro_variable var;

    var.key = "snes9x_hires_blend";
    var.value = NULL;

    hires_blend = 0;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        if (!strcmp (var.value, "blur"))
            hires_blend = 1;
        else if (!strcmp (var.value, "merge"))
            hires_blend = 2;
    }

    var.key = "snes9x_overclock_superfx";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        int freq = atoi(var.value);
        Settings.SuperFXClockMultiplier = freq;
    }

    var.key = "snes9x_up_down_allowed";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        Settings.UpAndDown = !strcmp(var.value, "disabled") ? false : true;
    }
    else
        Settings.UpAndDown = false;
    
    strcpy(key, "snes9x_sndchan_volume_x");
    var.key=key;
    for (int i=0;i<8;i++)
    {
        key[strlen("snes9x_sndchan_volume_")]='1'+i;
        var.value=NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
        {
            Settings.ChannelsVolumePercent[i] = atoi(var.value);
        }
    }
    
    int disabled_layers=0;
    strcpy(key, "snes9x_layer_x");
    for (int i=0;i<5;i++)
    {
        key[strlen("snes9x_layer_")]='1'+i;
        var.value=NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && !strcmp("disabled", var.value))
            disabled_layers|=1<<i;
    }
    Settings.BG_Forced=disabled_layers;

    //for some reason, Transparency seems to control both the fixed color and the windowing registers?
    var.key="snes9x_gfx_clip";
    var.value=NULL;
    Settings.DisableGraphicWindows=(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && !strcmp("disabled", var.value));

    var.key="snes9x_gfx_transp";
    var.value=NULL;
    Settings.Transparency=!(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && !strcmp("disabled", var.value));

    var.key="snes9x_audio_interpolation";
    var.value=NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (strcmp(var.value, "gaussian") == 0)
            Settings.InterpolationMethod = DSP_INTERPOLATION_GAUSSIAN;
        else if (strcmp(var.value, "linear") == 0)
            Settings.InterpolationMethod = DSP_INTERPOLATION_LINEAR;
        else if (strcmp(var.value, "cubic") == 0)
            Settings.InterpolationMethod = DSP_INTERPOLATION_CUBIC;
        else if (strcmp(var.value, "sinc") == 0)
            Settings.InterpolationMethod = DSP_INTERPOLATION_SINC;
        else if (strcmp(var.value, "none") == 0)
            Settings.InterpolationMethod = DSP_INTERPOLATION_NONE;
    }
    else
        Settings.InterpolationMethod = DSP_INTERPOLATION_GAUSSIAN;


    Settings.OneClockCycle      = 6;
    Settings.OneSlowClockCycle  = 8;
    Settings.TwoClockCycles     = 12;

    var.key="snes9x_overclock_cycles";
    var.value=NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (strcmp(var.value, "max") == 0)
        {
            Settings.OneClockCycle      = 3;
            Settings.OneSlowClockCycle  = 3;
            Settings.TwoClockCycles     = 3;
        }
        else if (strcmp(var.value, "compatible") == 0)
        {
            Settings.OneClockCycle      = 4;
            Settings.OneSlowClockCycle  = 5;
            Settings.TwoClockCycles     = 6;
        }
        else if (strcmp(var.value, "light") == 0)
        {
            Settings.OneClockCycle      = 6;
            Settings.OneSlowClockCycle  = 6;
            Settings.TwoClockCycles     = 12;
        }
    }

    Settings.MaxSpriteTilesPerLine = 34;
    var.key="snes9x_reduce_sprite_flicker";
    var.value=NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        if (strcmp(var.value, "enabled") == 0)
            Settings.MaxSpriteTilesPerLine = 128;

    randomize_memory = false;
    var.key = "snes9x_randomize_memory";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        if (strcmp(var.value, "enabled") == 0)
            randomize_memory = true;

    var.key = "snes9x_overscan";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        overscan_mode newval = OVERSCAN_CROP_AUTO;
        if (strcmp(var.value, "enabled") == 0)
            newval = OVERSCAN_CROP_ON;
        else if (strcmp(var.value, "12_pixels") == 0)
            newval = OVERSCAN_CROP_12;
        else if (strcmp(var.value, "16_pixels") == 0)
            newval = OVERSCAN_CROP_16;
        else if (strcmp(var.value, "disabled") == 0)
            newval = OVERSCAN_CROP_OFF;

        if (newval != crop_overscan_mode)
        {
            crop_overscan_mode = newval;
            g_geometry_update = true;
        }
    }

    var.key = "snes9x_aspect";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        aspect_mode newval = ASPECT_RATIO_AUTO;
        if (strcmp(var.value, "ntsc") == 0)
            newval = ASPECT_RATIO_NTSC;
        else if (strcmp(var.value, "pal") == 0)
            newval = ASPECT_RATIO_PAL;
        else if (strcmp(var.value, "4:3") == 0)
            newval = ASPECT_RATIO_4_3;
        else if (strcmp(var.value, "4:3 scaled") == 0)
            newval = ASPECT_RATIO_4_3_SCALED;
        else if (strcmp(var.value, "uncorrected") == 0)
            newval = ASPECT_RATIO_1_1;

        if (newval != aspect_ratio_mode)
        {
            aspect_ratio_mode = newval;
            g_geometry_update = true;
        }
    }

    var.key = "snes9x_region";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "auto"))
        {
            Settings.ForceNTSC = false;
            Settings.ForcePAL = false;
        }
        else if (!strcmp(var.value, "ntsc"))
        {
            Settings.ForceNTSC = true;
            Settings.ForcePAL = false;
        }
        else if (!strcmp(var.value, "pal"))
        {
            Settings.ForceNTSC = false;
            Settings.ForcePAL = true;
        }
    }

    var.key="snes9x_lightgun_mode";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
      if (var.value && (!strcmp(var.value, "Absolute") || !strcmp(var.value, "Touchscreen"))) {
         setting_gun_input = SETTING_GUN_INPUT_POINTER;
      } else {
         setting_gun_input = SETTING_GUN_INPUT_LIGHTGUN;
      }
    }

    var.key="snes9x_mouse_mode";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
      if (var.value && !strcmp(var.value, "Absolute")) {
         setting_mouse_mode = SETTING_MOUSE_MODE_ABSOLUTE;
      } else {
         setting_mouse_mode = SETTING_MOUSE_MODE_RELATIVE;
      }
    }

    var.key="snes9x_superscope_reverse_buttons";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        setting_superscope_reverse_buttons = strcmp(var.value, "enabled") == 0;
    }

    /* Parse S-Pen tap action */
    var.key="snes9x_spen_tap_action";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "left_click"))
            spen_tap_action = SNES9X_SPEN_ACTION_LEFT_CLICK;
        else if (!strcmp(var.value, "right_click"))
            spen_tap_action = SNES9X_SPEN_ACTION_RIGHT_CLICK;
        else if (!strcmp(var.value, "middle_click"))
            spen_tap_action = SNES9X_SPEN_ACTION_MIDDLE_CLICK;
        else if (!strcmp(var.value, "trigger"))
            spen_tap_action = SNES9X_SPEN_ACTION_TRIGGER;
        else if (!strcmp(var.value, "reload"))
            spen_tap_action = SNES9X_SPEN_ACTION_RELOAD;
        else if (!strcmp(var.value, "disabled"))
            spen_tap_action = SNES9X_SPEN_ACTION_DISABLED;
    }

    /* Parse S-Pen barrel action */
    var.key="snes9x_spen_barrel_action";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "left_click"))
            spen_barrel_action = SNES9X_SPEN_ACTION_LEFT_CLICK;
        else if (!strcmp(var.value, "right_click"))
            spen_barrel_action = SNES9X_SPEN_ACTION_RIGHT_CLICK;
        else if (!strcmp(var.value, "middle_click"))
            spen_barrel_action = SNES9X_SPEN_ACTION_MIDDLE_CLICK;
        else if (!strcmp(var.value, "trigger"))
            spen_barrel_action = SNES9X_SPEN_ACTION_TRIGGER;
        else if (!strcmp(var.value, "reload"))
            spen_barrel_action = SNES9X_SPEN_ACTION_RELOAD;
        else if (!strcmp(var.value, "disabled"))
            spen_barrel_action = SNES9X_SPEN_ACTION_DISABLED;
    }

    /* Parse S-Pen hover behavior */
    var.key="snes9x_spen_hover_behavior";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "cursor"))
            spen_hover_behavior = SPEN_HOVER_CURSOR;
        else if (!strcmp(var.value, "active_cursor"))
            spen_hover_behavior = SPEN_HOVER_ACTIVE_CURSOR;
        else if (!strcmp(var.value, "lightgun_tracking"))
            spen_hover_behavior = SPEN_HOVER_LIGHTGUN_TRACK;
        else if (!strcmp(var.value, "disabled"))
            spen_hover_behavior = SPEN_HOVER_DISABLED;
    }

    /* Parse S-Pen coordinate mode */
    var.key="snes9x_spen_coordinate_mode";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "absolute"))
            spen_coordinate_mode = 0;
        else if (!strcmp(var.value, "relative"))
            spen_coordinate_mode = 1;
    }

    /* Parse S-Pen input mode */
    var.key="snes9x_spen_input_mode";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "auto"))
            spen_input_mode = 0;
        else if (!strcmp(var.value, "mouse"))
            spen_input_mode = 1;
        else if (!strcmp(var.value, "lightgun"))
            spen_input_mode = 2;
    }

    /* Parse S-Pen servo parameters */
    var.key = "snes9x_spen_servo_gain";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        spen_servo_gain = atof(var.value);

    var.key = "snes9x_spen_servo_responsiveness";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        spen_servo_kp = atof(var.value);

    var.key = "snes9x_spen_servo_deadzone";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        spen_servo_deadzone = atoi(var.value);

    /* Log S-Pen configuration for hardware testing */
    #ifdef DEBUG_SPEN_VERBOSE
    if (log_cb) {
        const char* tap_actions[] = {"DISABLED", "LEFT_CLICK", "RIGHT_CLICK", "MIDDLE_CLICK", "TRIGGER", "RELOAD"};
        const char* barrel_actions[] = {"DISABLED", "LEFT_CLICK", "RIGHT_CLICK", "MIDDLE_CLICK", "TRIGGER", "RELOAD"};
        const char* hover_behaviors[] = {"CURSOR", "ACTIVE_CURSOR", "LIGHTGUN_TRACK", "DISABLED"};
        const char* coord_modes[] = {"ABSOLUTE", "RELATIVE"};
        const char* input_modes[] = {"AUTO", "MOUSE", "LIGHTGUN"};

        log_cb(RETRO_LOG_INFO, "[SNES9X S-Pen VERBOSE] === S-PEN CONFIGURATION LOADED ===\n");
        log_cb(RETRO_LOG_INFO, "[SNES9X S-Pen VERBOSE] Tap Action: %s (%d)\n",
               (spen_tap_action <= 5) ? tap_actions[spen_tap_action] : "UNKNOWN", spen_tap_action);
        log_cb(RETRO_LOG_INFO, "[SNES9X S-Pen VERBOSE] Barrel Action: %s (%d)\n",
               (spen_barrel_action <= 5) ? barrel_actions[spen_barrel_action] : "UNKNOWN", spen_barrel_action);
        log_cb(RETRO_LOG_INFO, "[SNES9X S-Pen VERBOSE] Hover Behavior: %s (%d)\n",
               (spen_hover_behavior <= 3) ? hover_behaviors[spen_hover_behavior] : "UNKNOWN", spen_hover_behavior);
        log_cb(RETRO_LOG_INFO, "[SNES9X S-Pen VERBOSE] Coordinate Mode: %s (%d)\n",
               (spen_coordinate_mode <= 1) ? coord_modes[spen_coordinate_mode] : "UNKNOWN", spen_coordinate_mode);
        log_cb(RETRO_LOG_INFO, "[SNES9X S-Pen VERBOSE] Input Mode: %s (%d)\n",
               (spen_input_mode <= 2) ? input_modes[spen_input_mode] : "UNKNOWN", spen_input_mode);
        log_cb(RETRO_LOG_INFO, "[SNES9X S-Pen VERBOSE] === END S-PEN CONFIGURATION ===\n");
    }
    #endif

    var.key="snes9x_superscope_crosshair";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        int crosshair;

        sscanf (var.value,"%d",&crosshair);
        S9xSetControllerCrosshair(X_SUPERSCOPE, crosshair, 0, 0);
    }

    var.key="snes9x_superscope_color";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        char *color = get_cursor_color(var.value);

        if (color[0] == 't')
            S9xSetControllerCrosshair(X_SUPERSCOPE, -1, get_cursor_color(var.value), "tBlack");
        else
            S9xSetControllerCrosshair(X_SUPERSCOPE, -1, get_cursor_color(var.value), "Black");
    }

    var.key="snes9x_justifier1_crosshair";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        int crosshair;

        sscanf (var.value,"%d",&crosshair);
        S9xSetControllerCrosshair(X_JUSTIFIER1, crosshair, 0, 0);
    }

    var.key="snes9x_justifier1_color";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        char *color = get_cursor_color(var.value);

        if (color[0] == 't')
            S9xSetControllerCrosshair(X_JUSTIFIER1, -1, get_cursor_color(var.value), "tBlack");
        else
            S9xSetControllerCrosshair(X_JUSTIFIER1, -1, get_cursor_color(var.value), "Black");
    }

    var.key="snes9x_justifier2_crosshair";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        int crosshair;

        sscanf (var.value,"%d",&crosshair);
        S9xSetControllerCrosshair(X_JUSTIFIER2, crosshair, 0, 0);
    }

    var.key="snes9x_justifier2_color";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        char *color = get_cursor_color(var.value);

        if (color[0] == 't')
            S9xSetControllerCrosshair(X_JUSTIFIER2, -1, get_cursor_color(var.value), "tBlack");
        else
            S9xSetControllerCrosshair(X_JUSTIFIER2, -1, get_cursor_color(var.value), "Black");
    }

    var.key="snes9x_rifle_crosshair";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        int crosshair;

        sscanf (var.value,"%d",&crosshair);
        S9xSetControllerCrosshair(X_MACSRIFLE, crosshair, 0, 0);
    }

    var.key="snes9x_rifle_color";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        char *color = get_cursor_color(var.value);

        if (color[0] == 't')
            S9xSetControllerCrosshair(X_MACSRIFLE, -1, get_cursor_color(var.value), "tBlack");
        else
            S9xSetControllerCrosshair(X_MACSRIFLE, -1, get_cursor_color(var.value), "Black");
    }

    var.key = "snes9x_block_invalid_vram_access";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
        Settings.BlockInvalidVRAMAccessMaster = !strcmp(var.value, "disabled") ? false : true;
    else
        Settings.BlockInvalidVRAMAccessMaster = true;

    var.key = "snes9x_echo_buffer_hack";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
        Settings.SeparateEchoBuffer = !strcmp(var.value, "disabled") ? false : true;
    else
        Settings.SeparateEchoBuffer = false;

    var.key = "snes9x_blargg";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
    {
        if (strcmp(var.value, "disabled") == 0)
            blargg_filter = 0;
        else
        {
            int old_filter = blargg_filter;

            if(!snes_ntsc) snes_ntsc = new snes_ntsc_t;
            snes_ntsc_setup_t setup = snes_ntsc_composite;

            if (strcmp(var.value, "monochrome") == 0)
            {
                blargg_filter = 1;

                setup = snes_ntsc_monochrome;
            }
            else if (strcmp(var.value, "rf") == 0)
            {
                blargg_filter = 2;

                setup = snes_ntsc_composite;
                setup.merge_fields = 0;
            }
            else if (strcmp(var.value, "composite") == 0)
            {
                blargg_filter = 3;

                setup = snes_ntsc_composite;
            }
            else if (strcmp(var.value, "s-video") == 0)
            {
                blargg_filter = 4;

                setup = snes_ntsc_svideo;
            }
            else if (strcmp(var.value, "rgb") == 0)
            {
                blargg_filter = 5;

                setup = snes_ntsc_rgb;
            }

            if (old_filter != blargg_filter)
                snes_ntsc_init( snes_ntsc, &setup );
        }
    }

    /* Show/hide core options
     * > If frontend supports core option categories,
     *   then show/hide toggle options are ignored,
     *   and no other options should be hidden */

    var.key = "snes9x_show_lightgun_settings";
    var.value = NULL;

    if (!libretro_supports_option_categories &&
        environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        bool show_lightgun_settings_prev = show_lightgun_settings;

        show_lightgun_settings = true;
        if (strcmp(var.value, "disabled") == 0)
            show_lightgun_settings = false;

        if (show_lightgun_settings != show_lightgun_settings_prev)
        {
            size_t i;
            struct retro_core_option_display option_display;
            char lightgun_keys[10][64] = {
                "snes9x_lightgun_mode",
                "snes9x_superscope_reverse_buttons",
                "snes9x_superscope_crosshair",
                "snes9x_superscope_color",
                "snes9x_justifier1_crosshair",
                "snes9x_justifier1_color",
                "snes9x_justifier2_crosshair",
                "snes9x_justifier2_color",
                "snes9x_rifle_crosshair",
                "snes9x_rifle_color"
            };

            option_display.visible = show_lightgun_settings;

            for (i = 0; i < 10; i++)
            {
                option_display.key = lightgun_keys[i];
                environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
            }
        }
    }

    var.key = "snes9x_show_advanced_av_settings";
    var.value = NULL;

    if (!libretro_supports_option_categories &&
        environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        bool show_advanced_av_settings_prev = show_advanced_av_settings;

        show_advanced_av_settings = true;
        if (strcmp(var.value, "disabled") == 0)
            show_advanced_av_settings = false;

        if (show_advanced_av_settings != show_advanced_av_settings_prev)
        {
            size_t i;
            struct retro_core_option_display option_display;
            char av_keys[15][32] = {
                "snes9x_layer_1",
                "snes9x_layer_2",
                "snes9x_layer_3",
                "snes9x_layer_4",
                "snes9x_layer_5",
                "snes9x_gfx_clip",
                "snes9x_gfx_transp",
                "snes9x_sndchan_1",
                "snes9x_sndchan_2",
                "snes9x_sndchan_3",
                "snes9x_sndchan_4",
                "snes9x_sndchan_5",
                "snes9x_sndchan_6",
                "snes9x_sndchan_7",
                "snes9x_sndchan_8"
            };

            option_display.visible = show_advanced_av_settings;

            for (i = 0; i < 15; i++)
            {
                option_display.key = av_keys[i];
                environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
            }
        }
    }
}

void S9xSyncSpeed() {

    if (Settings.Mute) {
        S9xClearSamples();
        return;
    }

    static std::vector<int16_t> audio_buffer;

    size_t avail = S9xGetSampleCount();

    if (audio_buffer.size() < avail)
        audio_buffer.resize(avail);

    S9xMixSamples((uint8*)&audio_buffer[0], avail);
    audio_batch_cb(&audio_buffer[0], avail >> 1);
}

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info,0,sizeof(retro_system_info));

    info->library_name = "Snes9x";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
    info->library_version = VERSION GIT_VERSION;
    info->valid_extensions = "smc|sfc|swc|fig|bs|st";
    info->need_fullpath = false;
    info->block_extract = false;
}

float get_aspect_ratio(unsigned width, unsigned height)
{
    if (aspect_ratio_mode == ASPECT_RATIO_4_3)
    {
        return SNES_4_3;
    }
    else if (aspect_ratio_mode == ASPECT_RATIO_4_3_SCALED)
    {
        return (4.0f * (MAX_SNES_HEIGHT - height)) / (3.0f * (MAX_SNES_WIDTH - width));
    }
    else if (aspect_ratio_mode == ASPECT_RATIO_1_1)
    {
        return (float) width / (float) height;
    }

    // OV2: not sure if these really make sense - NTSC is similar to 4:3, PAL looks weird
    double sample_frequency_ntsc = 135000000.0f / 11.0f;
    double sample_frequency_pal = 14750000.0;

    double sample_freq = retro_get_region() == RETRO_REGION_NTSC ? sample_frequency_ntsc : sample_frequency_pal;
    double dot_rate = (Settings.PAL ? PAL_MASTER_CLOCK : NTSC_MASTER_CLOCK) / 4.0;

    if (aspect_ratio_mode == ASPECT_RATIO_NTSC) // ntsc
    {
        sample_freq = sample_frequency_ntsc;
        dot_rate = NTSC_MASTER_CLOCK / 4.0;
    }
    else if (aspect_ratio_mode == ASPECT_RATIO_PAL) // pal
    {
        sample_freq = sample_frequency_pal;
        dot_rate = PAL_MASTER_CLOCK / 4.0;
    }

    double par = sample_freq / 2.0 / dot_rate;
    return (float)(width * par / height);
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info,0,sizeof(retro_system_av_info));
    unsigned width = SNES_WIDTH;
    unsigned height = PPU.ScreenHeight;
    if (crop_overscan_mode == OVERSCAN_CROP_ON)
        height = SNES_HEIGHT;
    else if (crop_overscan_mode == OVERSCAN_CROP_12)
        height = 216;
    else if (crop_overscan_mode == OVERSCAN_CROP_16)
        height = 208;
    else if (crop_overscan_mode == OVERSCAN_CROP_OFF)
        height = SNES_HEIGHT_EXTENDED;

    info->geometry.base_width = width;
    info->geometry.base_height = height;
    info->geometry.max_width = MAX_SNES_WIDTH_NTSC;
    info->geometry.max_height = MAX_SNES_HEIGHT;
    info->geometry.aspect_ratio = get_aspect_ratio(width, height);
    info->timing.sample_rate = 32040;
    info->timing.fps = retro_get_region() == RETRO_REGION_NTSC ? 21477272.0 / 357366.0 : 21281370.0 / 425568.0;

    g_screen_gun_width = width;
    g_screen_gun_height = height;
}

unsigned retro_api_version()
{
    return RETRO_API_VERSION;
}


void retro_reset()
{
    S9xSoftReset();
}

static unsigned snes_devices[8];
void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if (port < 8)
    {
        int offset = snes_devices[0] == RETRO_DEVICE_JOYPAD_MULTITAP ? 4 : 1;
        switch (device)
        {
            case RETRO_DEVICE_JOYPAD:
                S9xSetController(port, CTL_JOYPAD, port * offset, 0, 0, 0);
                snes_devices[port] = RETRO_DEVICE_JOYPAD;
                break;
            case RETRO_DEVICE_JOYPAD_MULTITAP:
                S9xSetController(port, CTL_MP5, port * offset, port * offset + 1, port * offset + 2, port * offset + 3);
                snes_devices[port] = RETRO_DEVICE_JOYPAD_MULTITAP;
                break;
            case RETRO_DEVICE_MOUSE:
                S9xSetController(port, CTL_MOUSE, port, 0, 0, 0);
                snes_devices[port] = RETRO_DEVICE_MOUSE;
                break;
            case RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE:
                S9xSetController(port, CTL_SUPERSCOPE, 0, 0, 0, 0);
                snes_devices[port] = RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE;
                break;
            case RETRO_DEVICE_LIGHTGUN_JUSTIFIER:
                S9xSetController(port, CTL_JUSTIFIER, 0, 0, 0, 0);
                snes_devices[port] = RETRO_DEVICE_LIGHTGUN_JUSTIFIER;
                break;
            case RETRO_DEVICE_LIGHTGUN_JUSTIFIER_2:
            	if ( port == 2 )
            	{
					S9xSetController(1, CTL_JUSTIFIER, 1, 0, 0, 0);
                	snes_devices[port] = RETRO_DEVICE_LIGHTGUN_JUSTIFIER_2;
				}
				else
				{
					if (log_cb)
						log_cb(RETRO_LOG_ERROR, "Invalid Justifier (2P) assignment to port %d, must be port 2.\n", port);
					S9xSetController(port, CTL_NONE, 0, 0, 0, 0);
					snes_devices[port] = RETRO_DEVICE_NONE;
				}
                break;
            case RETRO_DEVICE_LIGHTGUN_MACS_RIFLE:
                S9xSetController(port, CTL_MACSRIFLE, 0, 0, 0, 0);
                snes_devices[port] = RETRO_DEVICE_LIGHTGUN_MACS_RIFLE;
                break;
            case RETRO_DEVICE_NONE:
                S9xSetController(port, CTL_NONE, 0, 0, 0, 0);
                snes_devices[port] = RETRO_DEVICE_NONE;
                break;
            default:
                if (log_cb)
                    log_cb(RETRO_LOG_ERROR, "Invalid device (%d).\n", device);
                break;
        }

        S9xControlsSoftReset();
    }
    else if(device != RETRO_DEVICE_NONE)
        log_cb(RETRO_LOG_INFO, "Nonexistent Port (%d).\n", port);
}

void retro_cheat_reset()
{
    S9xDeleteCheats();
}

void retro_cheat_set(unsigned index, bool enabled, const char *codeline)
{
    char codeCopy[256];
    char* code;

    if (codeline == (char *)'\0') return;

    strcpy(codeCopy,codeline);
    code=strtok(codeCopy,"+,.; ");
    while (code != NULL) {
        //Convert GH RAW to PAR
        if (strlen(code)==9 && code[6]==':')
        {
            code[6]=code[7];
            code[7]=code[8];
            code[8]='\0';
        }

        /* Goldfinger was broken and nobody noticed. Removed */
        if (S9xAddCheatGroup (std::string("retro"), std::string(code)) >= 0)
        {
            if (enabled)
                S9xEnableCheatGroup (Cheat.group.size () - 1);
        }
        else
        {
            printf("CHEAT: Failed to recognize %s\n",code);
        }

        code=strtok(NULL,"+,.; "); // bad code, ignore
    }

    S9xCheatsEnable();
}

static void init_descriptors(void)
{
    struct retro_input_descriptor desc[] = {
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,		"D-Pad Up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,		"B" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,		"A" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,		"X" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,		"Y" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,		"L" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,		"R" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,	"Select" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,		"Start" },

        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,		"D-Pad Up" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,		"B" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,		"A" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,		"X" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,		"Y" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,		"L" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,		"R" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,	"Select" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,		"Start" },

        { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
        { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,		"D-Pad Up" },
        { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
        { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
        { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,		"B" },
        { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,		"A" },
        { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,		"X" },
        { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,		"Y" },
        { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,		"L" },
        { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,		"R" },
        { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,	"Select" },
        { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,		"Start" },

        { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
        { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,		"D-Pad Up" },
        { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
        { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
        { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,		"B" },
        { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,		"A" },
        { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,		"X" },
        { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,		"Y" },
        { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,		"L" },
        { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,		"R" },
        { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,	"Select" },
        { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,		"Start" },

        { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
        { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,		"D-Pad Up" },
        { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
        { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
        { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,		"B" },
        { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,		"A" },
        { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,		"X" },
        { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,		"Y" },
        { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,		"L" },
        { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,		"R" },
        { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,	"Select" },
        { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,		"Start" },
	    
        { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
        { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,		"D-Pad Up" },
        { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
        { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
        { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,		"B" },
        { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,		"A" },
        { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,		"X" },
        { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,		"Y" },
        { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,		"L" },
        { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,		"R" },
        { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,	"Select" },
        { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,		"Start" },
	    
        { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
        { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,		"D-Pad Up" },
        { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
        { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
        { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,		"B" },
        { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,		"A" },
        { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,		"X" },
        { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,		"Y" },
        { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,		"L" },
        { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,		"R" },
        { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,	"Select" },
        { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,		"Start" },

        { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
        { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,		"D-Pad Up" },
        { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
        { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
        { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,		"B" },
        { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,		"A" },
        { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,		"X" },
        { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,		"Y" },
        { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,		"L" },
        { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,		"R" },
        { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,	"Select" },
        { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,		"Start" },
	    
        { 0, 0, 0, 0, NULL },
    };

    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

static bool valid_normal_bank (uint8 bankbyte)
{
    switch (bankbyte)
    {
        case 32: case 33: case 48: case 49:
            return (true);
    }

    return (false);
}

static int is_bsx (uint8 *p)
{
    if ((p[26] == 0x33 || p[26] == 0xFF) && (!p[21] || (p[21] & 131) == 128) && valid_normal_bank(p[24]))
    {
        unsigned char	m = p[22];

        if (!m && !p[23])
            return (2);

        if ((m == 0xFF && p[23] == 0xFF) || (!(m & 0xF) && ((m >> 4) - 1 < 12)))
            return (1);
    }

    return (0);
}

static bool8 LoadBIOS(uint8 *biosrom, const char *biosname, int biossize)
{
    FILE	*fp;
    char	name[PATH_MAX + 1];
    bool8 r = FALSE;

    strcpy(name, S9xGetDirectory(ROMFILENAME_DIR).c_str());
    strcat(name, SLASH_STR);
    strcat(name, biosname);

    fp = fopen(name, "rb");
    if (!fp)
    {
        strcpy(name, S9xGetDirectory(BIOS_DIR).c_str());
        strcat(name, SLASH_STR);
        strcat(name, biosname);

        fp = fopen(name, "rb");
    }

    if (fp)
    {
        size_t size;

        size = fread((void *) biosrom, 1, biossize, fp);
        fclose(fp);
        if (size == (unsigned int) biossize)
            r = TRUE;
    }

    return (r);
}

static bool8 is_SufamiTurbo_Cart (const uint8 *data, uint32 size)
{
    if (size >= 0x80000 && size <= 0x100000 &&
        strncmp((char *) data, "BANDAI SFC-ADX", 14) == 0 && strncmp((char * ) (data + 0x10), "SFC-ADX BACKUP", 14) != 0)
        return (TRUE);
    else
        return (FALSE);
}

bool retro_load_game(const struct retro_game_info *game)
{
    init_descriptors();

    update_variables();

    /* Re-evaluate the S-Pen RAM-cursor profile for every load (game may have changed). */
    spen_ram_cursor_enabled = false;
    spen_ram_cur_x_addr     = 0;
    spen_ram_cur_y_addr     = 0;

    if(game->data == NULL && game->size == 0 && game->path != NULL)
        rom_loaded = Memory.LoadROM(game->path);
    else
    {
        uint8 *biosrom = new uint8[0x100000];

        if (game->path != NULL)
        {
            extract_basename(g_basename, game->path, sizeof(g_basename));
            extract_directory(g_rom_dir, game->path, sizeof(g_rom_dir));
        }

        if (is_SufamiTurbo_Cart((uint8 *) game->data, game->size)) {
            if ((rom_loaded = LoadBIOS(biosrom,"STBIOS.bin",0x40000)))
            rom_loaded = Memory.LoadMultiCartMem((const uint8_t*)game->data, game->size, 0, 0, biosrom, 0x40000);
        }

        else
        if ((is_bsx((uint8 *) game->data + 0x7fc0)==1) | (is_bsx((uint8 *) game->data + 0xffc0)==1)) {
            if ((rom_loaded = LoadBIOS(biosrom,"BS-X.bin",0x100000)))
            rom_loaded = Memory.LoadMultiCartMem(biosrom, 0x100000, (const uint8_t*)game->data, game->size, 0, 0);
        }

        else
            rom_loaded = Memory.LoadROMMem((const uint8_t*)game->data ,game->size, g_basename);

        if(biosrom) delete[] biosrom;
    }

    if (rom_loaded)
    {
        /* If we're in RGB565 format, switch frontend to that */
        if (RED_SHIFT_BITS == 11)
        {
            enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
            if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
            {
                return false;
            }
        }

        g_geometry_update = true;

        if (randomize_memory)
        {
            srand(time(NULL));
            for(int lcv = 0; lcv < sizeof(Memory.RAM); lcv++)
                Memory.RAM[lcv] = rand() % 256;
        }

        /* Resolve the per-game S-Pen RAM-cursor profile now that Memory.ROMName /
           Memory.ROMCRC32 are valid. Match on internal-title prefix (case/space-padded).
           Pick the LONGEST matching title so a specific game ("LEMMINGS 2") is not
           shadowed by a shorter prefix of another ("LEMMINGS"), independent of table order. */
        size_t best_len = 0;
        for (size_t i = 0; i < sizeof(SPEN_MOUSE_PROFILES) / sizeof(SPEN_MOUSE_PROFILES[0]); i++)
        {
            const spen_mouse_profile_t *p = &SPEN_MOUSE_PROFILES[i];
            size_t tlen = strlen(p->title);
            if (tlen > best_len && strncmp(Memory.ROMName, p->title, tlen) == 0)
            {
                best_len                = tlen;
                spen_ram_cursor_enabled = true;
                spen_ram_cur_x_addr     = p->cur_x_addr;
                spen_ram_cur_y_addr     = p->cur_y_addr;
            }
        }

        if (log_cb)
            log_cb(RETRO_LOG_INFO, "[SPEN-PROFILE] ROM='%.21s' CRC=%08X -> RAM-cursor %s (x=$%04X y=$%04X)\n",
                   Memory.ROMName, (unsigned)Memory.ROMCRC32,
                   spen_ram_cursor_enabled ? "ON" : "off(fallback)", spen_ram_cur_x_addr, spen_ram_cur_y_addr);
    }

    if (!rom_loaded && log_cb)
        log_cb(RETRO_LOG_ERROR, "ROM loading failed...\n");

    Memory.ClearSRAM();

    return rom_loaded;
}

static void remove_header(uint8_t *&romptr, size_t &romsize, bool multicart_sufami)
{
    if (romptr==0 || romsize==0) return;

    uint32 calc_size = (romsize / 0x2000) * 0x2000;
    if ((romsize - calc_size == 512 && !Settings.ForceNoHeader) || Settings.ForceHeader)
    {
        romptr += 512;
        romsize -= 512;

        if(log_cb) log_cb(RETRO_LOG_INFO,"ROM header removed\n");
    }

    if (multicart_sufami && (romptr + romsize) >= (romptr + 0x100000))
    {
        if (strncmp((const char*)(romptr + 0x100000), "BANDAI SFC-ADX", 14) == 0 &&
            strncmp((const char*)(romptr + 0x000000), "BANDAI SFC-ADX", 14) == 0)
        {
            romptr += 0x100000;
            romsize -= 0x100000;

            if(log_cb) log_cb(RETRO_LOG_INFO,"Sufami Turbo Multi-ROM bios removed\n");
        }
    }
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
    uint8_t *romptr[3];
    size_t romsize[3];

    for(size_t i=0; i < num_info; i++)
    {
        romptr[i] = (uint8_t *) info[i].data;
        romsize[i] = info[i].size;
        remove_header(romptr[i], romsize[i], true);
    }

    init_descriptors();
    rom_loaded = false;

    update_variables();
    switch (game_type)
    {
        case RETRO_GAME_TYPE_BSX:
            if(num_info == 1)
            {
              rom_loaded = Memory.LoadROMMem((const uint8_t*)romptr[0],romsize[0]);
            }
            else if(num_info == 2)
            {
                memcpy(Memory.BIOSROM,(const uint8_t*)romptr[0],info[0].size);
                rom_loaded = Memory.LoadROMMem((const uint8_t*)romptr[1],info[1].size);
            }

            if (!rom_loaded && log_cb)
                log_cb(RETRO_LOG_ERROR, "BSX ROM loading failed...\n");
            break;
        case RETRO_GAME_TYPE_BSX_SLOTTED:
        case RETRO_GAME_TYPE_MULTI_CART:
            if(num_info == 2)
            {
                if (is_SufamiTurbo_Cart((const uint8_t*)romptr[0], romsize[0]))
                {
                    log_cb(RETRO_LOG_ERROR, "Cart is Sufami Turbo...\n");
                    uint8 *biosrom = new uint8[0x40000];
                    uint8 *biosptr = biosrom;

                    if (LoadBIOS(biosptr, "STBIOS.bin", 0x40000))
                    {
                        if (log_cb)
                            log_cb(RETRO_LOG_INFO, "Loading Sufami Turbo link game\n");

                        rom_loaded = Memory.LoadMultiCartMem((const uint8_t*)romptr[0], romsize[0],
                               (const uint8_t*)romptr[1], romsize[1], biosptr, 0x40000);
                    }

                if (biosrom)
                    delete[] biosrom;
                }
                else
                {
                    if (log_cb)
                        log_cb(RETRO_LOG_INFO, "Loading Multi-Cart link game\n");

                    rom_loaded = Memory.LoadMultiCartMem((const uint8_t*)romptr[0], romsize[0],
                        (const uint8_t*)romptr[1], romsize[1], NULL, 0);
                }
            }

            if (!rom_loaded && log_cb)
                log_cb(RETRO_LOG_ERROR, "Multirom loading failed...\n");
            break;

        case RETRO_GAME_TYPE_SUFAMI_TURBO:
            if(num_info == 2)
            {
                uint8 *biosrom = new uint8[0x100000];

                if ((rom_loaded = LoadBIOS(biosrom,"STBIOS.bin",0x100000)))
                    rom_loaded = Memory.LoadMultiCartMem((const uint8_t*)romptr[0], romsize[0],
                        (const uint8_t*)romptr[1], romsize[1], biosrom, 0x40000);

                if (biosrom)
                    delete[] biosrom;
            }

            if (!rom_loaded && log_cb)
                log_cb(RETRO_LOG_ERROR, "Sufami Turbo ROM loading failed...\n");
            break;

        default:
            rom_loaded = false;
            log_cb(RETRO_LOG_ERROR, "Multi-cart ROM loading failed...\n");
            break;
    }

    if (rom_loaded)
    {
        if(RED_SHIFT_BITS == 11)
        {
            enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
            if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
                return false;
        }

        g_geometry_update = true;
    }

    return rom_loaded;
}

void retro_unload_game(void)
{}

static void map_buttons();

static void check_system_specs(void)
{
    /* TODO - might have to variably set performance level based on SuperFX/SA-1/etc */
    unsigned level = 12;
    environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_init(void)
{
    struct retro_log_callback log;

    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
        log_cb = log.log;
    else
        log_cb = NULL;

    const char *dir = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
        snprintf(retro_system_directory, sizeof(retro_system_directory), "%s", dir);
    else
        snprintf(retro_system_directory, sizeof(retro_system_directory), "%s", ".");

    if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
        snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", dir);
    else
        snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", ".");

    // State that SNES9X supports achievements.
    bool achievements = true;
    environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);

    memset(&Settings, 0, sizeof(Settings));
    Settings.MouseMaster = TRUE;
    Settings.SuperScopeMaster = TRUE;
    Settings.JustifierMaster = TRUE;
    Settings.MultiPlayer5Master = TRUE;
    Settings.MacsRifleMaster = TRUE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.SixteenBitSound = TRUE;
    Settings.Stereo = TRUE;
    Settings.SoundPlaybackRate = 32040;
    Settings.SoundInputRate = 32040;
    Settings.Transparency = TRUE;
    Settings.AutoDisplayMessages = TRUE;
    Settings.InitialInfoStringTimeout = 120;
    Settings.HDMATimingHack = 100;
    Settings.BlockInvalidVRAMAccessMaster = TRUE;
    Settings.SeparateEchoBuffer = FALSE;
    Settings.CartAName[0] = 0;
    Settings.CartBName[0] = 0;
    Settings.AutoSaveDelay = 1;
    Settings.DontSaveOopsSnapshot = TRUE;
    Settings.ChannelsVolumePercent[0] = 100;
    Settings.ChannelsVolumePercent[1] = 100;
    Settings.ChannelsVolumePercent[2] = 100;
    Settings.ChannelsVolumePercent[3] = 100;
    Settings.ChannelsVolumePercent[4] = 100;
    Settings.ChannelsVolumePercent[5] = 100;
    Settings.ChannelsVolumePercent[6] = 100;
    Settings.ChannelsVolumePercent[7] = 100;
    Settings.ChannelsVolumePercent[8] = 100;

    CPU.Flags = 0;

    if (!Memory.Init() || !S9xInitAPU())
    {
        Memory.Deinit();
        S9xDeinitAPU();

        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "Failed to init Memory or APU.\n");
        exit(1);
    }

    S9xInitSound(32);

    S9xSetSoundMute(FALSE);
    S9xSetSamplesAvailableCallback(NULL, NULL);

    ntsc_screen_buffer = (uint16*) calloc(1, MAX_SNES_WIDTH_NTSC * 2 * (MAX_SNES_HEIGHT + 16));
    snes_ntsc_buffer = ntsc_screen_buffer + (MAX_SNES_WIDTH_NTSC >> 1) * 16;
    S9xGraphicsInit();

    S9xInitInputDevices();
    for (int i = 0; i < 2; i++)
    {
        S9xSetController(i, CTL_JOYPAD, i, 0, 0, 0);
        snes_devices[i] = RETRO_DEVICE_JOYPAD;
    }

    S9xUnmapAllControls();
    map_buttons();
    check_system_specs();
	
    if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
        libretro_supports_bitmasks = true;
}

#define MAP_BUTTON(id, name) S9xMapButton((id), S9xGetCommandT((name)), false)
#define MAKE_BUTTON(pad, btn) (((pad)<<4)|(btn))

#define PAD_1 1
#define PAD_2 2
#define PAD_3 3
#define PAD_4 4
#define PAD_5 5
#define PAD_6 6
#define PAD_7 7
#define PAD_8 8

#define BTN_B RETRO_DEVICE_ID_JOYPAD_B
#define BTN_Y RETRO_DEVICE_ID_JOYPAD_Y
#define BTN_SELECT RETRO_DEVICE_ID_JOYPAD_SELECT
#define BTN_START RETRO_DEVICE_ID_JOYPAD_START
#define BTN_UP RETRO_DEVICE_ID_JOYPAD_UP
#define BTN_DOWN RETRO_DEVICE_ID_JOYPAD_DOWN
#define BTN_LEFT RETRO_DEVICE_ID_JOYPAD_LEFT
#define BTN_RIGHT RETRO_DEVICE_ID_JOYPAD_RIGHT
#define BTN_A RETRO_DEVICE_ID_JOYPAD_A
#define BTN_X RETRO_DEVICE_ID_JOYPAD_X
#define BTN_L RETRO_DEVICE_ID_JOYPAD_L
#define BTN_R RETRO_DEVICE_ID_JOYPAD_R
#define BTN_FIRST BTN_B
#define BTN_LAST BTN_R

#define MOUSE_X RETRO_DEVICE_ID_MOUSE_X
#define MOUSE_Y RETRO_DEVICE_ID_MOUSE_Y
#define MOUSE_LEFT RETRO_DEVICE_ID_MOUSE_LEFT
#define MOUSE_RIGHT RETRO_DEVICE_ID_MOUSE_RIGHT
#define MOUSE_MIDDLE RETRO_DEVICE_ID_MOUSE_MIDDLE
#define MOUSE_FIRST MOUSE_X
#define MOUSE_LAST MOUSE_MIDDLE

static int scope_buttons[] =
{
	RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, // 2
	RETRO_DEVICE_ID_LIGHTGUN_CURSOR, // 3
	RETRO_DEVICE_ID_LIGHTGUN_TURBO, // 4
	RETRO_DEVICE_ID_LIGHTGUN_START, // 5
	RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN, // 6
};
static int scope_button_count = sizeof( scope_buttons ) / sizeof( int );

#define SUPER_SCOPE_TRIGGER 2
#define SUPER_SCOPE_CURSOR 3
#define SUPER_SCOPE_TURBO 4
#define SUPER_SCOPE_START 5

#define JUSTIFIER_TRIGGER 2
#define JUSTIFIER_START 3
#define JUSTIFIER_OFFSCREEN 4

#define MACS_RIFLE_TRIGGER 2

#define BTN_POINTER (BTN_LAST + 1)
#define BTN_POINTER2 (BTN_POINTER + 1)


static void map_buttons()
{
    MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_A), "Joypad1 A");
    MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_B), "Joypad1 B");
    MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_X), "Joypad1 X");
    MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_Y), "Joypad1 Y");
    MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_SELECT), "{Joypad1 Select,Mouse1 L}");
    MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_START), "{Joypad1 Start,Mouse1 R}");
    MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_L), "Joypad1 L");
    MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_R), "Joypad1 R");
    MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_LEFT), "Joypad1 Left");
    MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_RIGHT), "Joypad1 Right");
    MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_UP), "Joypad1 Up");
    MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_DOWN), "Joypad1 Down");
    S9xMapPointer((BTN_POINTER), S9xGetCommandT("Pointer Mouse1+Superscope+Justifier1+MacsRifle"), false);
    S9xMapPointer((BTN_POINTER2), S9xGetCommandT("Pointer Mouse2+Justifier2"), false);

    MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_B), "Joypad2 B");
    MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_Y), "Joypad2 Y");
    MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_SELECT), "{Joypad2 Select,Mouse2 L,Superscope Fire,Justifier1 Trigger,MacsRifle Trigger}");
    MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_START), "{Joypad2 Start,Mouse2 R,Superscope Cursor,Justifier1 Start}");
    MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_UP), "{Joypad2 Up,Superscope ToggleTurbo,Justifier1 AimOffscreen}");
    MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_DOWN), "{Joypad2 Down,Superscope Pause}");
    MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_LEFT), "{Joypad2 Left,Superscope AimOffscreen}");
    MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_RIGHT), "Joypad2 Right");
    MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_A), "Joypad2 A");
    MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_X), "Joypad2 X");
    MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_L), "Joypad2 L");
    MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_R), "Joypad2 R");

    MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_B), "Joypad3 B");
    MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_Y), "Joypad3 Y");
    MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_SELECT), "{Joypad3 Select,Justifier2 Trigger}");
    MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_START), "{Joypad3 Start,Justifier2 Start}");
    MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_UP), "{Joypad3 Up,Justifier2 AimOffscreen}");
    MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_DOWN), "Joypad3 Down");
    MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_LEFT), "Joypad3 Left");
    MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_RIGHT), "Joypad3 Right");
    MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_A), "Joypad3 A");
    MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_X), "Joypad3 X");
    MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_L), "Joypad3 L");
    MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_R), "Joypad3 R");

    MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_A), "Joypad4 A");
    MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_B), "Joypad4 B");
    MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_X), "Joypad4 X");
    MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_Y), "Joypad4 Y");
    MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_SELECT), "Joypad4 Select");
    MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_START), "Joypad4 Start");
    MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_L), "Joypad4 L");
    MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_R), "Joypad4 R");
    MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_LEFT), "Joypad4 Left");
    MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_RIGHT), "Joypad4 Right");
    MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_UP), "Joypad4 Up");
    MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_DOWN), "Joypad4 Down");

    MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_A), "Joypad5 A");
    MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_B), "Joypad5 B");
    MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_X), "Joypad5 X");
    MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_Y), "Joypad5 Y");
    MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_SELECT), "Joypad5 Select");
    MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_START), "Joypad5 Start");
    MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_L), "Joypad5 L");
    MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_R), "Joypad5 R");
    MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_LEFT), "Joypad5 Left");
    MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_RIGHT), "Joypad5 Right");
    MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_UP), "Joypad5 Up");
    MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_DOWN), "Joypad5 Down");

    MAP_BUTTON(MAKE_BUTTON(PAD_6, BTN_A), "Joypad6 A");
    MAP_BUTTON(MAKE_BUTTON(PAD_6, BTN_B), "Joypad6 B");
    MAP_BUTTON(MAKE_BUTTON(PAD_6, BTN_X), "Joypad6 X");
    MAP_BUTTON(MAKE_BUTTON(PAD_6, BTN_Y), "Joypad6 Y");
    MAP_BUTTON(MAKE_BUTTON(PAD_6, BTN_SELECT), "Joypad6 Select");
    MAP_BUTTON(MAKE_BUTTON(PAD_6, BTN_START), "Joypad6 Start");
    MAP_BUTTON(MAKE_BUTTON(PAD_6, BTN_L), "Joypad6 L");
    MAP_BUTTON(MAKE_BUTTON(PAD_6, BTN_R), "Joypad6 R");
    MAP_BUTTON(MAKE_BUTTON(PAD_6, BTN_LEFT), "Joypad6 Left");
    MAP_BUTTON(MAKE_BUTTON(PAD_6, BTN_RIGHT), "Joypad6 Right");
    MAP_BUTTON(MAKE_BUTTON(PAD_6, BTN_UP), "Joypad6 Up");
    MAP_BUTTON(MAKE_BUTTON(PAD_6, BTN_DOWN), "Joypad6 Down");
	
    MAP_BUTTON(MAKE_BUTTON(PAD_7, BTN_A), "Joypad7 A");
    MAP_BUTTON(MAKE_BUTTON(PAD_7, BTN_B), "Joypad7 B");
    MAP_BUTTON(MAKE_BUTTON(PAD_7, BTN_X), "Joypad7 X");
    MAP_BUTTON(MAKE_BUTTON(PAD_7, BTN_Y), "Joypad7 Y");
    MAP_BUTTON(MAKE_BUTTON(PAD_7, BTN_SELECT), "Joypad7 Select");
    MAP_BUTTON(MAKE_BUTTON(PAD_7, BTN_START), "Joypad7 Start");
    MAP_BUTTON(MAKE_BUTTON(PAD_7, BTN_L), "Joypad7 L");
    MAP_BUTTON(MAKE_BUTTON(PAD_7, BTN_R), "Joypad7 R");
    MAP_BUTTON(MAKE_BUTTON(PAD_7, BTN_LEFT), "Joypad7 Left");
    MAP_BUTTON(MAKE_BUTTON(PAD_7, BTN_RIGHT), "Joypad7 Right");
    MAP_BUTTON(MAKE_BUTTON(PAD_7, BTN_UP), "Joypad7 Up");
    MAP_BUTTON(MAKE_BUTTON(PAD_7, BTN_DOWN), "Joypad7 Down");
	
    MAP_BUTTON(MAKE_BUTTON(PAD_8, BTN_A), "Joypad8 A");
    MAP_BUTTON(MAKE_BUTTON(PAD_8, BTN_B), "Joypad8 B");
    MAP_BUTTON(MAKE_BUTTON(PAD_8, BTN_X), "Joypad8 X");
    MAP_BUTTON(MAKE_BUTTON(PAD_8, BTN_Y), "Joypad8 Y");
    MAP_BUTTON(MAKE_BUTTON(PAD_8, BTN_SELECT), "Joypad8 Select");
    MAP_BUTTON(MAKE_BUTTON(PAD_8, BTN_START), "Joypad8 Start");
    MAP_BUTTON(MAKE_BUTTON(PAD_8, BTN_L), "Joypad8 L");
    MAP_BUTTON(MAKE_BUTTON(PAD_8, BTN_R), "Joypad8 R");
    MAP_BUTTON(MAKE_BUTTON(PAD_8, BTN_LEFT), "Joypad8 Left");
    MAP_BUTTON(MAKE_BUTTON(PAD_8, BTN_RIGHT), "Joypad8 Right");
    MAP_BUTTON(MAKE_BUTTON(PAD_8, BTN_UP), "Joypad8 Up");
    MAP_BUTTON(MAKE_BUTTON(PAD_8, BTN_DOWN), "Joypad8 Down");
}

/* Promoted to int32_t to prevent overflow during long sessions: the servo
 * accumulates per-frame steps and would wrap an int16_t after ~32s of sustained
 * fast motion, producing catastrophic jumps. The SNES Mouse layer in
 * controls.cpp stores cur_x/old_x as int16 and computes the per-frame delta as
 * (cur_x - old_x) clamped to +-127; int16 wraparound preserves that delta
 * correctly as long as the real per-frame delta fits in int16, so casting to
 * int16_t at the S9xReportPointer boundary is sufficient and safe. We must NOT
 * reset the accumulator after a report: controls.cpp tracks old_x as the last
 * reported cumulative value, so the accumulator must continue carrying the
 * total to keep delta = step_x each frame. */
static int32_t snes_mouse_state[2][2] = {{0}, {0}};
static bool snes_superscope_turbo_latch = false;

static void input_report_gun_position( unsigned port, int s9xinput )
{
	int x, y;

    x = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
    y = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);

	/*scale & clamp - use 0xFFFE for universal hardware compatibility*/
	x = ( ( x + 0x7FFF ) * g_screen_gun_width ) / 0xFFFE;
	if ( x < 0 )
		x = 0;
	else if ( x >= g_screen_gun_width )
		x = g_screen_gun_width - 1;

	/*scale & clamp - use 0xFFFE for universal hardware compatibility*/
	y = ( ( y + 0x7FFF ) * g_screen_gun_height ) / 0xFFFE;
	if ( y < 0 )
		y = 0;
	else if ( y >= g_screen_gun_height )
		y = g_screen_gun_height - 1;

	S9xReportPointer(s9xinput, (int16_t)x, (int16_t)y);
}

static void input_handle_pointer_lightgun( unsigned port, unsigned gun_device, int s9xinput )
{
    int16_t pointer_x, pointer_y;
    int x, y;
    
    /* Always poll coordinates for S-Pen hover support - RetroArch handles hover detection */
    /* This enables proper hover cursor movement and side button detection during hover */
    pointer_x = input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
    pointer_y = input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
    /* pressed at idx 0 == any active pointer (hover+barrel or contact) */
    bool pointer_is_pressed = input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
    /* semantic indices (contact path always sets idx1/idx2; hover barrel sets idx2 if RA setting enabled) */
    bool tip_pressed    = input_state_cb(port, RETRO_DEVICE_POINTER, 1, RETRO_DEVICE_ID_POINTER_PRESSED);
    bool barrel_pressed = input_state_cb(port, RETRO_DEVICE_POINTER, 2, RETRO_DEVICE_ID_POINTER_PRESSED);
    
#ifdef DEBUG_SPEN_VERBOSE
    /* Debug: log pointer state to understand what RetroArch provides */
    if (port == 0) {
        static int log_counter = 0;
        if (log_counter++ % 60 == 0) { /* Log every ~1 second at 60fps */
            fprintf(stderr, "[SNES9X Debug] Pointer: x=%d y=%d pressed=%d tip=%d barrel=%d\n", pointer_x, pointer_y, pointer_is_pressed, tip_pressed, barrel_pressed);
            if (log_cb) {
                log_cb(RETRO_LOG_INFO, "[SNES9X Debug] Pointer: x=%d y=%d pressed=%d tip=%d barrel=%d\n", pointer_x, pointer_y, pointer_is_pressed, tip_pressed, barrel_pressed);
            }
        }
    }
#endif

	/* Matrix-based coordinate transformation consistent with S-Pen mouse mode */
	/* Convert libretro lightgun coordinates to normalized device coordinates */
	float ndc_x = (float)pointer_x / 32767.0f;
	float ndc_y = (float)pointer_y / 32767.0f;

	/* Transform from NDC [-1,1] to screen coordinates [0, width/height] */
	float screen_x = (ndc_x + 1.0f) * 0.5f * (float)g_screen_gun_width;
	float screen_y = (ndc_y + 1.0f) * 0.5f * (float)g_screen_gun_height;

	/* Apply sub-pixel precision with proper rounding */
	x = (int)(screen_x + 0.5f);
	y = (int)(screen_y + 0.5f);

	/* Clamp coordinates to valid range */
	if (x < 0)
		x = 0;
	else if (x >= g_screen_gun_width)
		x = g_screen_gun_width - 1;
	if (y < 0)
		y = 0;
	else if (y >= g_screen_gun_height)
		y = g_screen_gun_height - 1;

    // Touch sensitivity: Keep the gun position held for a fixed number of cycles after touch is released
    // because a very light touch can result in a misfire
    if ( pointer_cycles_after_released > 0 && pointer_cycles_after_released < POINTER_PRESSED_CYCLES ) {
        pointer_cycles_after_released++;
        x = pointer_pressed_last_x;
        y = pointer_pressed_last_y;
        S9xReportPointer(s9xinput, (int16_t)x, (int16_t)y);
        return;
    }

    if ( pointer_is_pressed )
    {
        pointer_pressed = 1;
        pointer_cycles_after_released = 0;
        pointer_pressed_last_x = x;
        pointer_pressed_last_y = y;
    }
    else if ( pointer_pressed )
    {
        pointer_cycles_after_released++;
        if (pointer_cycles_after_released < POINTER_PRESSED_CYCLES) {
            x = pointer_pressed_last_x;
            y = pointer_pressed_last_y;
            S9xReportPointer(s9xinput, (int16_t)x, (int16_t)y);
            return;
        }
        pointer_pressed = 0;
        // unpress the primary trigger
        switch (gun_device)
        {
        case RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE:
            S9xReportButton(MAKE_BUTTON(PAD_2, setting_superscope_reverse_buttons ? SUPER_SCOPE_CURSOR : SUPER_SCOPE_TRIGGER), false);
            break;
        case RETRO_DEVICE_LIGHTGUN_JUSTIFIER:
            S9xReportButton(MAKE_BUTTON(PAD_2, JUSTIFIER_TRIGGER), false);
            break;
        case RETRO_DEVICE_LIGHTGUN_MACS_RIFLE:
            S9xReportButton(MAKE_BUTTON(PAD_2, MACS_RIFLE_TRIGGER), false);
            break;
        default:
            break;
        }
        return;
    }
    S9xReportPointer(s9xinput, (int16_t)x, (int16_t)y);

    // triggers
    switch (gun_device)
    {
        case RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE:
        {
            bool start_pressed = false;
            bool trigger_pressed = false;
            bool turbo_pressed = false;
            bool cursor_pressed = false;
            
            /* Legacy finger touch multi-touch support - always preserved */
            if ( pointer_is_pressed ) {
                int touch_count = input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_COUNT);
                if ( touch_count == 4 ) {
                    start_pressed = true;
                } else if ( touch_count == 3 ) {
                    turbo_pressed = true;
                } else if ( touch_count == 2 ) {
                    if ( setting_superscope_reverse_buttons ) {
                        trigger_pressed = true;
                    } else {
                        cursor_pressed = true;
                    }
                } else {
                    if ( setting_superscope_reverse_buttons ) {
                        cursor_pressed = true;
                    } else {
                        trigger_pressed = true;
                    }
                }
            }
            
            /* S-Pen enhancement - use semantic pointer indices from RetroArch */
            bool tap_detected    = tip_pressed;     /* left-equivalent */
            bool barrel_detected = barrel_pressed;  /* right-equivalent */
            /* Hover detection: stylus present but not touching */
            bool hover_detected  = !pointer_is_pressed && (spen_hover_behavior != SPEN_HOVER_DISABLED);
            
#ifdef DEBUG_SPEN_VERBOSE
            /* Debug logging for S-Pen events */
            if (log_cb) {
                if (hover_detected || tap_detected || barrel_detected) {
                    log_cb(RETRO_LOG_INFO, "[SNES9X S-Pen] Lightgun: hover=%d tap=%d barrel=%d coords=(%d,%d)\n",
                           hover_detected, tap_detected, barrel_detected, x, y);
                }
            }
#endif
            
            if (tap_detected && spen_tap_action != SNES9X_SPEN_ACTION_DISABLED) {
                switch (spen_tap_action) {
                    case SNES9X_SPEN_ACTION_LEFT_CLICK:
                    case SNES9X_SPEN_ACTION_RIGHT_CLICK:
                    case SNES9X_SPEN_ACTION_MIDDLE_CLICK:
                    case SNES9X_SPEN_ACTION_TRIGGER:
                        trigger_pressed = true; break;
                    case SNES9X_SPEN_ACTION_RELOAD:
                        /* Reload typically handled as off-screen shot in lightgun games */
                        break;
                }
            }
            
            if (barrel_detected && spen_barrel_action != SNES9X_SPEN_ACTION_DISABLED) {
                switch (spen_barrel_action) {
                    case SNES9X_SPEN_ACTION_LEFT_CLICK:
                    case SNES9X_SPEN_ACTION_RIGHT_CLICK:
                    case SNES9X_SPEN_ACTION_MIDDLE_CLICK:
                    case SNES9X_SPEN_ACTION_TRIGGER:
                        trigger_pressed = true; break;
                    case SNES9X_SPEN_ACTION_RELOAD:
                        /* Reload typically handled as off-screen shot */
                        break;
                }
            }
            
            /* Hover behavior for lightgun tracking */
            if (hover_detected && spen_hover_behavior == SPEN_HOVER_LIGHTGUN_TRACK) {
                cursor_pressed = true; /* Show cursor when hovering for aiming */
            }
            S9xReportButton(MAKE_BUTTON(PAD_2, SUPER_SCOPE_START), start_pressed);
            S9xReportButton(MAKE_BUTTON(PAD_2, SUPER_SCOPE_TRIGGER), trigger_pressed);
            S9xReportButton(MAKE_BUTTON(PAD_2, SUPER_SCOPE_CURSOR), cursor_pressed);
            bool old_turbo = turbo_pressed;
            turbo_pressed = turbo_pressed && !snes_superscope_turbo_latch;
            snes_superscope_turbo_latch = old_turbo;
            S9xReportButton(MAKE_BUTTON(PAD_2, SUPER_SCOPE_TURBO), turbo_pressed);
            break;
        }

        case RETRO_DEVICE_LIGHTGUN_JUSTIFIER:
        {
            bool trigger_pressed = false;
            bool start_pressed = false;
            bool offscreen = false;
            if ( pointer_pressed ) {
                int touch_count = input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_COUNT);
                if ( touch_count == 3 ) {
                    start_pressed = true;
                } else if ( touch_count == 2 ) {
                    offscreen = true;
                } else {
                    trigger_pressed = true;
                }
            }
            S9xReportButton(MAKE_BUTTON(PAD_2, JUSTIFIER_TRIGGER), trigger_pressed || offscreen);
            S9xReportButton(MAKE_BUTTON(PAD_2, JUSTIFIER_START), start_pressed ? 1 : 0 );
            S9xReportButton(MAKE_BUTTON(PAD_2, JUSTIFIER_OFFSCREEN), offscreen);
            break;
        }
        case RETRO_DEVICE_LIGHTGUN_MACS_RIFLE:
        {
            int pressed = pointer_pressed;
            S9xReportButton(MAKE_BUTTON(PAD_2, MACS_RIFLE_TRIGGER),pressed);
            break;
        }
        case RETRO_DEVICE_NONE:
            break;
        default:
            if (log_cb)
                log_cb(RETRO_LOG_ERROR, "Unknown device for touchscreen lightgun...\n");
    }
}

static void report_buttons()
{
    int offset = snes_devices[0] == RETRO_DEVICE_JOYPAD_MULTITAP ? 4 : 1;
    int _x, _y;
    int16_t joy_bits;

    for (int port = 0; port <= 1; port++)
    {
        switch (snes_devices[port])
        {
            case RETRO_DEVICE_JOYPAD:
                if (libretro_supports_bitmasks)
                    joy_bits = input_state_cb(port * offset, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
                else
                {
                    joy_bits = 0;
                    for (int i = 0; i < (RETRO_DEVICE_ID_JOYPAD_R3+1); i++)
                        joy_bits |= input_state_cb(port * offset, RETRO_DEVICE_JOYPAD, 0, i) ? (1 << i) : 0;
                }

                for (int i = BTN_FIRST; i <= BTN_LAST; i++)
                    S9xReportButton(MAKE_BUTTON(port * offset + 1, i), joy_bits & (1 << i));
                break;

            case RETRO_DEVICE_JOYPAD_MULTITAP:
                for (int j = 0; j < 4; j++)
                {
                    if (libretro_supports_bitmasks)
                        joy_bits = input_state_cb(port * offset + j, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
                    else
                    {
                        joy_bits = 0;
                        for (int i = 0; i < (RETRO_DEVICE_ID_JOYPAD_R3+1); i++)
                            joy_bits |= input_state_cb(port * offset + j, RETRO_DEVICE_JOYPAD, 0, i) ? (1 << i) : 0;
                    }

                    for (int i = BTN_FIRST; i <= BTN_LAST; i++)
                        S9xReportButton(MAKE_BUTTON(port * offset + j + 1, i), joy_bits & (1 << i));
				}
                break;

            case RETRO_DEVICE_MOUSE:
            {
                /* Enhanced mouse handling: S-Pen (absolute/relative) + legacy mouse/touch */

                int16_t pointer_x = input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
                int16_t pointer_y = input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
                bool general_pressed = input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
                bool tip_pressed = input_state_cb(port, RETRO_DEVICE_POINTER, 1, RETRO_DEVICE_ID_POINTER_PRESSED);
                bool barrel_pressed = input_state_cb(port, RETRO_DEVICE_POINTER, 2, RETRO_DEVICE_ID_POINTER_PRESSED);
                int pointer_count = input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_COUNT);
                /* RetroArch delivers HOVER as updated POINTER_X/Y with COUNT=0 and PRESSED=false
                   (the X/Y query is count-independent). So detect the pen by a CHANGE in coords,
                   NOT by count>0 (misses hover) and NOT by coords!=0 (creeps on stale lifted coords).
                   coords_changed => pen is actively hovering or moving; static coords => pen away. */
                static int16_t spen_last_px[2] = {0, 0}, spen_last_py[2] = {0, 0};
                bool coords_changed = (pointer_x != spen_last_px[port] || pointer_y != spen_last_py[port]);
                spen_last_px[port] = pointer_x;
                spen_last_py[port] = pointer_y;
                bool pointer_active = pointer_count > 0 || general_pressed || tip_pressed || barrel_pressed || coords_changed;

                /* LOG POINT 1: Raw input from RetroArch - throttled to reduce spam */
                #ifdef DEBUG_SPEN_VERBOSE
                static int log_frame_counter = 0;
                if (log_cb && (log_frame_counter++ % 60 == 0 || general_pressed || tip_pressed || barrel_pressed)) {
                    log_cb(RETRO_LOG_INFO, "[SPEN-DEBUG-1] RAW INPUT: ptr_x=%d ptr_y=%d count=%d | pressed: gen=%d tip=%d barrel=%d\n",
                           pointer_x, pointer_y, pointer_count, general_pressed, tip_pressed, barrel_pressed);
                }
                #endif

                bool has_spen_features = (spen_tap_action != SNES9X_SPEN_ACTION_DISABLED ||
                                          spen_barrel_action != SNES9X_SPEN_ACTION_DISABLED ||
                                          spen_hover_behavior != SPEN_HOVER_DISABLED);
                bool force_spen_mode = (spen_input_mode == 1);
                bool stylus_signal = tip_pressed || barrel_pressed;
                bool hover_signal = (pointer_count > 0 || coords_changed) && (spen_hover_behavior != SPEN_HOVER_DISABLED);
                bool is_spen_mode = force_spen_mode ||
                                    (spen_input_mode == 0 && has_spen_features && (stylus_signal || hover_signal));

                /* S-PEN ABSOLUTE MODE */
                if (is_spen_mode && spen_coordinate_mode == 0 && (pointer_active || force_spen_mode)) {
                    bool any_button_pressed = tip_pressed || general_pressed || barrel_pressed;
                    (void)any_button_pressed;

                    if (!spen_servo_initialized[port]) {
                        spen_servo_initialized[port] = true;
                        spen_est_x[port] = g_screen_gun_width  / 2.0;
                        spen_est_y[port] = g_screen_gun_height / 2.0;
                    }

                    /* RetroArch pointer [-32767,+32767] -> absolute game pixels (target). */
                    double target_x = ((double)pointer_x + 32767.0) * (double)g_screen_gun_width  / 65534.0;
                    double target_y = ((double)pointer_y + 32767.0) * (double)g_screen_gun_height / 65534.0;

                    int step_x = 0, step_y = 0;
                    if (spen_ram_cursor_enabled) {
                        /* RAM-FEEDBACK servo (known mouse game, e.g. Mario Paint): read the game's
                           REAL cursor from WRAM ($7Exxxx == Memory.RAM[xxxx], addresses from the
                           per-ROM profile table) and drive it toward the pen target with a damped,
                           clamped delta. Closed-loop => gain-independent, no dead-reckoning drift,
                           self-recovers after lifts/edges. The game's own canvas clamp handles
                           out-of-canvas pen positions. */
                        int actual_x = (int)Memory.RAM[spen_ram_cur_x_addr];
                        int actual_y = (int)Memory.RAM[spen_ram_cur_y_addr];
                        int err_x = (int)(target_x + 0.5) - actual_x;
                        int err_y = (int)(target_y + 0.5) - actual_y;
                        double kp = (spen_servo_kp > 0.05) ? spen_servo_kp : 0.5; /* responsiveness; ~0.5 stable */
                        step_x = (int)(kp * err_x + (err_x >= 0 ? 0.5 : -0.5));
                        step_y = (int)(kp * err_y + (err_y >= 0 ? 0.5 : -0.5));
                        const int MAXSTEP = 24; /* cap per-frame delta to avoid overshoot at unknown game gain */
                        if (step_x >  MAXSTEP) step_x =  MAXSTEP; else if (step_x < -MAXSTEP) step_x = -MAXSTEP;
                        if (step_y >  MAXSTEP) step_y =  MAXSTEP; else if (step_y < -MAXSTEP) step_y = -MAXSTEP;
                        if (err_x == 0) step_x = 0;
                        if (err_y == 0) step_y = 0;
                        spen_est_x[port] = (double)actual_x; /* est := real cursor (for the trace) */
                        spen_est_y[port] = (double)actual_y;
                    } else {
                        /* DEAD-RECKONING fallback (unknown game): we cannot read its cursor RAM, so
                           the servo integrates its own estimate toward the absolute pen target and
                           emits a clamped delta. Drifts on blind moves but is safe for any ROM. */
                        spen_servo_params sp = { spen_servo_kp, spen_servo_gain, spen_servo_deadzone, 127 };
                        step_x = spen_servo_step(target_x, &spen_est_x[port], &sp);
                        step_y = spen_servo_step(target_y, &spen_est_y[port], &sp);

                        #ifdef DEBUG_SPEN_VERBOSE
                        /* WRAM cursor AUTO-FINDER: only for unprofiled games (this branch), only
                           accumulate when the pen is actively moving (target changed since last
                           frame) so static-hover frames don't dilute the correlation. Port 0 only
                           (single accumulator set). */
                        if (port == 0) {
                            static double spen_finder_last_tx = -1e9, spen_finder_last_ty = -1e9;
                            bool tgt_moved = (fabs(target_x - spen_finder_last_tx) >= 1.0 ||
                                              fabs(target_y - spen_finder_last_ty) >= 1.0);
                            if (tgt_moved) {
                                spen_finder_update(target_x, target_y);
                                spen_finder_last_tx = target_x;
                                spen_finder_last_ty = target_y;
                            }
                        }
                        #endif
                    }

                    /* Emit relative deltas; S9xReportPointer/UpdatePolledMouse derive the wire delta from cur-old. */
                    snes_mouse_state[port][0] += step_x;
                    snes_mouse_state[port][1] += step_y;

                    #ifdef DEBUG_SPEN_VERBOSE
                    if (log_cb && (log_frame_counter % 30 == 0 || tip_pressed)) {
                        log_cb(RETRO_LOG_INFO, "[SPEN-SERVO] raw=(%d,%d) tgt=(%.1f,%.1f) est=(%.1f,%.1f) step=(%d,%d) acc=(%d,%d) gain=%.2f kp=%.2f\n",
                               pointer_x, pointer_y, target_x, target_y, spen_est_x[port], spen_est_y[port],
                               step_x, step_y, snes_mouse_state[port][0], snes_mouse_state[port][1], spen_servo_gain, spen_servo_kp);
                        /* READ-ONLY RAM probe: which WRAM byte tracks the pen target? (candidate
                           Mario Paint cursor addresses). $7Exxxx == Memory.RAM[xxxx]. */
                        log_cb(RETRO_LOG_INFO, "[SPEN-RAM] tgt=(%.0f,%.0f) | 0226=%u 0227=%u | 04DC=%u 04DE=%u | tool0426=%u\n",
                               target_x, target_y,
                               (unsigned)Memory.RAM[0x0226], (unsigned)Memory.RAM[0x0227],
                               (unsigned)Memory.RAM[0x04DC], (unsigned)Memory.RAM[0x04DE],
                               (unsigned)Memory.RAM[0x0426]);
                    }
                    #endif

                    for (int i = MOUSE_LEFT; i <= MOUSE_LAST; i++) {
                        bool pressed = input_state_cb(port, RETRO_DEVICE_MOUSE, 0, i);

                        /* Hold MOUSE_LEFT while the pen is in proximity. Many SNES Mouse
                         * games (Mario Paint, Clock Tower) only update the rendered cursor
                         * when a button is held — without this they'd freeze on hover.
                         * Trade-off: canvas-paint games will draw on hover. Tap-to-click
                         * games still work because tip_pressed is what registers as the
                         * intentional click event. */
                        if (i == MOUSE_LEFT && general_pressed)
                            pressed = true;

                        if (tip_pressed && spen_tap_action != SNES9X_SPEN_ACTION_DISABLED) {
                            switch (spen_tap_action) {
                                case SNES9X_SPEN_ACTION_LEFT_CLICK:   if (i == MOUSE_LEFT) pressed = true; break;
                                case SNES9X_SPEN_ACTION_RIGHT_CLICK:  if (i == MOUSE_RIGHT) pressed = true; break;
                                case SNES9X_SPEN_ACTION_MIDDLE_CLICK: if (i == MOUSE_MIDDLE) pressed = true; break;
                                case SNES9X_SPEN_ACTION_TRIGGER:      if (i == MOUSE_LEFT) pressed = true; break;
                                case SNES9X_SPEN_ACTION_RELOAD:       break;
                            }
                        }

                        if (barrel_pressed && spen_barrel_action != SNES9X_SPEN_ACTION_DISABLED) {
                            switch (spen_barrel_action) {
                                case SNES9X_SPEN_ACTION_LEFT_CLICK:   if (i == MOUSE_LEFT) pressed = true; break;
                                case SNES9X_SPEN_ACTION_RIGHT_CLICK:  if (i == MOUSE_RIGHT) pressed = true; break;
                                case SNES9X_SPEN_ACTION_MIDDLE_CLICK: if (i == MOUSE_MIDDLE) pressed = true; break;
                                case SNES9X_SPEN_ACTION_TRIGGER:      if (i == MOUSE_LEFT) pressed = true; break;
                            }
                        }

                        #ifdef DEBUG_SPEN_INPUT
                        if (pressed && log_cb) {
                            static const char *btn_names[] = { "LEFT", "RIGHT", "MIDDLE", "BUTTON4", "BUTTON5" };
                            const char *btn_name = (i >= 0 && i < 5) ? btn_names[i] : "UNKNOWN";
                            log_cb(RETRO_LOG_INFO, "[SNES9X S-Pen] Button %s: PRESSED | general=%d tip=%d barrel=%d tap_action=%d barrel_action=%d\n",
                                   btn_name, general_pressed, tip_pressed, barrel_pressed, spen_tap_action, spen_barrel_action);
                        }
                        #endif

                        if (i == MOUSE_LEFT && !any_button_pressed && spen_hover_behavior == SPEN_HOVER_ACTIVE_CURSOR) {
                            #ifdef DEBUG_SPEN_VERBOSE
                            static int hover_log_counter = 0;
                            if (hover_log_counter++ % 60 == 0 && log_cb) {
                                log_cb(RETRO_LOG_INFO, "[SNES9X S-Pen VERBOSE] Hover: Cursor est at (%d,%d) no buttons pressed - hover_behavior=%d\n", (int)spen_est_x[port], (int)spen_est_y[port], spen_hover_behavior);
                            }
                            #endif
                        }

                        S9xReportButton(MAKE_BUTTON(port + 1, i), pressed);
                    }
                }
                /* S-PEN RELATIVE MODE */
                else if (is_spen_mode && spen_coordinate_mode == 1 && (pointer_active || force_spen_mode)) {
                    double screen_width = (double)g_screen_gun_width;
                    double screen_height = (double)g_screen_gun_height;
                    double screen_x = ((double)pointer_x + 32767.0) * screen_width / 65534.0;
                    double screen_y = ((double)pointer_y + 32767.0) * screen_height / 65534.0;
                    int x = (int)(screen_x + 0.5);
                    int y = (int)(screen_y + 0.5);

                    if (!spen_rel_initialized[port]) {
                        spen_rel_initialized[port] = true;
                        spen_last_rel_x[port] = x;
                        spen_last_rel_y[port] = y;
                        _x = 0;
                        _y = 0;
                    } else {
                        _x = x - spen_last_rel_x[port];
                        _y = y - spen_last_rel_y[port];
                        spen_last_rel_x[port] = x;
                        spen_last_rel_y[port] = y;
                    }

                    snes_mouse_state[port][0] += _x;
                    snes_mouse_state[port][1] += _y;

                    for (int i = MOUSE_LEFT; i <= MOUSE_LAST; i++) {
                        bool pressed = false;

                        if (i == MOUSE_LEFT && general_pressed)
                            pressed = true;

                        if (tip_pressed && spen_tap_action != SNES9X_SPEN_ACTION_DISABLED) {
                            switch (spen_tap_action) {
                                case SNES9X_SPEN_ACTION_LEFT_CLICK:   if (i == MOUSE_LEFT) pressed = true; break;
                                case SNES9X_SPEN_ACTION_RIGHT_CLICK:  if (i == MOUSE_RIGHT) pressed = true; break;
                                case SNES9X_SPEN_ACTION_MIDDLE_CLICK: if (i == MOUSE_MIDDLE) pressed = true; break;
                                case SNES9X_SPEN_ACTION_TRIGGER:      if (i == MOUSE_LEFT) pressed = true; break;
                                case SNES9X_SPEN_ACTION_RELOAD:       break;
                            }
                        }

                        if (barrel_pressed && spen_barrel_action != SNES9X_SPEN_ACTION_DISABLED) {
                            switch (spen_barrel_action) {
                                case SNES9X_SPEN_ACTION_LEFT_CLICK:   if (i == MOUSE_LEFT) pressed = true; break;
                                case SNES9X_SPEN_ACTION_RIGHT_CLICK:  if (i == MOUSE_RIGHT) pressed = true; break;
                                case SNES9X_SPEN_ACTION_MIDDLE_CLICK: if (i == MOUSE_MIDDLE) pressed = true; break;
                                case SNES9X_SPEN_ACTION_TRIGGER:      if (i == MOUSE_LEFT) pressed = true; break;
                            }
                        }

                        S9xReportButton(MAKE_BUTTON(port + 1, i), pressed);
                    }
                }
                /* Legacy absolute mouse (touch) when S-Pen is inactive */
                else if (setting_mouse_mode == SETTING_MOUSE_MODE_ABSOLUTE && pointer_active) {
                    if (!spen_servo_initialized[port]) {
                        spen_servo_initialized[port] = true;
                        spen_est_x[port] = g_screen_gun_width  / 2.0;
                        spen_est_y[port] = g_screen_gun_height / 2.0;
                    }
                    double tgx = ((double)pointer_x + 0x7FFF) * (double)g_screen_gun_width  / 0xFFFE;
                    double tgy = ((double)pointer_y + 0x7FFF) * (double)g_screen_gun_height / 0xFFFE;
                    spen_servo_params sp2 = { spen_servo_kp, spen_servo_gain, spen_servo_deadzone, 127 };
                    snes_mouse_state[port][0] += spen_servo_step(tgx, &spen_est_x[port], &sp2);
                    snes_mouse_state[port][1] += spen_servo_step(tgy, &spen_est_y[port], &sp2);

                    for (int i = MOUSE_LEFT; i <= MOUSE_LAST; i++) {
                        bool pressed = input_state_cb(port, RETRO_DEVICE_MOUSE, 0, i);
                        if (i == MOUSE_LEFT && general_pressed)
                            pressed = true;
                        S9xReportButton(MAKE_BUTTON(port + 1, i), pressed);
                    }
                }
                /* Traditional relative mouse */
                else {
                    /* pen away / non-stylus: emit real mouse deltas (0 for stylus), keep servo est frozen */
                    _x = input_state_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
                    _y = input_state_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
                    snes_mouse_state[port][0] += _x;
                    snes_mouse_state[port][1] += _y;
                    for (int i = MOUSE_LEFT; i <= MOUSE_LAST; i++)
                        S9xReportButton(MAKE_BUTTON(port + 1, i), input_state_cb(port, RETRO_DEVICE_MOUSE, 0, i));
                }
                /* int32 -> int16 truncation here is intentional and safe: see the
                 * comment on snes_mouse_state. controls.cpp computes the wire delta
                 * as (cur_x - old_x) in int16 arithmetic, which two's-complement
                 * wraparound makes equivalent to the true mathematical delta as
                 * long as the per-frame delta itself fits in int16 (we clamp servo
                 * steps to +-127). */
                S9xReportPointer(BTN_POINTER + port,
                                 (int16_t)snes_mouse_state[port][0],
                                 (int16_t)snes_mouse_state[port][1]);
            }
                break;

            case RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE:
            {
                /* S-Pen input mode integration: force lightgun mode uses pointer input for better precision */
                bool use_spen_lightgun = (spen_input_mode == 2) || /* Force lightgun mode */
                                       (spen_input_mode == 0 && /* Auto-detect mode */
                                        (spen_tap_action != SNES9X_SPEN_ACTION_DISABLED || 
                                         spen_barrel_action != SNES9X_SPEN_ACTION_DISABLED ||
                                         spen_hover_behavior != SPEN_HOVER_DISABLED));

                if (use_spen_lightgun || setting_gun_input == SETTING_GUN_INPUT_POINTER) {
                    input_handle_pointer_lightgun(port, RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE, BTN_POINTER);
                } else {
                    // Lightgun is default
                    input_report_gun_position( port, BTN_POINTER );

                    for (int i = 0; i < scope_button_count; i++)
                    {
                        int id = scope_buttons[i];
                        bool btn = input_state_cb( port, RETRO_DEVICE_LIGHTGUN, 0, id )?true:false;

                        /* RETRO_DEVICE_ID_LIGHTGUN_TURBO special case - core needs a rising-edge trigger */
                        if ( id == RETRO_DEVICE_ID_LIGHTGUN_TURBO )
                        {
                            bool old = btn;
                            btn = btn && !snes_superscope_turbo_latch;
                            snes_superscope_turbo_latch = old;
                        }
                        int super_scope_button_id = i+2;
                        if ( setting_superscope_reverse_buttons )
                        {
                            if ( super_scope_button_id == SUPER_SCOPE_TRIGGER ) {
                                super_scope_button_id = SUPER_SCOPE_CURSOR;
                            } else if ( super_scope_button_id == SUPER_SCOPE_CURSOR ) {
                                super_scope_button_id = SUPER_SCOPE_TRIGGER;
                            }
                        }
                        S9xReportButton(MAKE_BUTTON(PAD_2, super_scope_button_id), btn);
                    }
                }
            }
                break;

            case RETRO_DEVICE_LIGHTGUN_JUSTIFIER:

                if ( setting_gun_input == SETTING_GUN_INPUT_POINTER ) {
                    input_handle_pointer_lightgun(port, RETRO_DEVICE_LIGHTGUN_JUSTIFIER, BTN_POINTER);
                } else {
                    // Lightgun is default
                    input_report_gun_position( port, BTN_POINTER );

                    {
                        /* Special Reload Button */
                        int btn_offscreen_shot = input_state_cb( port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD );

                        /* Trigger ? */
                        int btn_trigger = input_state_cb( port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER );
                        S9xReportButton(MAKE_BUTTON(PAD_2, JUSTIFIER_TRIGGER), btn_trigger || btn_offscreen_shot);

                        /* Start Button ? */
                        int btn_start = input_state_cb( port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START );
                        S9xReportButton(MAKE_BUTTON(PAD_2, JUSTIFIER_START), btn_start ? 1 : 0 );

                        /* Aiming off-screen ? */
                        int btn_offscreen = input_state_cb( port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN );
                        S9xReportButton(MAKE_BUTTON(PAD_2, JUSTIFIER_OFFSCREEN), btn_offscreen || btn_offscreen_shot);
                    }

                    /* Second Gun? */
                    if ( snes_devices[port+1] == RETRO_DEVICE_LIGHTGUN_JUSTIFIER_2 )
                    {
                        int second = port+1;

                        input_report_gun_position( second, BTN_POINTER2 );

                        /* Special Reload Button */
                        int btn_offscreen_shot = input_state_cb( second, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD );

                        /* Trigger ? */
                        int btn_trigger = input_state_cb( second, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER );
                        S9xReportButton(MAKE_BUTTON(PAD_3, JUSTIFIER_TRIGGER), btn_trigger || btn_offscreen_shot);

                        /* Start Button ? */
                        int btn_start = input_state_cb( second, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START );
                        S9xReportButton(MAKE_BUTTON(PAD_3, JUSTIFIER_START), btn_start ? 1 : 0 );

                        /* Aiming off-screen ? */
                        int btn_offscreen = input_state_cb( second, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN );
                        S9xReportButton(MAKE_BUTTON(PAD_3, JUSTIFIER_OFFSCREEN), btn_offscreen || btn_offscreen_shot);
                    }
                }
                break;

            case RETRO_DEVICE_LIGHTGUN_MACS_RIFLE:

                if ( setting_gun_input == SETTING_GUN_INPUT_POINTER ) {
                    input_handle_pointer_lightgun(port, RETRO_DEVICE_LIGHTGUN_MACS_RIFLE, BTN_POINTER);
                } else {
                    input_report_gun_position( port, BTN_POINTER );

                    {
                        /* Trigger ? */
                        int btn_trigger = input_state_cb( port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER );
                        S9xReportButton(MAKE_BUTTON(PAD_2, MACS_RIFLE_TRIGGER), btn_trigger);
                    }
                }
                break;

            case RETRO_DEVICE_NONE:
                break;

            default:
                if (log_cb)
                    log_cb(RETRO_LOG_ERROR, "Unknown device...\n");
        }
    }
}

void retro_run()
{
    static uint16 height = PPU.ScreenHeight;
    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
        update_variables();

    if (g_geometry_update || height != PPU.ScreenHeight)
    {
        update_geometry();
        height = PPU.ScreenHeight;
    }

    int result = -1;
    bool okay = environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &result);
    if (okay)
    {
        bool audioEnabled = 0 != (result & 2);
        bool videoEnabled = 0 != (result & 1);
        IPPU.RenderThisFrame = videoEnabled;
        S9xSetSoundMute(!audioEnabled);
    }
    else
    {
        IPPU.RenderThisFrame = true;
        S9xSetSoundMute(false);
    }

    poll_cb();
    report_buttons();
    S9xMainLoop();
}

void retro_deinit()
{
    S9xDeinitAPU();
    Memory.Deinit();
    S9xGraphicsDeinit();
    S9xUnmapAllControls();

    free(screen_buffer);
    free(ntsc_screen_buffer);

    libretro_supports_option_categories = false;
    libretro_supports_bitmasks = false;
}


unsigned retro_get_region()
{
    return Settings.PAL ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

void* retro_get_memory_data(unsigned type)
{
    void* data;

    switch(type)
    {
        case RETRO_MEMORY_SNES_SUFAMI_TURBO_A_RAM:
        case RETRO_MEMORY_SAVE_RAM:
            data = Memory.SRAM;
            break;
        case RETRO_MEMORY_SNES_SUFAMI_TURBO_B_RAM:
            data = Multi.sramB;
            break;
        case RETRO_MEMORY_RTC:
            data = RTCData.reg;
            break;
        case RETRO_MEMORY_SYSTEM_RAM:
        data = Memory.RAM;
        break;
        case RETRO_MEMORY_VIDEO_RAM:
        data = Memory.VRAM;
        break;
        //case RETRO_MEMORY_ROM:
        //	data = Memory.ROM;
        //	break;
        default:
            data = NULL;
            break;
    }

    return data;
}

size_t retro_get_memory_size(unsigned type)
{
    size_t size;

    switch(type) {
        case RETRO_MEMORY_SNES_SUFAMI_TURBO_A_RAM:
        case RETRO_MEMORY_SAVE_RAM:
            size = (unsigned) (Memory.SRAMSize ? (1 << (Memory.SRAMSize + 3)) * 128 : 0);
            if (size > 0x20000)
            size = 0x20000;
            break;
        case RETRO_MEMORY_SNES_SUFAMI_TURBO_B_RAM:
            size = (unsigned) (Multi.cartType==4 && Multi.sramSizeB ? (1 << (Multi.sramSizeB + 3)) * 128 : 0);
            break;
        case RETRO_MEMORY_RTC:
            size = (Settings.SRTC || Settings.SPC7110RTC)?20:0;
            break;
        case RETRO_MEMORY_SYSTEM_RAM:
            size = 128 * 1024;
            break;
        case RETRO_MEMORY_VIDEO_RAM:
            size = 64 * 1024;
            break;
        //case RETRO_MEMORY_ROM:
        //	size = Memory.CalculatedSize;
        //	break;
        default:
            size = 0;
            break;
    }

    return size;
}

size_t retro_serialize_size()
{
    return rom_loaded ? S9xFreezeSize() : 0;
}

bool retro_serialize(void *data, size_t size)
{
    int result = -1;
    bool okay = false;
    okay = environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &result);
    if (okay)
    {
        Settings.FastSavestates = 0 != (result & 4);
    }
    if (S9xFreezeGameMem((uint8_t*)data,size) == FALSE)
        return false;

    return true;
}

bool retro_unserialize(const void* data, size_t size)
{
    int result = -1;
    bool okay = false;
    okay = environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &result);
    if (okay)
    {
        Settings.FastSavestates = 0 != (result & 4);
    }
    if (S9xUnfreezeGameMem((const uint8_t*)data,size) != SUCCESS)
        return false;

    // restore disabled sound channels
    if (disabled_channels)
    {
        S9xSetSoundControl(disabled_channels^0xFF);
    }

    return true;
}

bool8 S9xDeinitUpdate(int width, int height)
{
    static int burst_phase = 0;
    int overscan_offset = 0;

    if (crop_overscan_mode == OVERSCAN_CROP_ON)
    {
        if (height > SNES_HEIGHT * 2)
        {
            overscan_offset = 14;
            height = SNES_HEIGHT * 2;
        }
        else if ((height > SNES_HEIGHT) && (height != SNES_HEIGHT * 2))
        {
            overscan_offset = 7;
            height = SNES_HEIGHT;
        }
    }
    else if (crop_overscan_mode == OVERSCAN_CROP_12)
    {
        if (height > 216 * 2)
        {
            overscan_offset = 8;
            height = 216 * 2;
        }
        else if ((height > 216) && (height != 216 * 2))
        {
            overscan_offset = 4;
            height = 216;
        }
    }
    else if (crop_overscan_mode == OVERSCAN_CROP_16)
    {
        if (height > 208 * 2)
        {
            overscan_offset = 16;
            height = 208 * 2;
        }
        else if ((height > 208) && (height != 208 * 2))
        {
            overscan_offset = 8;
            height = 208;
        }
    }
    else if (crop_overscan_mode == OVERSCAN_CROP_OFF)
    {
        if (height > SNES_HEIGHT_EXTENDED)
        {
            if (height < SNES_HEIGHT_EXTENDED * 2)
            {
                overscan_offset = -16;
                memset(GFX.Screen + (GFX.Pitch >> 1) * height,0,GFX.Pitch * ((SNES_HEIGHT_EXTENDED << 1) - height));
            }
            height = SNES_HEIGHT_EXTENDED * 2;
        }
        else
        {
            if (height < SNES_HEIGHT_EXTENDED)
            {
                overscan_offset = -8;
                memset(GFX.Screen + (GFX.Pitch >> 1) * height,0,GFX.Pitch * (SNES_HEIGHT_EXTENDED - height));
            }
            height = SNES_HEIGHT_EXTENDED;
        }
    }


    if (blargg_filter)
    {
        burst_phase = (burst_phase + 1) % 3;

        if (width == 512)
            snes_ntsc_blit_hires(snes_ntsc, GFX.Screen, GFX.Pitch / 2, burst_phase, width, height, snes_ntsc_buffer, MAX_SNES_WIDTH_NTSC * 2);
        else
            snes_ntsc_blit(snes_ntsc, GFX.Screen, GFX.Pitch / 2, burst_phase, width, height, snes_ntsc_buffer, MAX_SNES_WIDTH_NTSC * 2);

        video_cb(snes_ntsc_buffer + ((int)(MAX_SNES_WIDTH_NTSC) * overscan_offset), SNES_NTSC_OUT_WIDTH(256), height, MAX_SNES_WIDTH_NTSC * 2);
    }
    else if (width == MAX_SNES_WIDTH && hires_blend)
    {
        #define AVERAGE_565(el0, el1) (((el0) & (el1)) + ((((el0) ^ (el1)) & 0xF7DE) >> 1))

        if (hires_blend == 1) /* Blur method */
        {
            for (int y = 0; y < height; y++)
            {
                uint16 *input = (uint16 *) ((uint8 *) GFX.Screen + y * GFX.Pitch);
                uint16 *output = (uint16 *) ((uint8 *) GFX.Screen + y * GFX.Pitch);
                uint16 l, r;

                l = 0;
                for (int x = 0; x < (width >> 1); x++)
                {
                    r = *input++;
                    *output++ = AVERAGE_565 (l, r);
                    l = r;

                    r = *input++;
                    *output++ = AVERAGE_565 (l, r);
                    l = r;
                }
            }
        }
        else if (hires_blend == 2) /* Merge method */
        {
            for (int y = 0; y < height; y++)
            {
                uint16 *input = (uint16 *) ((uint8 *) GFX.Screen + y * GFX.Pitch);
                uint16 *output = (uint16 *) ((uint8 *) GFX.Screen + y * GFX.Pitch);
                uint16 l, r;

                for (int x = 0; x < (width >> 1); x++)
                {
                    l = *input++;
                    r = *input++;
                    *output++ = AVERAGE_565 (l, r);
                }
            }

            width >>= 1;
        }

        video_cb(GFX.Screen + ((int)(GFX.Pitch >> 1) * overscan_offset), width, height, GFX.Pitch);
    }
    else
    {
        video_cb(GFX.Screen + ((int)(GFX.Pitch >> 1) * overscan_offset), width, height, GFX.Pitch);
    }

    return TRUE;
}

bool8 S9xContinueUpdate(int width, int height)
{
    S9xDeinitUpdate(width, height);
    return true;
}

// Dummy functions that should probably be implemented correctly later.
void S9xParsePortConfig(ConfigFile&, int) {}
const char* S9xStringInput(const char* in) { return in; }

#ifdef _WIN32
#define SLASH '\\'
#else
#define SLASH '/'
#endif

std::string S9xGetDirectory(s9x_getdirtype type)
{
    switch (type)
    {
        case BIOS_DIR:
            return std::string(retro_system_directory);
        default:
            return std::string(g_rom_dir);
    }

    return std::string("");
}
void S9xInitInputDevices() {}
void S9xHandlePortCommand(s9xcommand_t, short, short) {}
bool S9xPollButton(uint32, bool*) { return false; }
void S9xToggleSoundChannel(int) {}
std::string S9xGetFilenameInc(std::string in, s9x_getdirtype) { return ""; }
const char* S9xBasename(const char* in) { return in; }
bool8 S9xInitUpdate() { return TRUE; }
void S9xExtraUsage() {}
bool8 S9xOpenSoundDevice() { return TRUE; }
bool S9xPollAxis(uint32, short*) { return FALSE; }
void S9xParseArg(char**, int&, int) {}
void S9xExit() {}
bool S9xPollPointer(uint32, short*, short*) { return false; }

void S9xMessage(int type, int, const char* s)
{
    if (!log_cb) return;

    switch (type)
    {
        case S9X_DEBUG:
            log_cb(RETRO_LOG_DEBUG, "%s\n", s);
            break;
        case S9X_WARNING:
            log_cb(RETRO_LOG_WARN, "%s\n", s);
            break;
        case S9X_INFO:
            log_cb(RETRO_LOG_INFO, "%s\n", s);
            break;
        case S9X_ERROR:
            log_cb(RETRO_LOG_ERROR, "%s\n", s);
            break;
        default:
            log_cb(RETRO_LOG_DEBUG, "%s\n", s);
            break;
    }
}

bool8 S9xOpenSnapshotFile(const char* filepath, bool8 read_only, STREAM *file)
{
    if(read_only)
    {
        if((*file = OPEN_STREAM(filepath, "rb")) != 0)
        {
            return (TRUE);
        }
    }
    else
    {
        if((*file = OPEN_STREAM(filepath, "wb")) != 0)
        {
            return (TRUE);
        }
    }
    return (FALSE);
}

void S9xCloseSnapshotFile(STREAM file)
{
    CLOSE_STREAM(file);
}

void S9xAutoSaveSRAM()
{
    return;
}
