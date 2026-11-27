/* Same X11 overlay as just-talk.
   With a compositor: 32-bit premultiplied pixels via XPutImage.
   Without one (this machine): 24-bit window plus a rounded shape mask.
   A 32-bit window stays invisible when nothing is compositing. */
#define _GNU_SOURCE
#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <cairo/cairo-xlib.h>
#include <fontconfig/fontconfig.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

static int width = 122, height = 42;
static int visible = 0;
static int use_argb = 0;
static GC draw_gc = 0;
static Window held_stream = 0;
static unsigned long held_bypass = 1;
static double theme_bg[3] = {0x25 / 255.0, 0x25 / 255.0, 0x25 / 255.0};
static double theme_text[3] = {0xee / 255.0, 0xee / 255.0, 0xee / 255.0};
static double theme_accent[3] = {0x2b / 255.0, 0xd8 / 255.0, 0x9b / 255.0};
static time_t theme_mtime = (time_t)-1;

static int x_error(Display *display, XErrorEvent *event) {
  (void)display;
  (void)event;
  return 0;
}

static int compositor_running(Display *display, int screen) {
  char name[32];
  snprintf(name, sizeof name, "_NET_WM_CM_S%d", screen);
  Atom atom = XInternAtom(display, name, False);
  return XGetSelectionOwner(display, atom) != None;
}

static Visual *argb_visual(Display *display, int screen) {
  XVisualInfo query;
  query.screen = screen;
  query.depth = 32;
  query.class = TrueColor;
  int count = 0;
  XVisualInfo *infos = XGetVisualInfo(
      display, VisualScreenMask | VisualDepthMask | VisualClassMask, &query, &count);
  Visual *visual = NULL;
  for (int i = 0; i < count; i++) {
    XRenderPictFormat *format = XRenderFindVisualFormat(display, infos[i].visual);
    if (format && format->type == PictTypeDirect && format->direct.alphaMask) {
      visual = infos[i].visual;
      break;
    }
  }
  if (infos)
    XFree(infos);
  return visual;
}

static int class_has_moonlight(Display *display, Window win) {
  XClassHint hint = {0};
  if (!XGetClassHint(display, win, &hint))
    return 0;
  int match = (hint.res_class && strcasestr(hint.res_class, "moonlight")) ||
              (hint.res_name && strcasestr(hint.res_name, "moonlight"));
  if (hint.res_name)
    XFree(hint.res_name);
  if (hint.res_class)
    XFree(hint.res_class);
  return match;
}

static void consider_stream(Display *display, Window root, Window win) {
  if (!class_has_moonlight(display, win))
    return;
  XWindowAttributes attrs;
  if (!XGetWindowAttributes(display, win, &attrs) || attrs.map_state != IsViewable)
    return;
  if (attrs.width < 400 || attrs.height < 300)
    return;
  Atom bypass = XInternAtom(display, "_NET_WM_BYPASS_COMPOSITOR", False);
  Atom actual = None;
  int format = 0;
  unsigned long n = 0, left = 0;
  unsigned char *data = NULL;
  unsigned long value = 1;
  if (XGetWindowProperty(display, win, bypass, 0, 1, False, XA_CARDINAL, &actual, &format, &n, &left, &data) == Success && data) {
    if (n)
      value = *(unsigned long *)data;
    XFree(data);
  }
  if (!held_stream) {
    held_stream = win;
    held_bypass = value;
  }
  if (value != 0) {
    unsigned long off = 0;
    XChangeProperty(display, win, bypass, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&off, 1);
  }
}

static void walk_streams(Display *display, Window root, Window win, int depth) {
  consider_stream(display, root, win);
  if (depth <= 0)
    return;
  Window root_return, parent, *children = NULL;
  unsigned count = 0;
  if (!XQueryTree(display, win, &root_return, &parent, &children, &count))
    return;
  for (unsigned i = 0; i < count; i++)
    walk_streams(display, root, children[i], depth - 1);
  if (children)
    XFree(children);
}

static void hold_stream(Display *display, int screen) {
  walk_streams(display, RootWindow(display, screen), RootWindow(display, screen), 4);
}

static void release_stream(Display *display) {
  if (!held_stream)
    return;
  Atom bypass = XInternAtom(display, "_NET_WM_BYPASS_COMPOSITOR", False);
  XChangeProperty(display, held_stream, bypass, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&held_bypass, 1);
  held_stream = 0;
}

