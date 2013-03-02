#include "pdfapp.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#define mupdf_icon_bitmap_16_width 16
#define mupdf_icon_bitmap_16_height 16
static unsigned char mupdf_icon_bitmap_16_bits[] = {
	0x00, 0x00, 0x00, 0x1e, 0x00, 0x2b, 0x80, 0x55, 0x8c, 0x62, 0x8c, 0x51,
	0x9c, 0x61, 0x1c, 0x35, 0x3c, 0x1f, 0x3c, 0x0f, 0xfc, 0x0f, 0xec, 0x0d,
	0xec, 0x0d, 0xcc, 0x0c, 0xcc, 0x0c, 0x00, 0x00 };

#define mupdf_icon_bitmap_16_mask_width 16
#define mupdf_icon_bitmap_16_mask_height 16
static unsigned char mupdf_icon_bitmap_16_mask_bits[] = {
	0x00, 0x1e, 0x00, 0x3f, 0x80, 0x7f, 0xce, 0xff, 0xde, 0xff, 0xde, 0xff,
	0xfe, 0xff, 0xfe, 0x7f, 0xfe, 0x3f, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe, 0x1f,
	0xfe, 0x1f, 0xfe, 0x1f, 0xfe, 0x1f, 0xce, 0x1c };

#ifndef timeradd
#define timeradd(a, b, result) \
	do { \
		(result)->tv_sec = (a)->tv_sec + (b)->tv_sec; \
		(result)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
		if ((result)->tv_usec >= 1000000) \
		{ \
			++(result)->tv_sec; \
			(result)->tv_usec -= 1000000; \
		} \
	} while (0)
#endif

