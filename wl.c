#include "wl.h"

/* Drawing Context */
typedef struct {
	uint32_t col[MAX(LEN(colorname), 256)];
	Font font, bfont, ifont, ibfont;
} DC;

struct wl_registry_listener reglistener = { regglobal, regglobalremove };
struct wl_surface_listener surflistener = { surfenter, surfleave };
struct wl_callback_listener framelistener = { framedone };
struct wl_keyboard_listener kbdlistener =
 { kbdkeymap, kbdenter, kbdleave, kbdkey, kbdmodifiers, kbdrepeatinfo };
struct wl_pointer_listener ptrlistener =
 { ptrenter, ptrleave, ptrmotion, ptrbutton, ptraxis };
struct xdg_shell_listener xdgshelllistener = { xdgshellping };
struct xdg_surface_listener xdgsurflistener =
 { xdgsurfconfigure, xdgsurfclose };
struct wl_data_device_listener datadevlistener =
 { datadevoffer, datadeventer, datadevleave, datadevmotion, datadevdrop,
   datadevselection };
struct wl_data_offer_listener dataofferlistener = { dataofferoffer };
struct wl_data_source_listener datasrclistener =
	{ datasrctarget, datasrcsend, datasrccancelled };

static DC dc;
static Wayland wl;
static WLD wld;

void
wlsetsel(char *str, uint32_t serial)
{
	free(sel.primary);
	sel.primary = str;

	if (str) {
		sel.source = wl_data_device_manager_create_data_source(wl.datadevmanager);
		wl_data_source_add_listener(sel.source, &datasrclistener, NULL);
		wl_data_source_offer(sel.source, "text/plain; charset=utf-8");
	} else {
		sel.source = NULL;
	}
	wl_data_device_set_selection(wl.datadev, sel.source, serial);
}

void wlloadcursor(void)
{
	char *names[] = { mouseshape, "xterm", "ibeam", "text" };
	int i;

	cursor.theme = wl_cursor_theme_load(NULL, 32, wl.shm);

	for (i = 0; !cursor.cursor && i < LEN(names); i++)
		cursor.cursor = wl_cursor_theme_get_cursor(cursor.theme, names[i]);

	cursor.surface = wl_compositor_create_surface(wl.cmp);
}

void
ptraxis(void * data, struct wl_pointer * pointer, uint32_t time, uint32_t axis,
        wl_fixed_t value)
{
	Axiskey *ak;
	int dir = value > 0 ? +1 : -1;

	if (IS_SET(MODE_MOUSE) && !(wl.xkb.mods & forceselmod)) {
		wlmousereportaxis(axis, value);
		return;
	}

	for (ak = ashortcuts; ak < ashortcuts + LEN(ashortcuts); ak++) {
		if (axis == ak->axis && dir == ak->dir
				&& match(ak->mask, wl.xkb.mods)) {
			ttysend(ak->s, strlen(ak->s));
			return;
		}
	}
}

void
xdgshellping(void *data, struct xdg_shell *shell, uint32_t serial)
{
	xdg_shell_pong(shell, serial);
}

void
xdgsurfconfigure(void *data, struct xdg_surface *surf, int32_t w, int32_t h,
                 struct wl_array *states, uint32_t serial)
{
	xdg_surface_ack_configure(surf, serial);
	if (w == wl.w && h == wl.h)
		return;
	cresize(w, h);
	if (wl.configured)
		ttyresize();
	else
		wl.configured = true;
}

void
xdgsurfclose(void *data, struct xdg_surface *surf)
{
	/* Send SIGHUP to shell */
	kill(pid, SIGHUP);
	exit(0);
}

void
datadevoffer(void *data, struct wl_data_device *datadev,
             struct wl_data_offer *offer)
{
	wl_data_offer_add_listener(offer, &dataofferlistener, NULL);
}

void
datadeventer(void *data, struct wl_data_device *datadev, uint32_t serial,
		struct wl_surface *surf, wl_fixed_t x, wl_fixed_t y,
		struct wl_data_offer *offer)
{
}

void
datadevleave(void *data, struct wl_data_device *datadev)
{
}

void
datadevmotion(void *data, struct wl_data_device *datadev, uint32_t time,
              wl_fixed_t x, wl_fixed_t y)
{
}

void
datadevdrop(void *data, struct wl_data_device *datadev)
{
}

void
datadevselection(void *data, struct wl_data_device *datadev,
                 struct wl_data_offer *offer)
{
	if (offer && (uintptr_t) wl_data_offer_get_user_data(offer) == 1)
		wl.seloffer = offer;
	else
		wl.seloffer = NULL;
}

