/* Same kind of X11 overlay as just-talk: override-redirect, drawn with real pixels. */
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
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

static int width = 122, height = 42;
static int visible = 0;

static int x_error(Display *display, XErrorEvent *event) {
  (void)display;
  (void)event;
  return 0;
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

static int moonlight_rect(Display *display, int screen, int *x, int *y, int *w, int *h) {
  Window root = RootWindow(display, screen);
  Window root_return, parent;
  Window *children = NULL;
  unsigned count = 0;
  if (!XQueryTree(display, root, &root_return, &parent, &children, &count))
    return 0;
  int best = 0;
  for (unsigned i = 0; i < count; i++) {
    XClassHint hint = {0};
    if (!XGetClassHint(display, children[i], &hint))
      continue;
    int match = (hint.res_class && strcasestr(hint.res_class, "moonlight")) ||
                (hint.res_name && strcasestr(hint.res_name, "moonlight"));
    if (hint.res_name)
      XFree(hint.res_name);
    if (hint.res_class)
      XFree(hint.res_class);
    if (!match)
      continue;
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(display, children[i], &attrs) || attrs.map_state != IsViewable)
      continue;
    if (attrs.width < 400 || attrs.height < 300)
      continue;
    if (attrs.width * attrs.height <= best)
      continue;
    Window child = 0;
    int abs_x = 0, abs_y = 0;
    XTranslateCoordinates(display, children[i], root, 0, 0, &abs_x, &abs_y, &child);
    best = attrs.width * attrs.height;
    *x = abs_x;
    *y = abs_y;
    *w = attrs.width;
    *h = attrs.height;
  }
  if (children)
    XFree(children);
  return best > 0;
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

static void paint(Display *display, Window window, Visual *visual, const char *state, const char *text) {
  cairo_surface_t *surface = cairo_xlib_surface_create(display, window, visual, width, height);
  cairo_t *cr = cairo_create(surface);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  rounded(cr, 0, 0, width, height, height / 2.0);
  cairo_set_source_rgba(cr, 20 / 255.0, 20 / 255.0, 20 / 255.0, 215 / 255.0);
  cairo_fill(cr);
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
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  XFlush(display);
}

int main(void) {
  setlocale(LC_ALL, "");
  XSetErrorHandler(x_error);
  Display *display = XOpenDisplay(NULL);
  if (!display)
    return 1;
  int screen = DefaultScreen(display);
  Visual *visual = argb_visual(display, screen);
  int depth = 32;
  Colormap colormap = 0;
  Window root = RootWindow(display, screen);
  if (!visual) {
    visual = DefaultVisual(display, screen);
    depth = DefaultDepth(display, screen);
    colormap = DefaultColormap(display, screen);
  } else {
    colormap = XCreateColormap(display, root, visual, AllocNone);
  }
  XSetWindowAttributes attrs;
  memset(&attrs, 0, sizeof attrs);
  attrs.colormap = colormap;
  attrs.override_redirect = True;
  attrs.save_under = True;
  attrs.background_pixel = 0;
  attrs.border_pixel = 0;
  Window window = XCreateWindow(
      display, root, 0, 0, width, height, 0, depth, InputOutput, visual,
      CWColormap | CWOverrideRedirect | CWSaveUnder | CWBackPixel | CWBorderPixel, &attrs);
  Atom type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
  Atom utility = XInternAtom(display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
  XChangeProperty(display, window, type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&utility, 1);
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
        if (next_w != width || next_h != height) {
          width = next_w;
          height = next_h;
          XResizeWindow(display, window, width, height);
        }
        visible = 1;
        XMapRaised(display, window);
        paint(display, window, visual, state, text);
      }
    }
    if (!visible)
      continue;
    int origin_x = 0, origin_y = 0, area_w = DisplayWidth(display, screen), area_h = DisplayHeight(display, screen);
    moonlight_rect(display, screen, &origin_x, &origin_y, &area_w, &area_h);
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