#ifndef timersub
#define timersub(a, b, result) \
	do { \
		(result)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
		(result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
		if ((result)->tv_usec < 0) { \
			--(result)->tv_sec; \
			(result)->tv_usec += 1000000; \
		} \
	} while (0)
#endif

extern int ximage_init(Display *display, int screen, Visual *visual);
extern int ximage_get_depth(void);
extern Visual *ximage_get_visual(void);
extern Colormap ximage_get_colormap(void);
extern void ximage_blit(Drawable d, GC gc, int dstx, int dsty,
	unsigned char *srcdata,
	int srcx, int srcy, int srcw, int srch, int srcstride);

void windrawstringxor(pdfapp_t *app, int x, int y, char *s);
void cleanup(pdfapp_t *app);

static Display *xdpy;
static Atom XA_TARGETS;
static Atom XA_TIMESTAMP;
static Atom XA_UTF8_STRING;
static Atom WM_DELETE_WINDOW;
static Atom NET_WM_STATE;
static Atom NET_WM_STATE_FULLSCREEN;
static int x11fd;
static int xscr;
static Window xwin;
static Pixmap xicon, xmask;
static GC xgc;
static XEvent xevt;
static int mapped = 0;
static Cursor xcarrow, xchand, xcwait, xccaret;
static int justcopied = 0;
static int dirty = 0;
static int dirtysearch = 0;
static char *password = "";
static XColor xbgcolor;
static XColor xshcolor;
static int reqw = 0;
static int reqh = 0;
static char copylatin1[1024 * 16] = "";
static char copyutf8[1024 * 48] = "";
static Time copytime;
static char *filename;

#define FONTNAME "6x13"
static XFontStruct *font;
static int fdescent;
static int fheight;

static pdfapp_t gapp;
static int closing = 0;
static int reloading = 0;
static int showingpage = 0;

static int advance_scheduled = 0;
static struct timeval tmo_advance;

/*
 * Dialog boxes
 */

void winerror(pdfapp_t *app, char *msg)
{
	fprintf(stderr, "mupdf: error: %s\n", msg);
	cleanup(app);
	exit(1);
}

void winwarn(pdfapp_t *app, char *msg)
{
	fprintf(stderr, "mupdf: warning: %s\n", msg);
}

void winalert(pdfapp_t *app, fz_alert_event *alert)
{
	fprintf(stderr, "Alert %s: %s", alert->title, alert->message);
	switch (alert->button_group_type)
	{
	case FZ_ALERT_BUTTON_GROUP_OK:
	case FZ_ALERT_BUTTON_GROUP_OK_CANCEL:
		alert->button_pressed = FZ_ALERT_BUTTON_OK;
		break;
	case FZ_ALERT_BUTTON_GROUP_YES_NO:
	case FZ_ALERT_BUTTON_GROUP_YES_NO_CANCEL:
		alert->button_pressed = FZ_ALERT_BUTTON_YES;
		break;
	}
}

void winprint(pdfapp_t *app)
{
	fprintf(stderr, "The MuPDF library supports printing, but this application currently does not");
}

char *winpassword(pdfapp_t *app, char *filename)
{
	char *r = password;
	password = NULL;
	return r;
}

char *wintextinput(pdfapp_t *app, char *inittext, int retry)
{
	static char buf[256];

	if (retry)
		return NULL;

	printf("> [%s] ", inittext);
	fgets(buf, sizeof buf, stdin);
	return buf;
}

int winchoiceinput(pdfapp_t *app, int nopts, char *opts[], int *nvals, char *vals[])
{
	/* FIXME: temporary dummy implementation */
	return 0;
}

/*
 * X11 magic
 */

static void winopen(void)
{
	XWMHints *wmhints;
	XClassHint *classhint;

	xdpy = XOpenDisplay(NULL);
	if (!xdpy)
		fz_throw(gapp.ctx, "cannot open display");

	XA_TARGETS = XInternAtom(xdpy, "TARGETS", False);
	XA_TIMESTAMP = XInternAtom(xdpy, "TIMESTAMP", False);
	XA_UTF8_STRING = XInternAtom(xdpy, "UTF8_STRING", False);
	WM_DELETE_WINDOW = XInternAtom(xdpy, "WM_DELETE_WINDOW", False);
	NET_WM_STATE = XInternAtom(xdpy, "_NET_WM_STATE", False);
	NET_WM_STATE_FULLSCREEN = XInternAtom(xdpy, "_NET_WM_STATE_FULLSCREEN", False);

	xscr = DefaultScreen(xdpy);

	ximage_init(xdpy, xscr, DefaultVisual(xdpy, xscr));

	xcarrow = XCreateFontCursor(xdpy, XC_left_ptr);
	xchand = XCreateFontCursor(xdpy, XC_hand2);
	xcwait = XCreateFontCursor(xdpy, XC_watch);
	xccaret = XCreateFontCursor(xdpy, XC_xterm);

	xbgcolor.red = 0x7000;
	xbgcolor.green = 0x7000;
	xbgcolor.blue = 0x7000;

	xshcolor.red = 0x4000;
	xshcolor.green = 0x4000;
	xshcolor.blue = 0x4000;

	XAllocColor(xdpy, DefaultColormap(xdpy, xscr), &xbgcolor);
	XAllocColor(xdpy, DefaultColormap(xdpy, xscr), &xshcolor);

	xwin = XCreateWindow(xdpy, DefaultRootWindow(xdpy),
		10, 10, 200, 100, 0,
		ximage_get_depth(),
		InputOutput,
		ximage_get_visual(),
		0,
		NULL);
	if (xwin == None)
		fz_throw(gapp.ctx, "cannot create window");

	XSetWindowColormap(xdpy, xwin, ximage_get_colormap());
	XSelectInput(xdpy, xwin,
		StructureNotifyMask | ExposureMask | KeyPressMask |
		PointerMotionMask | ButtonPressMask | ButtonReleaseMask);

	mapped = 0;

	xgc = XCreateGC(xdpy, xwin, 0, NULL);

	if ((font = XLoadQueryFont(xdpy, FONTNAME)) == NULL)
		winerror(&gapp, "cannot load font: " FONTNAME);
	XSetFont(xdpy, xgc, font->fid);
	fdescent = font->descent + 1;
	fheight = font->ascent + fdescent;

	XDefineCursor(xdpy, xwin, xcarrow);

	wmhints = XAllocWMHints();
	if (wmhints)
	{
		wmhints->flags = IconPixmapHint | IconMaskHint;
		xicon = XCreateBitmapFromData(xdpy, xwin,
			(char*)mupdf_icon_bitmap_16_bits,
			mupdf_icon_bitmap_16_width,
			mupdf_icon_bitmap_16_height);
		xmask = XCreateBitmapFromData(xdpy, xwin,
			(char*)mupdf_icon_bitmap_16_mask_bits,
			mupdf_icon_bitmap_16_mask_width,
			mupdf_icon_bitmap_16_mask_height);
		if (xicon && xmask)
		{
			wmhints->icon_pixmap = xicon;
			wmhints->icon_mask = xmask;
			XSetWMHints(xdpy, xwin, wmhints);
		}
		XFree(wmhints);
	}

	classhint = XAllocClassHint();
	if (classhint)
	{
		classhint->res_name = "mupdf";
		classhint->res_class = "MuPDF";
		XSetClassHint(xdpy, xwin, classhint);
		XFree(classhint);
	}

	XSetWMProtocols(xdpy, xwin, &WM_DELETE_WINDOW, 1);

	x11fd = ConnectionNumber(xdpy);
}

void winclose(pdfapp_t *app)
{
	closing = 1;
}

int winsavequery(pdfapp_t *app)
{
	/* FIXME: temporary dummy implementation */
	return DISCARD;
}

int wingetsavepath(pdfapp_t *app, char *buf, int len)
{
	/* FIXME: temporary dummy implementation */
	return 0;
}

void winreplacefile(char *source, char *target)
{
	rename(source, target);
}

void cleanup(pdfapp_t *app)
{
	fz_context *ctx = app->ctx;

	pdfapp_close(app);

	XDestroyWindow(xdpy, xwin);

	XFreePixmap(xdpy, xicon);

	XFreeCursor(xdpy, xccaret);
	XFreeCursor(xdpy, xcwait);
	XFreeCursor(xdpy, xchand);
	XFreeCursor(xdpy, xcarrow);

	XFreeGC(xdpy, xgc);

	XCloseDisplay(xdpy);

	fz_free_context(ctx);
}

static int winresolution()
{
	return DisplayWidth(xdpy, xscr) * 25.4 /
		DisplayWidthMM(xdpy, xscr) + 0.5;
}

void wincursor(pdfapp_t *app, int curs)
{
	if (curs == ARROW)
		XDefineCursor(xdpy, xwin, xcarrow);
	if (curs == HAND)
		XDefineCursor(xdpy, xwin, xchand);
	if (curs == WAIT)
		XDefineCursor(xdpy, xwin, xcwait);
	if (curs == CARET)
		XDefineCursor(xdpy, xwin, xccaret);
	XFlush(xdpy);
}

void wintitle(pdfapp_t *app, char *s)
{
	XStoreName(xdpy, xwin, s);
#ifdef X_HAVE_UTF8_STRING
	Xutf8SetWMProperties(xdpy, xwin, s, s, NULL, 0, NULL, NULL, NULL);
#else
	XmbSetWMProperties(xdpy, xwin, s, s, NULL, 0, NULL, NULL, NULL);
#endif
}

void winhelp(pdfapp_t *app)
{
	fprintf(stderr, "%s\n%s", pdfapp_version(app), pdfapp_usage(app));
}

void winresize(pdfapp_t *app, int w, int h)
{
	int image_w = fz_pixmap_width(gapp.ctx, gapp.image);
	int image_h = fz_pixmap_height(gapp.ctx, gapp.image);
	XWindowChanges values;
	int mask, width, height;

	mask = CWWidth | CWHeight;
	values.width = w;
	values.height = h;
	XConfigureWindow(xdpy, xwin, mask, &values);

	reqw = w;
	reqh = h;

	if (!mapped)
	{
		gapp.winw = w;
		gapp.winh = h;
		width = -1;
		height = -1;

		XMapWindow(xdpy, xwin);
		XFlush(xdpy);

		while (1)
		{
			XNextEvent(xdpy, &xevt);
			if (xevt.type == ConfigureNotify)
			{
				width = xevt.xconfigure.width;
				height = xevt.xconfigure.height;
			}
			if (xevt.type == MapNotify)
				break;
		}

		XSetForeground(xdpy, xgc, WhitePixel(xdpy, xscr));
		XFillRectangle(xdpy, xwin, xgc, 0, 0, image_w, image_h);
		XFlush(xdpy);

		if (width != reqw || height != reqh)
		{
			gapp.shrinkwrap = 0;
			dirty = 1;
			pdfapp_onresize(&gapp, width, height);
		}

		mapped = 1;
	}
}

void winfullscreen(pdfapp_t *app, int state)
{
	XEvent xev;
	xev.xclient.type = ClientMessage;
	xev.xclient.serial = 0;
	xev.xclient.send_event = True;
	xev.xclient.window = xwin;
	xev.xclient.message_type = NET_WM_STATE;
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = state;
	xev.xclient.data.l[1] = NET_WM_STATE_FULLSCREEN;
	xev.xclient.data.l[2] = 0;
	XSendEvent(xdpy, DefaultRootWindow(xdpy), False,
		SubstructureRedirectMask | SubstructureNotifyMask,
		&xev);
}

static void fillrect(int x, int y, int w, int h)
{
	if (w > 0 && h > 0)
		XFillRectangle(xdpy, xwin, xgc, x, y, w, h);
}

static void winblitsearch(pdfapp_t *app)
{
	if (gapp.isediting)
	{
		char buf[sizeof(gapp.search) + 10];
		snprintf(buf, sizeof(buf), "%c%s", gapp.searchdir < 0 ? '?' : '/', gapp.search);
		XSetForeground(xdpy, xgc, BlackPixel(xdpy, xscr));
		fillrect(0, gapp.winh - fheight, gapp.winw, fheight);
		windrawstring(&gapp, 2, gapp.winh - fdescent, buf);
	}
}

static void winblit(pdfapp_t *app)
{
	int image_w = fz_pixmap_width(gapp.ctx, gapp.image);
	int image_h = fz_pixmap_height(gapp.ctx, gapp.image);
	int image_n = fz_pixmap_components(gapp.ctx, gapp.image);
	unsigned char *image_samples = fz_pixmap_samples(gapp.ctx, gapp.image);
	int x0 = gapp.panx;
	int y0 = gapp.pany;
	int x1 = gapp.panx + image_w;
	int y1 = gapp.pany + image_h;

	XSetForeground(xdpy, xgc, xbgcolor.pixel);
	fillrect(0, 0, x0, gapp.winh);
	fillrect(x1, 0, gapp.winw - x1, gapp.winh);
	fillrect(0, 0, gapp.winw, y0);
	fillrect(0, y1, gapp.winw, gapp.winh - y1);

	XSetForeground(xdpy, xgc, xshcolor.pixel);
	fillrect(x0+2, y1, image_w, 2);
	fillrect(x1, y0+2, 2, image_h);

	if (gapp.iscopying || justcopied)
	{
		pdfapp_invert(&gapp, &gapp.selr);
		justcopied = 1;
	}

	pdfapp_inverthit(&gapp);

	if (image_n == 4)
		ximage_blit(xwin, xgc,
			x0, y0,
			image_samples,
			0, 0,
			image_w,
			image_h,
			image_w * image_n);
	else if (image_n == 2)
	{
		int i = image_w*image_h;
		unsigned char *color = malloc(i*4);
		if (color)
		{
			unsigned char *s = image_samples;
			unsigned char *d = color;
			for (; i > 0 ; i--)
			{
				d[2] = d[1] = d[0] = *s++;
				d[3] = *s++;
				d += 4;
			}
			ximage_blit(xwin, xgc,
				x0, y0,
				color,
				0, 0,
				image_w,
				image_h,
				image_w * 4);
			free(color);
		}
	}

	pdfapp_inverthit(&gapp);

	if (gapp.iscopying || justcopied)
	{
		pdfapp_invert(&gapp, &gapp.selr);
		justcopied = 1;
	}

	winblitsearch(app);

	if (showingpage)
	{
		char buf[42];
		snprintf(buf, sizeof buf, "Page %d/%d", gapp.pageno, gapp.pagecount);
		windrawstringxor(&gapp, 10, 20, buf);
	}
}

void winrepaint(pdfapp_t *app)
{
	dirty = 1;
}

void winrepaintsearch(pdfapp_t *app)
{
	dirtysearch = 1;
}

void winadvancetimer(pdfapp_t *app, float duration)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	memset(&tmo_advance, 0, sizeof(tmo_advance));
	tmo_advance.tv_sec = (int)duration;
	tmo_advance.tv_usec = 1000000 * (duration - tmo_advance.tv_sec);
	timeradd(&tmo_advance, &now, &tmo_advance);
	advance_scheduled = 1;
}