void
dataofferoffer(void *data, struct wl_data_offer *offer, const char *mimetype)
{
	/* mark the offer as usable if it supports plain text */
	if (strncmp(mimetype, "text/plain", 10) == 0)
		wl_data_offer_set_user_data(offer, (void *)(uintptr_t) 1);
}

void
datasrctarget(void *data, struct wl_data_source *source, const char *mimetype)
{
}

void
datasrcsend(void *data, struct wl_data_source *source, const char *mimetype,
            int32_t fd)
{
	char *buf = sel.primary;
	int len = strlen(sel.primary);
	ssize_t ret;
	while ((ret = write(fd, buf, MIN(len, BUFSIZ))) > 0) {
		len -= ret;
		buf += ret;
	}
	close(fd);
}

void
datasrccancelled(void *data, struct wl_data_source *source)
{
	if (sel.source == source) {
		sel.source = NULL;
		selclear();
	}
	wl_data_source_destroy(source);
}

void
cresize(int width, int height)
{
	int col, row;

	if (width != 0)
		wl.w = width;
	if (height != 0)
		wl.h = height;

	col = (wl.w - 2 * borderpx) / wl.cw;
	row = (wl.h - 2 * borderpx) / wl.ch;

	tresize(col, row);
	wlresize(col, row);
}

void
kbdkeymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd,
          uint32_t size)
{
	char *string;

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}

	string = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);

	if (string == MAP_FAILED) {
		close(fd);
		return;
	}

	wl.xkb.keymap = xkb_keymap_new_from_string(wl.xkb.ctx, string,
			XKB_KEYMAP_FORMAT_TEXT_V1, 0);
	munmap(string, size);
	close(fd);
	wl.xkb.state = xkb_state_new(wl.xkb.keymap);

	wl.xkb.ctrl = xkb_keymap_mod_get_index(wl.xkb.keymap, XKB_MOD_NAME_CTRL);
	wl.xkb.alt = xkb_keymap_mod_get_index(wl.xkb.keymap, XKB_MOD_NAME_ALT);
	wl.xkb.shift = xkb_keymap_mod_get_index(wl.xkb.keymap, XKB_MOD_NAME_SHIFT);
	wl.xkb.logo = xkb_keymap_mod_get_index(wl.xkb.keymap, XKB_MOD_NAME_LOGO);

	wl.xkb.mods = 0;
}

void
kbdenter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
         struct wl_surface *surface, struct wl_array *keys)
{
	wl.state |= WIN_FOCUSED;
	if (IS_SET(MODE_FOCUS))
		ttywrite("\033[I", 3);
	/* need to redraw the cursor */
	needdraw = true;
}

void
kbdleave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
	 struct wl_surface *surface)
{
	/* selection offers are invalidated when we lose keyboard focus */
	wl.seloffer = NULL;
	wl.state &= ~WIN_FOCUSED;
	if (IS_SET(MODE_FOCUS))
		ttywrite("\033[O", 3);
	/* need to redraw the cursor */
	needdraw = true;
	/* disable key repeat */
	repeat.len = 0;
}

void
kbdkey(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
       uint32_t key, uint32_t state)
{
	xkb_keysym_t ksym;
	char buf[32], *str;
	int len;
	Rune c;
	Shortcut *bp;

	if (IS_SET(MODE_KBDLOCK))
		return;

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		if (repeat.key == key)
			repeat.len = 0;
		return;
	}

	ksym = xkb_state_key_get_one_sym(wl.xkb.state, key + 8);
	len = xkb_keysym_to_utf8(ksym, buf, sizeof buf);
	if (len > 0)
	    --len;

	/* 1. shortcuts */
	for (bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
		if (ksym == bp->keysym && match(bp->mod, wl.xkb.mods)) {
			bp->func(&(bp->arg));
			return;
		}
	}

	/* 2. custom keys from config.h */
	if ((str = kmap(ksym, wl.xkb.mods))) {
		len = strlen(str);
		goto send;
	}

	/* 3. composed string from input method */
	if (len == 0)
		return;
	if (len == 1 && wl.xkb.mods & MOD_MASK_ALT) {
		if (IS_SET(MODE_8BIT)) {
			if (*buf < 0177) {
				c = *buf | 0x80;
				len = utf8encode(c, buf);
			}
		} else {
			buf[1] = buf[0];
			buf[0] = '\033';
			len = 2;
		}
	}
	/* convert character to control character */
	else if (len == 1 && wl.xkb.mods & MOD_MASK_CTRL) {
		if ((*buf >= '@' && *buf < '\177') || *buf == ' ')
			*buf &= 0x1F;
		else if (*buf == '2') *buf = '\000';
		else if (*buf >= '3' && *buf <= '7')
			*buf -= ('3' - '\033');
		else if (*buf == '8') *buf = '\177';
		else if (*buf == '/') *buf = '_' & 0x1F;
	}

	str = buf;