static void monitor_of_focus(Display *display, int screen, int *x, int *y, int *w, int *h) {
  *x = 0;
  *y = 0;
  *w = DisplayWidth(display, screen);
  *h = DisplayHeight(display, screen);
  Window focus = None;
  int revert = 0;
  XGetInputFocus(display, &focus, &revert);
  if (focus == None || focus == PointerRoot)
    return;
  XWindowAttributes attrs;
  if (!XGetWindowAttributes(display, focus, &attrs))
    return;
  Window child = 0;
  int fx = 0, fy = 0;
  if (!XTranslateCoordinates(display, focus, RootWindow(display, screen), attrs.width / 2, attrs.height / 2, &fx, &fy, &child))
    return;
  int count = 0;
  XineramaScreenInfo *screens = XineramaQueryScreens(display, &count);
  if (!screens)
    return;
  for (int i = 0; i < count; i++) {
    int sx = screens[i].x_org, sy = screens[i].y_org;
    int sw = screens[i].width, sh = screens[i].height;
    if (fx >= sx && fx < sx + sw && fy >= sy && fy < sy + sh) {
      *x = sx;
      *y = sy;
      *w = sw;
      *h = sh;
      break;
    }
  }
  XFree(screens);
}

static void apply_shape(Display *display, Window window) {
  /* Capsule, same as just-talk: the corner radius is half the height. */
  int radius = height / 2;
  if (radius > width / 2)
    radius = width / 2;
  if (radius < 1)
    radius = 1;
  int stride = (width + 7) / 8;
  char *bits = calloc((size_t)stride * (size_t)height, 1);
  if (!bits)
    return;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int inside = 1;
      if (x < radius || x >= width - radius) {
        int cx = x < radius ? radius : width - radius - 1;
        int cy = y < height / 2 ? radius : height - radius - 1;
        int dx = x - cx, dy = y - cy;
        inside = dx * dx + dy * dy <= radius * radius;
      }
      if (inside)
        bits[y * stride + x / 8] |= (char)(1 << (x % 8));
    }
  }
  Pixmap mask = XCreateBitmapFromData(display, window, bits, (unsigned)width, (unsigned)height);
  free(bits);
  if (!mask)
    return;
  XShapeCombineMask(display, window, ShapeBounding, 0, 0, mask, ShapeSet);
  XFreePixmap(display, mask);
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double radius) {
  if (radius > h / 2)
    radius = h / 2;
  if (radius > w / 2)
    radius = w / 2;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - radius, y + radius, radius, -M_PI / 2, 0);
  cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, M_PI / 2);
  cairo_arc(cr, x + radius, y + h - radius, radius, M_PI / 2, M_PI);
  cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

/* Same size the candidate bar uses: the configured point size at Xft.dpi.
   "Sans 13" at 144 DPI is 26 pixels, not 13. */
static double font_px = 26;
static char font_family[128] = "Sans";
static cairo_font_face_t *font_face = NULL;
static time_t font_mtime = (time_t)-1;

static double screen_dpi(Display *display) {
  double dpi = 96;
  const char *resources = XResourceManagerString(display);
  if (!resources)
    return dpi;
  const char *hit = strstr(resources, "Xft.dpi:");
  if (!hit)
    return dpi;
  hit += strlen("Xft.dpi:");
  while (*hit == ' ' || *hit == '\t')
    hit++;
  double value = strtod(hit, NULL);
  if (value >= 72 && value <= 400)
    return value;
  return dpi;
}

/* Cairo's toy font API draws the same pixel size with a heavier stroke than
   the Pango layout fcitx uses. A fontconfig face with slight hinting matches
   that stroke. */
static void build_font_face(void) {
  if (font_face) {
    cairo_font_face_destroy(font_face);
    font_face = NULL;
  }
  FcInit();
  FcPattern *pat = FcPatternCreate();
  if (!pat)
    return;
  FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)font_family);
  FcPatternAddDouble(pat, FC_PIXEL_SIZE, font_px);
  FcPatternAddBool(pat, FC_ANTIALIAS, FcTrue);
  FcPatternAddInteger(pat, FC_HINT_STYLE, FC_HINT_SLIGHT);
  FcPatternAddBool(pat, FC_HINTING, FcTrue);
  FcCharSet *han = FcCharSetCreate();
  if (han) {
    FcCharSetAddChar(han, 0x4e2d);
    FcPatternAddCharSet(pat, FC_CHARSET, han);
    FcCharSetDestroy(han);
  }
  FcConfigSubstitute(NULL, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult result;
  FcPattern *match = FcFontMatch(NULL, pat, &result);
  if (match) {
    font_face = cairo_ft_font_face_create_for_pattern(match);
    FcPatternDestroy(match);
  }
  FcPatternDestroy(pat);
}