void windrawstringxor(pdfapp_t *app, int x, int y, char *s)
{
	int prevfunction;
	XGCValues xgcv;

	XGetGCValues(xdpy, xgc, GCFunction, &xgcv);
	prevfunction = xgcv.function;
	xgcv.function = GXxor;
	XChangeGC(xdpy, xgc, GCFunction, &xgcv);

	XSetForeground(xdpy, xgc, WhitePixel(xdpy, DefaultScreen(xdpy)));

	XDrawString(xdpy, xwin, xgc, x, y, s, strlen(s));
	XFlush(xdpy);

	XGetGCValues(xdpy, xgc, GCFunction, &xgcv);
	xgcv.function = prevfunction;
	XChangeGC(xdpy, xgc, GCFunction, &xgcv);
}

void windrawstring(pdfapp_t *app, int x, int y, char *s)
{
	XSetForeground(xdpy, xgc, WhitePixel(xdpy, DefaultScreen(xdpy)));
	XDrawString(xdpy, xwin, xgc, x, y, s, strlen(s));
}

void windocopy(pdfapp_t *app)
{
	unsigned short copyucs2[16 * 1024];
	char *latin1 = copylatin1;
	char *utf8 = copyutf8;
	unsigned short *ucs2;
	int ucs;

	pdfapp_oncopy(&gapp, copyucs2, 16 * 1024);

	for (ucs2 = copyucs2; ucs2[0] != 0; ucs2++)
	{
		ucs = ucs2[0];

		utf8 += fz_runetochar(utf8, ucs);

		if (ucs < 256)
			*latin1++ = ucs;
		else
			*latin1++ = '?';
	}

	*utf8 = 0;
	*latin1 = 0;

	XSetSelectionOwner(xdpy, XA_PRIMARY, xwin, copytime);

	justcopied = 1;
}