send:
	memcpy(repeat.str, str, len);
	repeat.key = key;
	repeat.len = len;
	repeat.started = false;
	clock_gettime(CLOCK_MONOTONIC, &repeat.last);
	ttysend(str, len);
}

void
kbdmodifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
             uint32_t dep, uint32_t lat, uint32_t lck, uint32_t group)
{
	xkb_mod_mask_t mod_mask;

	xkb_state_update_mask(wl.xkb.state, dep, lat, lck, group, 0, 0);

	mod_mask = xkb_state_serialize_mods(wl.xkb.state, XKB_STATE_MODS_EFFECTIVE);
	wl.xkb.mods = 0;

	if (mod_mask & (1 << wl.xkb.ctrl))
		wl.xkb.mods |= MOD_MASK_CTRL;
	if (mod_mask & (1 << wl.xkb.alt))
		wl.xkb.mods |= MOD_MASK_ALT;
	if (mod_mask & (1 << wl.xkb.shift))
		wl.xkb.mods |= MOD_MASK_SHIFT;
	if (mod_mask & (1 << wl.xkb.logo))
		wl.xkb.mods |= MOD_MASK_LOGO;
}

void
kbdrepeatinfo(void *data, struct wl_keyboard *keyboard, int32_t rate,
              int32_t delay)
{
	keyrepeatdelay = delay;
	keyrepeatinterval = 1000 / rate;
}

void
ptrenter(void *data, struct wl_pointer *pointer, uint32_t serial,
         struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
	struct wl_cursor_image *img = cursor.cursor->images[0];
	struct wl_buffer *buffer;

	wl_pointer_set_cursor(pointer, serial, cursor.surface,
			img->hotspot_x, img->hotspot_y);
	buffer = wl_cursor_image_get_buffer(img);
	wl_surface_attach(cursor.surface, buffer, 0, 0);
	wl_surface_damage(cursor.surface, 0, 0, img->width, img->height);
	wl_surface_commit(cursor.surface);
}

void
ptrleave(void *data, struct wl_pointer *pointer, uint32_t serial,
         struct wl_surface *surface)
{
}

void
ptrmotion(void *data, struct wl_pointer * pointer, uint32_t serial,
          wl_fixed_t x, wl_fixed_t y)
{
	int oldey, oldex, oldsby, oldsey;

	if (IS_SET(MODE_MOUSE)) {
		wlmousereportmotion(x, y);
		return;
	}

	wl.px = wl_fixed_to_int(x);
	wl.py = wl_fixed_to_int(y);

	if (!sel.mode)
		return;

	sel.mode = SEL_READY;
	oldey = sel.oe.y;
	oldex = sel.oe.x;
	oldsby = sel.nb.y;
	oldsey = sel.ne.y;
	getbuttoninfo();

	if (oldey != sel.oe.y || oldex != sel.oe.x)
		tsetdirt(MIN(sel.nb.y, oldsby), MAX(sel.ne.y, oldsey));
}

void
ptrbutton(void * data, struct wl_pointer * pointer, uint32_t serial,
          uint32_t time, uint32_t button, uint32_t state)
{
	MouseShortcut *ms;

	if (IS_SET(MODE_MOUSE) && !(wl.xkb.mods & forceselmod)) {
		wlmousereportbutton(button, state);
		return;
	}

	switch (state) {
	case WL_POINTER_BUTTON_STATE_RELEASED:
		if (button == BTN_MIDDLE) {
			selpaste(NULL);
		} else if (button == BTN_LEFT) {
			if (sel.mode == SEL_READY) {
				getbuttoninfo();
				selcopy(serial);
			} else
				selclear();
			sel.mode = SEL_IDLE;
			tsetdirt(sel.nb.y, sel.ne.y);
		}
		break;

	case WL_POINTER_BUTTON_STATE_PRESSED:
		for (ms = mshortcuts; ms < mshortcuts + LEN(mshortcuts); ms++) {
			if (button == ms->b && match(ms->mask, wl.xkb.mods)) {
				ttysend(ms->s, strlen(ms->s));
				return;
			}
		}

		if (button == BTN_LEFT) {
			/* Clear previous selection, logically and visually. */
			selclear();
			sel.mode = SEL_EMPTY;
			sel.type = SEL_REGULAR;
			sel.oe.x = sel.ob.x = x2col(wl.px);
			sel.oe.y = sel.ob.y = y2row(wl.py);

			/*
			 * If the user clicks below predefined timeouts
			 * specific snapping behaviour is exposed.
			 */
			if (time - sel.tclick2 <= tripleclicktimeout) {
				sel.snap = SNAP_LINE;
			} else if (time - sel.tclick1 <= doubleclicktimeout) {
				sel.snap = SNAP_WORD;
			} else {
				sel.snap = 0;
			}
			selnormalize();

			if (sel.snap != 0)
				sel.mode = SEL_READY;
			tsetdirt(sel.nb.y, sel.ne.y);
			sel.tclick2 = sel.tclick1;
			sel.tclick1 = time;
		}
		break;
	}
}