static void load_font(Display *display) {
  const char *home = getenv("HOME");
  char path[512];
  snprintf(path, sizeof path, "%s/.config/fcitx5/conf/classicui.conf", home ? home : "");
  struct stat st;
  int have = stat(path, &st) == 0;
  if (have && st.st_mtime == font_mtime && font_face)
    return;
  font_mtime = have ? st.st_mtime : 0;
  double pt = 13;
  snprintf(font_family, sizeof font_family, "Sans");
  FILE *file = fopen(path, "r");
  if (file) {
    char line[256];
    while (fgets(line, sizeof line, file)) {
      if (strncmp(line, "Font=", 5) != 0)
        continue;
      char *cursor = line + 5;
      while (*cursor == ' ' || *cursor == '\t')
        cursor++;
      size_t n = strcspn(cursor, "\r\n");
      while (n && (cursor[n - 1] == ' ' || cursor[n - 1] == '\t'))
        n--;
      char spec[128];
      if (n >= sizeof spec)
        n = sizeof spec - 1;
      memcpy(spec, cursor, n);
      spec[n] = 0;
      char *size = spec + n;
      while (size > spec && ((size[-1] >= '0' && size[-1] <= '9') || size[-1] == '.'))
        size--;
      if (size < spec + n && size > spec && size[-1] == ' ') {
        double value = strtod(size, NULL);
        if (value >= 8 && value <= 72)
          pt = value;
        size--;
        while (size > spec && (size[-1] == ' ' || size[-1] == '\t'))
          size--;
        *size = 0;
        if (spec[0])
          snprintf(font_family, sizeof font_family, "%s", spec);
      }
    }
    fclose(file);
  }
  font_px = pt * screen_dpi(display) / 72.0;
  build_font_face();
}

static int parse_hex(const char *s, double out[3]) {
  if (*s == '#')
    s++;
  unsigned value = 0;
  for (int i = 0; i < 6; i++) {
    char c = s[i];
    int digit = -1;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    if (digit < 0)
      return 0;
    value = (value << 4) | (unsigned)digit;
  }
  out[0] = ((value >> 16) & 255) / 255.0;
  out[1] = ((value >> 8) & 255) / 255.0;
  out[2] = (value & 255) / 255.0;
  return 1;
}

static void conf_color(const char *path, const char *key, double out[3]) {
  FILE *file = fopen(path, "r");
  if (!file)
    return;
  char line[256];
  size_t key_len = strlen(key);
  while (fgets(line, sizeof line, file)) {
    if (strncmp(line, key, key_len) != 0 || line[key_len] != '=')
      continue;
    parse_hex(line + key_len + 1, out);
  }
  fclose(file);
}

static void load_panel(const char *path) {
  FILE *file = fopen(path, "r");
  if (!file)
    return;
  char buf[4096];
  size_t n = fread(buf, 1, sizeof buf - 1, file);
  fclose(file);
  buf[n] = 0;
  char *stroke = strstr(buf, "stroke=\"#");
  if (!stroke)
    return;
  char *fill = NULL;
  for (char *p = buf; p < stroke; p++) {
    if (strncmp(p, "fill=\"#", 7) == 0)
      fill = p;
  }
  if (fill)
    parse_hex(fill + 6, theme_bg);
}

static int theme_path(const char *name, const char *file, char *out, size_t out_len) {
  const char *home = getenv("HOME");
  if (home) {
    snprintf(out, out_len, "%s/.local/share/fcitx5/themes/%s/%s", home, name, file);
    if (access(out, R_OK) == 0)
      return 1;
  }
  snprintf(out, out_len, "/usr/share/fcitx5/themes/%s/%s", name, file);
  return access(out, R_OK) == 0;
}