void onselreq(Window requestor, Atom selection, Atom target, Atom property, Time time)
{
	XEvent nevt;

	advance_scheduled = 0;

	if (property == None)
		property = target;

	nevt.xselection.type = SelectionNotify;
	nevt.xselection.send_event = True;
	nevt.xselection.display = xdpy;
	nevt.xselection.requestor = requestor;
	nevt.xselection.selection = selection;
	nevt.xselection.target = target;
	nevt.xselection.property = property;
	nevt.xselection.time = time;

	if (target == XA_TARGETS)
	{
		Atom atomlist[4];
		atomlist[0] = XA_TARGETS;
		atomlist[1] = XA_TIMESTAMP;
		atomlist[2] = XA_STRING;
		atomlist[3] = XA_UTF8_STRING;
		XChangeProperty(xdpy, requestor, property, target,
			32, PropModeReplace,
			(unsigned char *)atomlist, sizeof(atomlist)/sizeof(Atom));
	}

	else if (target == XA_STRING)
	{
		XChangeProperty(xdpy, requestor, property, target,
			8, PropModeReplace,
			(unsigned char *)copylatin1, strlen(copylatin1));
	}

	else if (target == XA_UTF8_STRING)
	{
		XChangeProperty(xdpy, requestor, property, target,
			8, PropModeReplace,
			(unsigned char *)copyutf8, strlen(copyutf8));
	}

	else
	{
		nevt.xselection.property = None;
	}

	XSendEvent(xdpy, requestor, False, 0, &nevt);
}