void
regglobalremove(void *data, struct wl_registry *registry, uint32_t name)
{
}

void
surfenter(void *data, struct wl_surface *surface, struct wl_output *output)
{
	wl.vis++;
	if (!(wl.state & WIN_VISIBLE))
		wl.state |= WIN_VISIBLE;
}

void
surfleave(void *data, struct wl_surface *surface, struct wl_output *output)
{
	if (--wl.vis == 0)
		wl.state &= ~WIN_VISIBLE;
}

void
framedone(void *data, struct wl_callback *callback, uint32_t msecs)
{
	wl_callback_destroy(callback);
	wl.framecb = NULL;
	if (needdraw && wl.state & WIN_VISIBLE) {
		draw();
	}
}

void
regglobal(void *data, struct wl_registry *registry, uint32_t name,
          const char *interface, uint32_t version)
{
	if (strcmp(interface, "wl_compositor") == 0) {
		wl.cmp = wl_registry_bind(registry, name,
				&wl_compositor_interface, 3);
	} else if (strcmp(interface, "xdg_shell") == 0) {
		wl.shell = wl_registry_bind(registry, name,
				&xdg_shell_interface, 1);
		xdg_shell_add_listener(wl.shell, &xdgshelllistener, NULL);
		xdg_shell_use_unstable_version(wl.shell,
				XDG_SHELL_VERSION_CURRENT);
	} else if (strcmp(interface, "wl_shm") == 0) {
		wl.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, "wl_seat") == 0) {
		wl.seat = wl_registry_bind(registry, name,
				&wl_seat_interface, 4);
	} else if (strcmp(interface, "wl_data_device_manager") == 0) {
		wl.datadevmanager = wl_registry_bind(registry, name,
				&wl_data_device_manager_interface, 1);
	} else if (strcmp(interface, "wl_output") == 0) {
		/* bind to outputs so we can get surface enter events */
		wl_registry_bind(registry, name, &wl_output_interface, 2);
	}
}

void
wlzoom(const Arg *arg)
{
	Arg larg;

	larg.f = usedfontsize + arg->f;
	wlzoomabs(&larg);
}

void
wlzoomabs(const Arg *arg)
{
	wlunloadfonts();
	wlloadfonts(usedfont, arg->f);
	cresize(0, 0);
	ttyresize();
	redraw();
	/* XXX: Should the window size be updated here because wayland doesn't
	 * have a notion of hints?
	 * xhints();
	 */
}

void
wlzoomreset(const Arg *arg)
{
	Arg larg;

	if (defaultfontsize > 0) {
		larg.f = defaultfontsize;
		wlzoomabs(&larg);
	}
}

