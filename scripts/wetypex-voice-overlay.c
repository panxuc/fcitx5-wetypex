/* Same X11 overlay as just-talk.
   With a compositor: 32-bit premultiplied pixels via XPutImage.
   Without one (this machine): 24-bit window plus a rounded shape mask.
   A 32-bit window stays invisible when nothing is compositing. */
#define _GNU_SOURCE
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
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

static void consider_moonlight(Display *display, Window root, Window win, int *best, int *x, int *y, int *w, int *h) {
  XClassHint hint = {0};
  if (!XGetClassHint(display, win, &hint))
    return;
  int match = (hint.res_class && strcasestr(hint.res_class, "moonlight")) ||
              (hint.res_name && strcasestr(hint.res_name, "moonlight"));
  if (hint.res_name)
    XFree(hint.res_name);
  if (hint.res_class)
    XFree(hint.res_class);
  if (!match)
    return;
  XWindowAttributes attrs;
  if (!XGetWindowAttributes(display, win, &attrs) || attrs.map_state != IsViewable)
    return;
  if (attrs.width < 400 || attrs.height < 300)
    return;
  if (attrs.width * attrs.height <= *best)
    return;
  Window child = 0;
  int abs_x = 0, abs_y = 0;
  XTranslateCoordinates(display, win, root, 0, 0, &abs_x, &abs_y, &child);
  *best = attrs.width * attrs.height;
  *x = abs_x;
  *y = abs_y;
  *w = attrs.width;
  *h = attrs.height;
}

static void walk_moonlight(Display *display, Window root, Window win, int depth, int *best, int *x, int *y, int *w, int *h) {
  consider_moonlight(display, root, win, best, x, y, w, h);
  if (depth <= 0)
    return;
  Window root_return, parent, *children = NULL;
  unsigned count = 0;
  if (!XQueryTree(display, win, &root_return, &parent, &children, &count))
    return;
  for (unsigned i = 0; i < count; i++)
    walk_moonlight(display, root, children[i], depth - 1, best, x, y, w, h);
  if (children)
    XFree(children);
}

static int moonlight_rect(Display *display, int screen, int *x, int *y, int *w, int *h) {
  int best = 0;
  walk_moonlight(display, RootWindow(display, screen), RootWindow(display, screen), 3, &best, x, y, w, h);
  return best > 0;
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
  int radius = height / 2;
  int stride = (width + 7) / 8;
  char *bits = calloc((size_t)stride * (size_t)height, 1);
  if (!bits)
    return;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int inside = 1;
      if (x < radius || x >= width - radius) {
        int cx = x < radius ? radius : width - radius - 1;
        int cy = y < radius ? radius : (y >= height - radius ? height - radius - 1 : y);
        if (y < radius || y >= height - radius) {
          int dx = x - cx, dy = y - cy;
          inside = dx * dx + dy * dy <= radius * radius;
        }
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

static void use_font(cairo_t *cr) {
  cairo_select_font_face(cr, "Noto Sans CJK SC", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 14);
}

static void measure(const char *text, int *text_w, int *text_h) {
  *text_w = 0;
  *text_h = 0;
  if (!text || !text[0])
    return;
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  use_font(cr);
  cairo_text_extents_t extents;
  cairo_text_extents(cr, text, &extents);
  *text_w = (int)(extents.x_advance + 0.5);
  if (*text_w > 464)
    *text_w = 464;
  *text_h = 18;
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

static void draw_pill(cairo_t *cr, const char *state, const char *text, int opaque) {
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  if (opaque) {
    cairo_set_source_rgb(cr, 20 / 255.0, 20 / 255.0, 20 / 255.0);
    cairo_paint(cr);
  } else {
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    rounded(cr, 0, 0, width, height, height / 2.0);
    cairo_set_source_rgba(cr, 20 / 255.0, 20 / 255.0, 20 / 255.0, 215 / 255.0);
    cairo_fill(cr);
  }
  if (state && strcmp(state, "recording") == 0)
    cairo_set_source_rgb(cr, 1, 65 / 255.0, 65 / 255.0);
  else if (state && strcmp(state, "polishing") == 0)
    cairo_set_source_rgb(cr, 1, 140 / 255.0, 60 / 255.0);
  else
    cairo_set_source_rgb(cr, 1, 160 / 255.0, 70 / 255.0);
  cairo_arc(cr, 21, height / 2.0, 7, 0, 2 * M_PI);
  cairo_fill(cr);
  if (text && text[0]) {
    use_font(cr);
    cairo_set_source_rgb(cr, 245 / 255.0, 245 / 255.0, 245 / 255.0);
    cairo_move_to(cr, 42, height / 2.0 + 5);
    cairo_show_text(cr, text);
  }
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
  XStoreName(display, window, "wetypex-voice-card");

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
        int text_w = 0, text_h = 0;
        measure(text, &text_w, &text_h);
        int next_w = text[0] ? text_w + 56 : 122;
        int next_h = text[0] ? text_h + 28 : 42;
        if (next_w < 122)
          next_w = 122;
        if (next_w > 520)
          next_w = 520;
        if (next_h < 42)
          next_h = 42;
        int resized = next_w != width || next_h != height;
        if (resized) {
          width = next_w;
          height = next_h;
          XResizeWindow(display, window, (unsigned)width, (unsigned)height);
          if (!use_argb)
            apply_shape(display, window);
        }
        visible = 1;
        XMapRaised(display, window);
        paint(display, window, visual, state, text);
      }
    }
    if (!visible)
      continue;
    int origin_x = 0, origin_y = 0, area_w = 0, area_h = 0;
    if (!moonlight_rect(display, screen, &origin_x, &origin_y, &area_w, &area_h))
      monitor_of_focus(display, screen, &origin_x, &origin_y, &area_w, &area_h);
    int x = origin_x + (area_w - width) / 2;
    int y = origin_y + area_h - height - 100;
    if (x < origin_x)
      x = origin_x;
    if (y < origin_y)
      y = origin_y;
    XMoveWindow(display, window, x, y);
    XRaiseWindow(display, window);
    XFlush(display);
  }
  XCloseDisplay(display);
  return 0;
}