static void load_theme(void) {
  const char *home = getenv("HOME");
  char classic[512];
  snprintf(classic, sizeof classic, "%s/.config/fcitx5/conf/classicui.conf", home ? home : "");
  struct stat st;
  if (stat(classic, &st) == 0 && st.st_mtime == theme_mtime)
    return;
  theme_mtime = stat(classic, &st) == 0 ? st.st_mtime : 0;
  char theme_name[128] = "wetypex-dark";
  char dark_name[128] = "wetypex-dark";
  int follow = 0;
  FILE *file = fopen(classic, "r");
  if (file) {
    char line[256];
    while (fgets(line, sizeof line, file)) {
      if (strncmp(line, "Theme=", 6) == 0)
        sscanf(line + 6, "%127s", theme_name);
      else if (strncmp(line, "DarkTheme=", 10) == 0)
        sscanf(line + 10, "%127s", dark_name);
      else if (strncmp(line, "UseDarkTheme=True", 17) == 0)
        follow = 1;
    }
    fclose(file);
  }
  const char *name = theme_name;
  if (follow) {
    int dark = 0;
    if (home) {
      char gtk[512];
      snprintf(gtk, sizeof gtk, "%s/.config/gtk-3.0/settings.ini", home);
      FILE *settings = fopen(gtk, "r");
      if (settings) {
        char line[256];
        while (fgets(line, sizeof line, settings))
          if (strstr(line, "gtk-application-prefer-dark-theme") && strstr(line, "1"))
            dark = 1;
        fclose(settings);
      }
    }
    if (dark)
      name = dark_name;
  }
  char path[512];
  if (theme_path(name, "theme.conf", path, sizeof path)) {
    conf_color(path, "NormalColor", theme_text);
    conf_color(path, "HighlightCandidateColor", theme_accent);
  }
  if (theme_path(name, "panel.svg", path, sizeof path))
    load_panel(path);
}

static void use_font(cairo_t *cr) {
  if (!font_face)
    build_font_face();
  if (font_face)
    cairo_set_font_face(cr, font_face);
  cairo_set_font_size(cr, font_px);
}

#define MAX_KEEP 3
#define LINE_CAP 4096

typedef struct {
  char text[MAX_KEEP][LINE_CAP];
  int count;
  int text_w;
  int text_h;
  int line_h;
  int ascent;
} TextLayout;

static TextLayout laid;

static int codepoint_len(const unsigned char *s) {
  if ((*s & 0x80) == 0)
    return 1;
  if ((*s & 0xe0) == 0xc0)
    return 2;
  if ((*s & 0xf0) == 0xe0)
    return 3;
  if ((*s & 0xf8) == 0xf0)
    return 4;
  return 1;
}

static void push_line(TextLayout *layout, const char *s, int n) {
  if (n <= 0)
    return;
  if (n >= LINE_CAP)
    n = LINE_CAP - 1;
  if (layout->count == MAX_KEEP) {
    memmove(layout->text[0], layout->text[1], (size_t)(MAX_KEEP - 1) * LINE_CAP);
    layout->count = MAX_KEEP - 1;
  }
  memcpy(layout->text[layout->count], s, (size_t)n);
  layout->text[layout->count][n] = 0;
  layout->count++;
}

static int text_width(cairo_t *cr, const char *s) {
  if (!s || !s[0])
    return 0;
  cairo_text_extents_t extents;
  cairo_text_extents(cr, s, &extents);
  int width_px = (int)(extents.x_advance + 0.5);
  return width_px > 0 ? width_px : 0;
}

/* just-talk keeps the newest three wrapped lines and lets the older ones scroll off. */
static void layout_text(const char *src, int max_w) {
  memset(&laid, 0, sizeof laid);
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  use_font(cr);
  cairo_font_extents_t font;
  cairo_font_extents(cr, &font);
  laid.ascent = (int)(font.ascent + 0.5);
  laid.line_h = (int)(font.height + font_px * 0.22 + 0.5);
  if (laid.line_h < laid.ascent + 4)
    laid.line_h = laid.ascent + 4;
  if (src && src[0]) {
    char current[LINE_CAP];
    int current_n = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p) {
      if (*p == '\n' || *p == '\r') {
        current[current_n] = 0;
        push_line(&laid, current, current_n);
        current_n = 0;
        if (*p == '\r' && p[1] == '\n')
          p++;
        p++;
        continue;
      }
      int n = codepoint_len(p);
      char trial[LINE_CAP];
      if (current_n + n >= LINE_CAP) {
        current[current_n] = 0;
        push_line(&laid, current, current_n);
        current_n = 0;
      }
      memcpy(trial, current, (size_t)current_n);
      memcpy(trial + current_n, p, (size_t)n);
      trial[current_n + n] = 0;
      if (current_n > 0 && text_width(cr, trial) > max_w) {
        current[current_n] = 0;
        push_line(&laid, current, current_n);
        memcpy(current, p, (size_t)n);
        current_n = n;
      } else {
        memcpy(current + current_n, p, (size_t)n);
        current_n += n;
      }
      p += n;
    }
    current[current_n] = 0;
    push_line(&laid, current, current_n);
    for (int i = 0; i < laid.count; i++) {
      int line_w = text_width(cr, laid.text[i]);
      if (line_w > laid.text_w)
        laid.text_w = line_w;
    }
  }
  laid.text_h = laid.line_h * (laid.count > 0 ? laid.count : 1);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