void
wlinit(void)
{
	struct wl_registry *registry;

	if (!(wl.dpy = wl_display_connect(NULL)))
		die("Can't open display\n");

	registry = wl_display_get_registry(wl.dpy);
	wl_registry_add_listener(registry, &reglistener, NULL);
	wld.ctx = wld_wayland_create_context(wl.dpy, WLD_ANY);
	wld.renderer = wld_create_renderer(wld.ctx);

	wl_display_roundtrip(wl.dpy);

	if (!wl.shm)
		die("Display has no SHM\n");
	if (!wl.seat)
		die("Display has no seat\n");
	if (!wl.datadevmanager)
		die("Display has no data device manager\n");

	wl.keyboard = wl_seat_get_keyboard(wl.seat);
	wl_keyboard_add_listener(wl.keyboard, &kbdlistener, NULL);
	wl.pointer = wl_seat_get_pointer(wl.seat);
	wl_pointer_add_listener(wl.pointer, &ptrlistener, NULL);
	wl.datadev = wl_data_device_manager_get_data_device(wl.datadevmanager,
			wl.seat);
	wl_data_device_add_listener(wl.datadev, &datadevlistener, NULL);

	/* font */
	if (!FcInit())
		die("Could not init fontconfig.\n");

	usedfont = (opt_font == NULL)? font : opt_font;
	wld.fontctx = wld_font_create_context();
	wlloadfonts(usedfont, 0);

	wlloadcols();
	wlloadcursor();

	wl.vis = 0;
	wl.h = 2 * borderpx + term.row * wl.ch;
	wl.w = 2 * borderpx + term.col * wl.cw;

	wl.surface = wl_compositor_create_surface(wl.cmp);
	wl_surface_add_listener(wl.surface, &surflistener, NULL);

	wl.xdgsurface = xdg_shell_get_xdg_surface(wl.shell, wl.surface);
	xdg_surface_add_listener(wl.xdgsurface, &xdgsurflistener, NULL);
	xdg_surface_set_app_id(wl.xdgsurface, opt_class ? opt_class : termname);

	wl.xkb.ctx = xkb_context_new(0);
	wlresettitle();
}

/*
 * TODO: Implement something like XftDrawGlyphFontSpec in wld, and then apply a
 * similar patch to ae1923d27533ff46400d93765e971558201ca1ee
 */

