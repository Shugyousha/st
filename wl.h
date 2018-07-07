#include <stdlib.h>

#include <wld/wld.h>
#include <wld/wayland.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <fontconfig/fontconfig.h>

/* macros */
#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))
#define LEN(a)			(sizeof(a) / sizeof(a)[0])
#define DEFAULT(a, b)		(a) = (a) ? (a) : (b)
#define BETWEEN(x, a, b)	((a) <= (x) && (x) <= (b))
#define DIVCEIL(n, d)		(((n) + ((d) - 1)) / (d))
#define ISCONTROLC0(c)		(BETWEEN(c, 0, 0x1f) || (c) == '\177')
#define ISCONTROLC1(c)		(BETWEEN(c, 0x80, 0x9f))
#define ISCONTROL(c)		(ISCONTROLC0(c) || ISCONTROLC1(c))
#define ISDELIM(u)		(utf8strchr(worddelimiters, u) != NULL)
#define LIMIT(x, a, b)		(x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b)		((a).mode != (b).mode || (a).fg != (b).fg || \
				(a).bg != (b).bg)
#define IS_SET(flag)		((term.mode & (flag)) != 0)
#define TIMEDIFF(t1, t2)	((t1.tv_sec-t2.tv_sec)*1000 + \
				(t1.tv_nsec-t2.tv_nsec)/1E6)
#define MODBIT(x, set, bit)	((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

#define TRUECOLOR(r,g,b)	(1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x)		(1 << 24 & (x))
#define TRUERED(x)		(((x) & 0xff0000) >> 8)
#define TRUEGREEN(x)		(((x) & 0xff00))
#define TRUEBLUE(x)		(((x) & 0xff) << 8)

#include "xdg-shell-unstable-v5-client-protocol.h"

#define AXIS_VERTICAL	WL_POINTER_AXIS_VERTICAL_SCROLL
#define AXIS_HORIZONTAL	WL_POINTER_AXIS_HORIZONTAL_SCROLL

typedef uint_least32_t Rune;

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

typedef union {
	int i;
	uint ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	Rune u;           /* character code */
	ushort mode;      /* attribute flags */
	uint32_t fg;      /* foreground  */
	uint32_t bg;      /* background  */
} Glyph;

typedef Glyph *Line;

typedef struct {
	struct xkb_context *ctx;
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	xkb_mod_index_t ctrl, alt, shift, logo;
	unsigned int mods;
} XKB;

typedef struct {
	int mode;
	int type;
	int snap;
	/*
	 * Selection variables:
	 * nb – normalized coordinates of the beginning of the selection
	 * ne – normalized coordinates of the end of the selection
	 * ob – original coordinates of the beginning of the selection
	 * oe – original coordinates of the end of the selection
	 */
	struct {
		int x, y;
	} nb, ne, ob, oe;

	char *primary;
	struct wl_data_source *source;
	int alt;
	uint32_t tclick1, tclick2;
} Selection;


/* Font structure */
typedef struct {
	int height;
	int width;
	int ascent;
	int descent;
	int badslant;
	int badweight;
	short lbearing;
	short rbearing;
	struct wld_font *match;
	FcFontSet *set;
	FcPattern *pattern;
} Font;

typedef struct {
	struct wl_display *dpy;
	struct wl_compositor *cmp;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
	struct wl_data_device_manager *datadevmanager;
	struct wl_data_device *datadev;
	struct wl_data_offer *seloffer;
	struct wl_surface *surface;
	struct wl_buffer *buffer;
	struct xdg_shell *shell;
	struct xdg_surface *xdgsurface;
	XKB xkb;
	bool configured;
	int px, py; /* pointer x and y */
	int tw, th; /* tty width and height */
	int w, h; /* window width and height */
	int ch; /* char height */
	int cw; /* char width  */
	int vis;
	char state; /* focus, redraw, visible */
	int cursor; /* cursor style */
	struct wl_callback * framecb;
} Wayland;