void winreloadfile(pdfapp_t *app)
{
	pdfapp_close(app);
	pdfapp_open(app, filename, 1);
}

void winopenuri(pdfapp_t *app, char *buf)
{
	char *browser = getenv("BROWSER");
	if (!browser)
	{
#ifdef __APPLE__
		browser = "open";
#else
		browser = "xdg-open";
#endif
	}
	if (fork() == 0)
	{
		execlp(browser, browser, buf, (char*)0);
		fprintf(stderr, "cannot exec '%s'\n", browser);
		exit(0);
	}
}

static void onkey(int c)
{
	advance_scheduled = 0;

	if (justcopied)
	{
		justcopied = 0;
		winrepaint(&gapp);
	}

	if (!gapp.isediting && c == 'P')
	{
		showingpage = 1;
		winrepaint(&gapp);
		return;
	}

	pdfapp_onkey(&gapp, c);
}

static void onmouse(int x, int y, int btn, int modifiers, int state)
{
	if (state != 0)
		advance_scheduled = 0;

	if (state != 0 && justcopied)
	{
		justcopied = 0;
		winrepaint(&gapp);
	}

	pdfapp_onmouse(&gapp, x, y, btn, modifiers, state);
}

static void signal_handler(int signal)
{
	if (signal == SIGHUP)
		reloading = 1;
}