void
wldraws(char *s, Glyph base, int x, int y, int charlen, int bytelen)
{
	int winx = borderpx + x * wl.cw, winy = borderpx + y * wl.ch,
	    width = charlen * wl.cw, xp, i;
	int frcflags, charexists;
	int u8fl, u8fblen, u8cblen, doesexist;
	char *u8c, *u8fs;
	Rune unicodep;
	Font *font = &dc.font;
	FcResult fcres;
	FcPattern *fcpattern, *fontpattern;
	FcFontSet *fcsets[] = { NULL };
	FcCharSet *fccharset;
	uint32_t fg, bg, temp;
	int oneatatime;

	frcflags = FRC_NORMAL;

	/* Fallback on color display for attributes not supported by the font */
	if (base.mode & ATTR_ITALIC && base.mode & ATTR_BOLD) {
		if (dc.ibfont.badslant || dc.ibfont.badweight)
			base.fg = defaultattr;
		font = &dc.ibfont;
		frcflags = FRC_ITALICBOLD;
	} else if (base.mode & ATTR_ITALIC) {
		if (dc.ifont.badslant)
			base.fg = defaultattr;
		font = &dc.ifont;
		frcflags = FRC_ITALIC;
	} else if (base.mode & ATTR_BOLD) {
		if (dc.bfont.badweight)
			base.fg = defaultattr;
		font = &dc.ifont;
		frcflags = FRC_BOLD;
	}

	if (IS_TRUECOL(base.fg)) {
		fg = base.fg | 0xff000000;
	} else {
		fg = dc.col[base.fg];
	}

	if (IS_TRUECOL(base.bg)) {
		bg = base.bg | 0xff000000;
	} else {
		bg = dc.col[base.bg];
	}

	if (base.mode & ATTR_BOLD) {
		/*
		 * change basic system colors [0-7]
		 * to bright system colors [8-15]
		 */
		if (BETWEEN(base.fg, 0, 7) && !(base.mode & ATTR_FAINT))
			fg = dc.col[base.fg + 8];

		if (base.mode & ATTR_ITALIC) {
			font = &dc.ibfont;
			frcflags = FRC_ITALICBOLD;
		} else {
			font = &dc.bfont;
			frcflags = FRC_BOLD;
		}
	}

	if (IS_SET(MODE_REVERSE)) {
		if (fg == dc.col[defaultfg]) {
			fg = dc.col[defaultbg];
		} else {
			fg = ~(fg & 0xffffff);
		}

		if (bg == dc.col[defaultbg]) {
			bg = dc.col[defaultfg];
		} else {
			bg = ~(bg & 0xffffff);
		}
	}

	if (base.mode & ATTR_REVERSE) {
		temp = fg;
		fg = bg;
		bg = temp;
	}

	if (base.mode & ATTR_FAINT && !(base.mode & ATTR_BOLD)) {
		fg = (fg & (0xff << 24))
			| ((((fg >> 16) & 0xff) / 2) << 16)
			| ((((fg >> 8) & 0xff) / 2) << 8)
			| ((fg & 0xff) / 2);
	}

	if (base.mode & ATTR_BLINK && term.mode & MODE_BLINK)
		fg = bg;

	if (base.mode & ATTR_INVISIBLE)
		fg = bg;

	/* Intelligent cleaning up of the borders. */
	if (x == 0) {
		wlclear(0, (y == 0)? 0 : winy, borderpx,
			((y >= term.row-1)? wl.h : (winy + wl.ch)));
	}
	if (x + charlen >= term.col) {
		wlclear(winx + width, (y == 0)? 0 : winy, wl.w,
			((y >= term.row-1)? wl.h : (winy + wl.ch)));
	}
	if (y == 0)
		wlclear(winx, 0, winx + width, borderpx);
	if (y == term.row-1)
		wlclear(winx, winy + wl.ch, winx + width, wl.h);

	/* Clean up the region we want to draw to. */
	wld_fill_rectangle(wld.renderer, bg, winx, winy, width, wl.ch);

	for (xp = winx; bytelen > 0;) {
		/*
		 * Search for the range in the to be printed string of glyphs
		 * that are in the main font. Then print that range. If
		 * some glyph is found that is not in the font, do the
		 * fallback dance.
		 */
		u8fs = s;
		u8fblen = 0;
		u8fl = 0;
		oneatatime = font->width != wl.cw;
		for (;;) {
			u8c = s;
			u8cblen = utf8decode(s, &unicodep, UTF_SIZ);
			s += u8cblen;
			bytelen -= u8cblen;

			doesexist = wld_font_ensure_char(font->match, unicodep);
			if (doesexist) {
					u8fl++;
					u8fblen += u8cblen;
					if (!oneatatime && bytelen > 0)
							continue;
			}

			if (u8fl > 0) {
				wld_draw_text(wld.renderer,
						font->match, fg, xp,
						winy + font->ascent,
						u8fs, u8fblen, NULL);
				xp += wl.cw * u8fl;
			}
			break;
		}
		if (doesexist) {
			if (oneatatime)
				continue;
			break;
		}

		/* Search the font cache. */
		for (i = 0; i < frclen; i++) {
			charexists = wld_font_ensure_char(frc[i].font, unicodep);
			/* Everything correct. */
			if (charexists && frc[i].flags == frcflags)
				break;
			/* We got a default font for a not found glyph. */
			if (!charexists && frc[i].flags == frcflags \
					&& frc[i].unicodep == unicodep) {
				break;
			}
		}

		/* Nothing was found. */
		if (i >= frclen) {
			if (!font->set)
				font->set = FcFontSort(0, font->pattern,
				                       1, 0, &fcres);
			fcsets[0] = font->set;

			/*
			 * Nothing was found in the cache. Now use
			 * some dozen of Fontconfig calls to get the
			 * font for one single character.
			 *
			 * Xft and fontconfig are design failures.
			 */
			fcpattern = FcPatternDuplicate(font->pattern);
			fccharset = FcCharSetCreate();

			FcCharSetAddChar(fccharset, unicodep);
			FcPatternAddCharSet(fcpattern, FC_CHARSET,
					fccharset);
			FcPatternAddBool(fcpattern, FC_SCALABLE, 1);

			FcConfigSubstitute(0, fcpattern,
					FcMatchPattern);
			FcDefaultSubstitute(fcpattern);

			fontpattern = FcFontSetMatch(0, fcsets, 1,
					fcpattern, &fcres);

			/*
			 * Overwrite or create the new cache entry.
			 */
			if (frclen >= LEN(frc)) {
				frclen = LEN(frc) - 1;
				wld_font_close(frc[frclen].font);
				frc[frclen].unicodep = 0;
			}

			frc[frclen].font = wld_font_open_pattern(wld.fontctx,
					fontpattern);
			frc[frclen].flags = frcflags;
			frc[frclen].unicodep = unicodep;

			i = frclen;
			frclen++;

			FcPatternDestroy(fcpattern);
			FcCharSetDestroy(fccharset);
		}

		wld_draw_text(wld.renderer, frc[i].font, fg,
				xp, winy + frc[i].font->ascent,
				u8c, u8cblen, NULL);

		xp += wl.cw * wcwidth(unicodep);
	}

	if (base.mode & ATTR_UNDERLINE) {
		wld_fill_rectangle(wld.renderer, fg, winx, winy + font->ascent + 1,
				width, 1);
	}

	if (base.mode & ATTR_STRUCK) {
		wld_fill_rectangle(wld.renderer, fg, winx, winy + 2 * font->ascent / 3,
				width, 1);
	}
}