static void draw_pill(cairo_t *cr, const char *state, const char *text, int opaque) {
  (void)state;
  /* just-talk draws one fill. A separate border sits inside the window edge,
     so the right side shows the stroke and the window edge as two borders. */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  if (opaque) {
    cairo_set_source_rgb(cr, theme_bg[0], theme_bg[1], theme_bg[2]);
    cairo_paint(cr);
  } else {
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    rounded(cr, 0, 0, width, height, height / 2.0);
    cairo_set_source_rgb(cr, theme_bg[0], theme_bg[1], theme_bg[2]);
    cairo_fill(cr);
  }
  cairo_set_source_rgb(cr, theme_accent[0], theme_accent[1], theme_accent[2]);
  double pad = font_px * 0.7;
  if (pad < 6)
    pad = 6;
  double dot = font_px * 0.22;
  if (dot < 3.5)
    dot = 3.5;
  cairo_arc(cr, pad + dot, height / 2.0, dot, 0, 2 * M_PI);
  cairo_fill(cr);
  if (laid.count > 0) {
    use_font(cr);
    cairo_set_source_rgb(cr, theme_text[0], theme_text[1], theme_text[2]);
    double x = pad + dot * 2 + font_px * 0.45;
    double block = laid.line_h * laid.count;
    double top = (height - block) / 2.0;
    if (top < pad * 0.35)
      top = pad * 0.35;
    double y = top + laid.ascent;
    for (int i = 0; i < laid.count; i++) {
      cairo_move_to(cr, x, y);
      cairo_show_text(cr, laid.text[i]);
      y += laid.line_h;
    }
  }
  (void)text;
}

static void paint(Display *display, Window window, Visual *visual, const char *state, const char *text) {
  if (!use_argb) {
    cairo_surface_t *surface = cairo_xlib_surface_create(display, window, visual, width, height);
    cairo_t *cr = cairo_create(surface);
    draw_pill(cr, state, text, 1);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XFlush(display);
    return;
  }
  cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(image);
  draw_pill(cr, state, text, 0);
  cairo_destroy(cr);
  cairo_surface_flush(image);
  char *data = (char *)cairo_image_surface_get_data(image);
  XImage *ximg = XCreateImage(display, visual, 32, ZPixmap, 0, data, (unsigned)width, (unsigned)height, 32, 0);
  if (ximg) {
    XPutImage(display, window, draw_gc, ximg, 0, 0, 0, 0, (unsigned)width, (unsigned)height);
    ximg->data = NULL;
    XDestroyImage(ximg);
  }
  cairo_surface_destroy(image);
  XFlush(display);
}