typedef struct {
	struct wld_context *ctx;
	struct wld_font_context *fontctx;
	struct wld_renderer *renderer;
	struct wld_buffer *buffer, *oldbuffer;
} WLD;

typedef struct {
	struct wl_cursor_theme *theme;
	struct wl_cursor *cursor;
	struct wl_surface *surface;
} Cursor;

void wldraws(char *, Glyph, int, int, int, int);
void wldrawglyph(Glyph, int, int);
void wlclear(int, int, int, int);
void wldrawcursor(void);
void wlinit(void);
void wlloadcols(void);
int wlsetcolorname(int, const char *);
void wlloadcursor(void);
int wlloadfont(Font *, FcPattern *);
void wlloadfonts(char *, double);
void wlsettitle(char *);
void wlresettitle(void);
void wlseturgency(int);
void wlsetsel(char*, uint32_t);
void wlunloadfont(Font *f);
void wlunloadfonts(void);
void wlresize(int, int);

void wlzoom(const Arg *);
void wlzoomabs(const Arg *);
void wlzoomreset(const Arg *);

void regglobal(void *, struct wl_registry *, uint32_t, const char *,
 	uint32_t);
void regglobalremove(void *, struct wl_registry *, uint32_t);
void surfenter(void *, struct wl_surface *, struct wl_output *);
void surfleave(void *, struct wl_surface *, struct wl_output *);
void framedone(void *, struct wl_callback *, uint32_t);
void kbdkeymap(void *, struct wl_keyboard *, uint32_t, int32_t, uint32_t);
void kbdenter(void *, struct wl_keyboard *, uint32_t,
 	struct wl_surface *, struct wl_array *);
void kbdleave(void *, struct wl_keyboard *, uint32_t,
 	struct wl_surface *);
void kbdkey(void *, struct wl_keyboard *, uint32_t, uint32_t, uint32_t,
 	uint32_t);
void kbdmodifiers(void *, struct wl_keyboard *, uint32_t, uint32_t,
 	uint32_t, uint32_t, uint32_t);
void kbdrepeatinfo(void *, struct wl_keyboard *, int32_t, int32_t);
void ptrenter(void *, struct wl_pointer *, uint32_t, struct wl_surface *,
 	wl_fixed_t, wl_fixed_t);
void ptrleave(void *, struct wl_pointer *, uint32_t,
 	struct wl_surface *);
void ptrmotion(void *, struct wl_pointer *, uint32_t,
 	wl_fixed_t, wl_fixed_t);
void ptrbutton(void *, struct wl_pointer *, uint32_t, uint32_t,
 	uint32_t, uint32_t);
void ptraxis(void *, struct wl_pointer *, uint32_t, uint32_t,
 	wl_fixed_t);
void xdgshellping(void *, struct xdg_shell *, uint32_t);
void xdgsurfconfigure(void *, struct xdg_surface *,
 	int32_t, int32_t, struct wl_array *, uint32_t);
void xdgsurfclose(void *, struct xdg_surface *);
void datadevoffer(void *, struct wl_data_device *,
 	struct wl_data_offer *);
void datadeventer(void *, struct wl_data_device *, uint32_t,
 	struct wl_surface *, wl_fixed_t, wl_fixed_t, struct wl_data_offer *);
void datadevleave(void *, struct wl_data_device *);
void datadevmotion(void *, struct wl_data_device *, uint32_t,
 	wl_fixed_t x, wl_fixed_t y);
void datadevdrop(void *, struct wl_data_device *);
void datadevselection(void *, struct wl_data_device *,
 	struct wl_data_offer *);
void dataofferoffer(void *, struct wl_data_offer *, const char *);
void datasrctarget(void *, struct wl_data_source *, const char *);
void datasrcsend(void *, struct wl_data_source *, const char *, int32_t);
void datasrccancelled(void *, struct wl_data_source *);

void draw(void);
void redraw(void);
void drawregion(int, int, int, int);

void cresize(int, int);