static void usage(void)
{
	fprintf(stderr, "usage: mupdf [options] file.pdf [page]\n");
	fprintf(stderr, "\t-b -\tset anti-aliasing quality in bits (0=off, 8=best)\n");
	fprintf(stderr, "\t-p -\tpassword\n");
	fprintf(stderr, "\t-r -\tresolution\n");
	exit(1);
}

int main(int argc, char **argv)
{
	int c;
	int len;
	char buf[128];
	KeySym keysym;
	int oldx = 0;
	int oldy = 0;
	int resolution = -1;
	int pageno = 1;
	fd_set fds;
	int width = -1;
	int height = -1;
	fz_context *ctx;
	struct timeval tmo_at;
	struct timeval now;
	struct timeval tmo;
	struct timeval *timeout;
	struct timeval tmo_advance_delay;

	ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	if (!ctx)
	{
		fprintf(stderr, "cannot initialise context\n");
		exit(1);
	}

	while ((c = fz_getopt(argc, argv, "p:r:b:")) != -1)
	{
		switch (c)
		{
		case 'p': password = fz_optarg; break;
		case 'r': resolution = atoi(fz_optarg); break;
		case 'b': fz_set_aa_level(ctx, atoi(fz_optarg)); break;
		default: usage();
		}
	}

	if (argc - fz_optind == 0)
		usage();

	filename = argv[fz_optind++];

	if (argc - fz_optind == 1)
		pageno = atoi(argv[fz_optind++]);

	pdfapp_init(ctx, &gapp);

	winopen();

	if (resolution == -1)
		resolution = winresolution();
	if (resolution < MINRES)
		resolution = MINRES;
	if (resolution > MAXRES)
		resolution = MAXRES;

	gapp.transitions_enabled = 1;
	gapp.scrw = DisplayWidth(xdpy, xscr);
	gapp.scrh = DisplayHeight(xdpy, xscr);
	gapp.resolution = resolution;
	gapp.pageno = pageno;

	pdfapp_open(&gapp, filename, 0);

	FD_ZERO(&fds);

	signal(SIGHUP, signal_handler);

	tmo_at.tv_sec = 0;
	tmo_at.tv_usec = 0;

	while (!closing)
	{
		while (!closing && XPending(xdpy) && !dirty)
		{
			XNextEvent(xdpy, &xevt);

			switch (xevt.type)
			{
			case Expose:
				dirty = 1;
				break;

			case ConfigureNotify:
				if (gapp.image)
				{
					if (xevt.xconfigure.width != reqw ||
						xevt.xconfigure.height != reqh)
						gapp.shrinkwrap = 0;
				}
				width = xevt.xconfigure.width;
				height = xevt.xconfigure.height;

				break;

			case KeyPress:
				len = XLookupString(&xevt.xkey, buf, sizeof buf, &keysym, NULL);

				if (!gapp.isediting)
					switch (keysym)
					{
					case XK_Escape:
						len = 1; buf[0] = '\033';
						break;

					case XK_Up:
						len = 1; buf[0] = 'k';
						break;
					case XK_Down:
						len = 1; buf[0] = 'j';
						break;

					case XK_Left:
						len = 1; buf[0] = 'b';
						break;
					case XK_Right:
						len = 1; buf[0] = ' ';
						break;

					case XK_Page_Up:
						len = 1; buf[0] = ',';
						break;
					case XK_Page_Down:
						len = 1; buf[0] = '.';
						break;
					}
				if (len)
					onkey(buf[0]);

				onmouse(oldx, oldy, 0, 0, 0);

				break;

			case MotionNotify:
				oldx = xevt.xmotion.x;
				oldy = xevt.xmotion.y;
				onmouse(xevt.xmotion.x, xevt.xmotion.y, 0, xevt.xmotion.state, 0);
				break;

			case ButtonPress:
				onmouse(xevt.xbutton.x, xevt.xbutton.y, xevt.xbutton.button, xevt.xbutton.state, 1);
				break;

			case ButtonRelease:
				copytime = xevt.xbutton.time;
				onmouse(xevt.xbutton.x, xevt.xbutton.y, xevt.xbutton.button, xevt.xbutton.state, -1);
				break;

			case SelectionRequest:
				onselreq(xevt.xselectionrequest.requestor,
					xevt.xselectionrequest.selection,
					xevt.xselectionrequest.target,
					xevt.xselectionrequest.property,
					xevt.xselectionrequest.time);
				break;

			case ClientMessage:
				if (xevt.xclient.format == 32 && xevt.xclient.data.l[0] == WM_DELETE_WINDOW)
					closing = 1;
				break;
			}
		}

		if (closing)
			continue;

		if (width != -1 || height != -1)
		{
			pdfapp_onresize(&gapp, width, height);
			width = -1;
			height = -1;
		}

		if (dirty || dirtysearch)
		{
			if (dirty)
				winblit(&gapp);
			else if (dirtysearch)
				winblitsearch(&gapp);
			dirty = 0;
			dirtysearch = 0;
			pdfapp_postblit(&gapp);
		}

		if (showingpage && !tmo_at.tv_sec && !tmo_at.tv_usec)
		{
			tmo.tv_sec = 2;
			tmo.tv_usec = 0;

			gettimeofday(&now, NULL);
			timeradd(&now, &tmo, &tmo_at);
		}

		if (XPending(xdpy) || dirty)
			continue;

		timeout = NULL;

		if (tmo_at.tv_sec || tmo_at.tv_usec)
		{
			gettimeofday(&now, NULL);
			timersub(&tmo_at, &now, &tmo);
			if (tmo.tv_sec <= 0)
			{
				tmo_at.tv_sec = 0;
				tmo_at.tv_usec = 0;
				timeout = NULL;
				showingpage = 0;
				winrepaint(&gapp);
			}
			else
				timeout = &tmo;
		}

		if (advance_scheduled)
		{
			gettimeofday(&now, NULL);
			timersub(&tmo_advance, &now, &tmo_advance_delay);
			if (tmo_advance_delay.tv_sec <= 0)
			{
				/* Too late already */
				onkey(' ');
				onmouse(oldx, oldy, 0, 0, 0);
				advance_scheduled = 0;
			}
			else if (timeout == NULL)
			{
				timeout = &tmo_advance_delay;
			}
			else
			{
				struct timeval tmp;
				timersub(&tmo_advance_delay, timeout, &tmp);
				if (tmp.tv_sec < 0)
				{
					timeout = &tmo_advance_delay;
				}
			}
		}

		FD_SET(x11fd, &fds);
		if (select(x11fd + 1, &fds, NULL, NULL, timeout) < 0)
		{
			if (reloading)
			{
				winreloadfile(&gapp);
				reloading = 0;
			}
		}
		if (!FD_ISSET(x11fd, &fds))
		{
			if (timeout == &tmo_advance_delay)
			{
				onkey(' ');
				onmouse(oldx, oldy, 0, 0, 0);
				advance_scheduled = 0;
			}
			else
			{
				tmo_at.tv_sec = 0;
				tmo_at.tv_usec = 0;
				timeout = NULL;
				showingpage = 0;
				winrepaint(&gapp);
			}
		}
	}

	cleanup(&gapp);

	return 0;
}