int main(void) {
  setlocale(LC_ALL, "");
  XSetErrorHandler(x_error);
  Display *display = XOpenDisplay(NULL);
  if (!display)
    return 1;
  int screen = DefaultScreen(display);
  Window root = RootWindow(display, screen);
  Visual *visual = NULL;
  int depth = 0;
  Colormap colormap = 0;
  use_argb = compositor_running(display, screen);
  if (use_argb)
    visual = argb_visual(display, screen);
  if (!visual)
    use_argb = 0;
  if (use_argb) {
    depth = 32;
    colormap = XCreateColormap(display, root, visual, AllocNone);
  } else {
    depth = DefaultDepth(display, screen);
    visual = DefaultVisual(display, screen);
    colormap = DefaultColormap(display, screen);
  }
  XSetWindowAttributes attrs;
  memset(&attrs, 0, sizeof attrs);
  attrs.colormap = colormap;
  attrs.override_redirect = True;
  attrs.save_under = True;
  attrs.background_pixel = 0;
  attrs.border_pixel = 0;
  Window window = XCreateWindow(
      display, root, 0, 0, (unsigned)width, (unsigned)height, 0, depth, InputOutput, visual,
      CWColormap | CWOverrideRedirect | CWSaveUnder | CWBackPixel | CWBorderPixel, &attrs);
  if (!use_argb) {
    XSetWindowAttributes again;
    again.override_redirect = True;
    again.save_under = True;
    XChangeWindowAttributes(display, window, CWOverrideRedirect | CWSaveUnder, &again);
    apply_shape(display, window);
  }
  Atom type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
  Atom utility = XInternAtom(display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
  XChangeProperty(display, window, type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&utility, 1);
  draw_gc = XCreateGC(display, window, 0, NULL);
  XSelectInput(display, window, 0);
  XShapeCombineRectangles(display, window, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
  /* Same withdrawn state Mutter records for the just-talk overlay. */
  Atom wm_state = XInternAtom(display, "WM_STATE", False);
  unsigned long withdrawn[2] = {0, 0};
  XChangeProperty(display, window, wm_state, wm_state, 32, PropModeReplace, (unsigned char *)withdrawn, 2);

  char line[8192];
  char state[64] = "recording";
  char text[8000] = "";
  while (1) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    struct timeval wait = {0, 40000};
    int ready = select(1, &fds, NULL, NULL, &wait);
    if (ready > 0) {
      if (!fgets(line, sizeof line, stdin))
        break;
      size_t len = strlen(line);
      while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = 0;
      if (strcmp(line, "hide") == 0) {
        if (visible) {
          XUnmapWindow(display, window);
          release_stream(display);
          XFlush(display);
          visible = 0;
        }
      } else if (strncmp(line, "show\t", 5) == 0) {
        char *sep = strchr(line + 5, '\t');
        if (sep) {
          *sep = 0;
          snprintf(state, sizeof state, "%s", line + 5);
          snprintf(text, sizeof text, "%s", sep + 1);
        } else {
          snprintf(state, sizeof state, "%s", line + 5);
          text[0] = 0;
        }
        load_font(display);
        load_theme();
        int max_text_w = (int)(font_px * 18);
        if (max_text_w < 180)
          max_text_w = 180;
        if (max_text_w > 640)
          max_text_w = 640;
        layout_text(text, max_text_w);
        double pad = font_px * 0.7;
        if (pad < 6)
          pad = 6;
        double dot = font_px * 0.46;
        if (dot < 8)
          dot = 8;
        double gap = font_px * 0.4;
        int chrome = (int)(pad * 2 + dot + gap);
        int next_w = chrome + (text[0] ? laid.text_w : (int)(font_px * 2.2));
        int next_h = laid.text_h + (int)(pad * 1.15);
        if (text[0] && (laid.count > 1 || laid.text_w > max_text_w - (int)font_px))
          next_w = chrome + max_text_w;
        if (next_w < chrome + 40)
          next_w = chrome + 40;
        if (next_h < (int)(font_px + pad))
          next_h = (int)(font_px + pad);
        int resized = next_w != width || next_h != height;
        if (resized) {
          width = next_w;
          height = next_h;
        }
        /* just-talk maps once. Mapping again on every update makes Mutter
           drop the window under Moonlight's unredirected stream. */
        int first = !visible;
        visible = 1;
        if (first)
          hold_stream(display, screen);
        int origin_x = 0, origin_y = 0, area_w = 0, area_h = 0;
        monitor_of_focus(display, screen, &origin_x, &origin_y, &area_w, &area_h);
        int x = origin_x + (area_w - width) / 2;
        int y = origin_y + area_h - height - 100;
        if (x < origin_x)
          x = origin_x;
        if (y < origin_y)
          y = origin_y;
        XWindowChanges changes;
        changes.x = x;
        changes.y = y;
        changes.width = width;
        changes.height = height;
        XConfigureWindow(display, window, CWX | CWY | CWWidth | CWHeight, &changes);
        if (resized && !use_argb) {
          apply_shape(display, window);
          XClearWindow(display, window);
        }
        if (first) {
          XMapRaised(display, window);
          XChangeProperty(display, window, wm_state, wm_state, 32, PropModeReplace, (unsigned char *)withdrawn, 2);
        }
        paint(display, window, visual, state, text);
      }
    }
  }
  XCloseDisplay(display);
  return 0;
}