void
wlunloadfont(Font *f)
{
	wld_font_close(f->match);
	FcPatternDestroy(f->pattern);
	if (f->set)
		FcFontSetDestroy(f->set);
}

void
wlunloadfonts(void)
{
	/* Free the loaded fonts in the font cache.  */
	while (frclen > 0)
		wld_font_close(frc[--frclen].font);

	wlunloadfont(&dc.font);
	wlunloadfont(&dc.bfont);
	wlunloadfont(&dc.ifont);
	wlunloadfont(&dc.ibfont);
}


void
wldrawglyph(Glyph g, int x, int y)
{
	static char buf[UTF_SIZ];
	size_t len = utf8encode(g.u, buf);
	int width = g.mode & ATTR_WIDE ? 2 : 1;

	wldraws(buf, g, x, y, width, len);
}

void
wldrawcursor(void)
{
	static int oldx = 0, oldy = 0;
	int curx;
	Glyph g = {' ', ATTR_NULL, defaultbg, defaultcs}, og;
	int ena_sel = sel.ob.x != -1 && sel.alt == IS_SET(MODE_ALTSCREEN);
	uint32_t drawcol;

	LIMIT(oldx, 0, term.col-1);
	LIMIT(oldy, 0, term.row-1);

	curx = term.c.x;

	/* adjust position if in dummy */
	if (term.line[oldy][oldx].mode & ATTR_WDUMMY)
		oldx--;
	if (term.line[term.c.y][curx].mode & ATTR_WDUMMY)
		curx--;

	/* remove the old cursor */
	og = term.line[oldy][oldx];
	if (ena_sel && selected(oldx, oldy))
		og.mode ^= ATTR_REVERSE;
	wldrawglyph(og, oldx, oldy);
	if (oldx != curx || oldy != term.c.y) {
		wl_surface_damage(wl.surface, borderpx + oldx * wl.cw,
				borderpx + oldy * wl.ch, wl.cw, wl.ch);
	}

	g.u = term.line[term.c.y][term.c.x].u;

	/*
	 * Select the right color for the right mode.
	 */
	if (IS_SET(MODE_REVERSE)) {
		g.mode |= ATTR_REVERSE;
		g.bg = defaultfg;
		if (ena_sel && selected(term.c.x, term.c.y)) {
			drawcol = dc.col[defaultcs];
			g.fg = defaultrcs;
		} else {
			drawcol = dc.col[defaultrcs];
			g.fg = defaultcs;
		}
	} else {
		if (ena_sel && selected(term.c.x, term.c.y)) {
			drawcol = dc.col[defaultrcs];
			g.fg = defaultfg;
			g.bg = defaultrcs;
		} else {
			drawcol = dc.col[defaultcs];
		}
	}

	if (IS_SET(MODE_HIDE))
		return;

	/* draw the new one */
	if (wl.state & WIN_FOCUSED) {
		switch (wl.cursor) {
		case 7: /* st extension: snowman */
			utf8decode("☃", &g.u, UTF_SIZ);
		case 0: /* Blinking Block */
		case 1: /* Blinking Block (Default) */
		case 2: /* Steady Block */
			g.mode |= term.line[term.c.y][curx].mode & ATTR_WIDE;
			wldrawglyph(g, term.c.x, term.c.y);
			break;
		case 3: /* Blinking Underline */
		case 4: /* Steady Underline */
			wld_fill_rectangle(wld.renderer, drawcol,
					borderpx + curx * wl.cw,
					borderpx + (term.c.y + 1) * wl.ch - \
						cursorthickness,
					wl.cw, cursorthickness);
			break;
		case 5: /* Blinking bar */
		case 6: /* Steady bar */
			wld_fill_rectangle(wld.renderer, drawcol,
					borderpx + curx * wl.cw,
					borderpx + term.c.y * wl.ch,
					cursorthickness, wl.ch);
			break;
		}
	} else {
		wld_fill_rectangle(wld.renderer, drawcol,
				borderpx + curx * wl.cw,
				borderpx + term.c.y * wl.ch,
				wl.cw - 1, 1);
		wld_fill_rectangle(wld.renderer, drawcol,
				borderpx + curx * wl.cw,
				borderpx + term.c.y * wl.ch,
				1, wl.ch - 1);
		wld_fill_rectangle(wld.renderer, drawcol,
				borderpx + (curx + 1) * wl.cw - 1,
				borderpx + term.c.y * wl.ch,
				1, wl.ch - 1);
		wld_fill_rectangle(wld.renderer, drawcol,
				borderpx + curx * wl.cw,
				borderpx + (term.c.y + 1) * wl.ch - 1,
				wl.cw, 1);
	}
	wl_surface_damage(wl.surface, borderpx + curx * wl.cw,
			borderpx + term.c.y * wl.ch, wl.cw, wl.ch);
	oldx = curx, oldy = term.c.y;
}

void
wlsettitle(char *title)
{
	xdg_surface_set_title(wl.xdgsurface, title);
}

void
wlresettitle(void)
{
	wlsettitle(opt_title ? opt_title : "st");
}

void
redraw(void)
{
	tfulldirt();
}

void
draw(void)
{
	int y, y0;

	for (y = 0; y <= term.bot; ++y) {
		if (!term.dirty[y])
			continue;
		for (y0 = y; y <= term.bot && term.dirty[y]; ++y);
		wl_surface_damage(wl.surface, 0, borderpx + y0 * wl.ch,
				wl.w, (y - y0) * wl.ch);
	}

	wld_set_target_buffer(wld.renderer, wld.buffer);
	drawregion(0, 0, term.col, term.row);
	wl.framecb = wl_surface_frame(wl.surface);
	wl_callback_add_listener(wl.framecb, &framelistener, NULL);
	wld_flush(wld.renderer);
	wl_surface_attach(wl.surface, wl.buffer, 0, 0);
	wl_surface_commit(wl.surface);
	/* need to wait to destroy the old buffer until we commit the new
	 * buffer */
	if (wld.oldbuffer) {
		wld_buffer_unreference(wld.oldbuffer);
		wld.oldbuffer = 0;
	}
	needdraw = false;
}

void
drawregion(int x1, int y1, int x2, int y2)
{
	int ic, ib, x, y, ox;
	Glyph base, new;
	char buf[DRAW_BUF_SIZ];
	int ena_sel = sel.ob.x != -1 && sel.alt == IS_SET(MODE_ALTSCREEN);

	for (y = y1; y < y2; y++) {
		if (!term.dirty[y])
			continue;

		term.dirty[y] = 0;
		base = term.line[y][0];
		ic = ib = ox = 0;
		for (x = x1; x < x2; x++) {
			new = term.line[y][x];
			if (new.mode == ATTR_WDUMMY)
				continue;
			if (ena_sel && selected(x, y))
				new.mode ^= ATTR_REVERSE;
			if (ib > 0 && (ATTRCMP(base, new)
					|| ib >= DRAW_BUF_SIZ-UTF_SIZ)) {
				wldraws(buf, base, ox, y, ic, ib);
				ic = ib = 0;
			}
			if (ib == 0) {
				ox = x;
				base = new;
			}

			ib += utf8encode(new.u, buf+ib);
			ic += (new.mode & ATTR_WIDE)? 2 : 1;
		}
		if (ib > 0)
			wldraws(buf, base, ox, y, ic, ib);
	}
	wldrawcursor();
}

void
wlseturgency(int add)
{
	/* XXX: no urgency equivalent yet in wayland */
}

void
wlmousereportmotion(wl_fixed_t fx, wl_fixed_t fy)
{
	int x = x2col(wl_fixed_to_int(fx)), y = y2row(wl_fixed_to_int(fy));

	if (x == oldx && y == oldy)
		return;
	if (!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY))
		return;
	/* MOUSE_MOTION: no reporting if no button is pressed */
	if (IS_SET(MODE_MOUSEMOTION) && oldbutton == 3)
		return;

	oldx = x;
	oldy = y;
	wlmousereport(oldbutton + 32, false, x, y);
}

void
wlmousereportaxis(uint32_t axis, wl_fixed_t amount)
{
	wlmousereport(64 + (axis == AXIS_VERTICAL ? 4 : 6)
		+ (amount > 0 ? 1 : 0), false, oldx, oldy);
}

int
x2col(int x)
{
	x -= borderpx;
	x /= wl.cw;

	return LIMIT(x, 0, term.col-1);
}

int
y2row(int y)
{
	y -= borderpx;
	y /= wl.ch;

	return LIMIT(y, 0, term.row-1);
}

void
getbuttoninfo(void)
{
	int type;
	uint state = wl.xkb.mods & ~forceselmod;

	sel.alt = IS_SET(MODE_ALTSCREEN);

	sel.oe.x = x2col(wl.px);
	sel.oe.y = y2row(wl.py);
	selnormalize();

	sel.type = SEL_REGULAR;
	for (type = 1; type < LEN(selmasks); ++type) {
		if (match(selmasks[type], state)) {
			sel.type = type;
			break;
		}
	}
}

