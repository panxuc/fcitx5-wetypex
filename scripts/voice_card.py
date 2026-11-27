#!/usr/bin/env python3
"""Bottom-center streaming voice pill, matching just-talk-go.

Live text stays in the pill. When recognition finishes, the text is copied
and pasted with Shift+Insert, so it works without a focused input method.
"""

import json
import os
import signal
import subprocess
import time
from pathlib import Path

os.environ.setdefault("GDK_BACKEND", "x11")

import cairo
import gi

gi.require_version("Gdk", "3.0")
gi.require_version("Gtk", "3.0")
gi.require_version("PangoCairo", "1.0")
from gi.repository import Gdk, GLib, Gtk, Pango, PangoCairo

DATA = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
INBOX = DATA / "fcitx5-wetypex/state/voice-inbox.json"
OVERLAY_BIN = next(
    (
        path
        for path in (
            Path(__file__).with_name("wetypex-voice-overlay"),
            Path.home() / ".local/bin/wetypex-voice-overlay",
        )
        if path.is_file()
    ),
    None,
)
VOICE_STATES = ("recording", "recognizing", "polishing")
_last_status = {}


def voice_status():
    global _last_status
    try:
        _last_status = json.loads(INBOX.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        pass
    return _last_status


def active_class():
    try:
        window_id = subprocess.check_output(["xdotool", "getactivewindow"], text=True).strip()
        props = subprocess.check_output(["xprop", "-id", window_id, "WM_CLASS"], text=True)
    except (OSError, subprocess.CalledProcessError):
        return ""
    return props.rsplit('"', 2)[-2] if '"' in props else ""


def display_size():
    try:
        width, height = subprocess.check_output(
            ["xdotool", "getdisplaygeometry"], text=True
        ).split()
        return int(width), int(height)
    except (OSError, subprocess.CalledProcessError, ValueError):
        return 1920, 1080


def monitor_rect():
    """Monitor that contains the focused window, same placement just-talk uses."""
    display = Gdk.Display.get_default()
    geo = None
    try:
        window_id = subprocess.check_output(["xdotool", "getactivewindow"], text=True).strip()
        geo = window_geometry(window_id)
    except (OSError, subprocess.CalledProcessError):
        geo = None
    if geo:
        cx, cy = geo[0] + geo[2] // 2, geo[1] + geo[3] // 2
    else:
        cx, cy = 0, 0
    monitor = None
    try:
        monitor = display.get_monitor_at_point(cx, cy)
    except Exception:
        monitor = None
    if monitor is None and display.get_n_monitors():
        monitor = display.get_monitor(0)
    if monitor is None:
        width, height = display_size()
        return 0, 0, width, height
    rect = monitor.get_geometry()
    return rect.x, rect.y, rect.width, rect.height


def moonlight_rect():
    """Visible Moonlight stream, so the pill sits inside the picture."""
    try:
        ids = subprocess.check_output(["xdotool", "search", "--class", "moonlight"], text=True).split()
    except (OSError, subprocess.CalledProcessError):
        return None
    best = None
    for window_id in ids:
        geo = window_geometry(window_id)
        if not geo:
            continue
        x, y, width, height = geo
        if width < 400 or height < 300:
            continue
        info = subprocess.run(["xwininfo", "-id", window_id], capture_output=True, text=True)
        if "IsViewable" not in info.stdout:
            continue
        area = width * height
        if best is None or area > best[0]:
            best = (area, x, y, width, height)
    return None if best is None else best[1:]


def window_geometry(window_id):
    try:
        raw = subprocess.check_output(["xdotool", "getwindowgeometry", window_id], text=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    position = size = None
    for line in raw.splitlines():
        line = line.strip()
        if line.startswith("Position:"):
            position = line.split(":", 1)[1].split("(")[0].strip()
        elif line.startswith("Geometry:"):
            size = line.split(":", 1)[1].strip()
    if not position or not size:
        return None
    try:
        x, y = (int(part) for part in position.split(","))
        width, height = (int(part) for part in size.split("x"))
    except ValueError:
        return None
    return x, y, width, height


def fcitx_panel_ids():
    try:
        raw = subprocess.check_output(
            ["xwininfo", "-name", "Fcitx5 Input Window"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return []
    for line in raw.splitlines():
        if "Window id:" in line:
            parts = line.split()
            if len(parts) >= 4:
                return [parts[3]]
    return []


def fill_round(cr, x, y, width, height, radius):
    radius = min(radius, width / 2.0, height / 2.0)
    if radius < 1:
        cr.rectangle(x, y, width, height)
        return
    cr.new_sub_path()
    cr.arc(x + width - radius, y + radius, radius, -3.14159 / 2, 0)
    cr.arc(x + width - radius, y + height - radius, radius, 0, 3.14159 / 2)
    cr.arc(x + radius, y + height - radius, radius, 3.14159 / 2, 3.14159)
    cr.arc(x + radius, y + radius, radius, 3.14159, 3 * 3.14159 / 2)
    cr.close_path()


def paste_text(text):
    text = text.strip()
    if not text:
        return
    subprocess.run(["xclip", "-selection", "clipboard"], input=text.encode(), check=False)
    subprocess.run(["xclip", "-selection", "primary"], input=text.encode(), check=False)
    time.sleep(0.1)
    if "moonlight" in active_class().lower():
        subprocess.run(
            ["xdotool", "key", "--clearmodifiers", "ctrl+alt+shift+v"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(0.3)
        subprocess.run(
            ["xdotool", "key", "--clearmodifiers", "ctrl+alt+shift+z"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return
    subprocess.run(
        ["xdotool", "key", "--clearmodifiers", "shift+Insert"],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


class VoicePill:
    def __init__(self):
        self.state = ""
        self.text = ""
        self.visible = False
        self.parked = set()
        self.seen_token = ""
        self.pasted_token = ""
        self.suppress_until = 0
        self.overlay = None
        self.pill_w = 160
        self.pill_h = 42
        self.outer_width = 176
        self.outer_height = 58
        self.window = Gtk.Window(type=Gtk.WindowType.POPUP)
        self.window.set_decorated(False)
        self.window.set_keep_above(True)
        self.window.set_skip_taskbar_hint(True)
        self.window.set_skip_pager_hint(True)
        self.window.set_accept_focus(False)
        self.window.set_focus_on_map(False)
        self.window.set_can_focus(False)
        self.window.set_app_paintable(True)
        self.window.set_type_hint(Gdk.WindowTypeHint.UTILITY)
        self.window.set_title("wetypex-voice-card")
        visual = self.window.get_screen().get_rgba_visual()
        if visual is not None:
            self.window.set_visual(visual)
        self.window.set_default_size(self.outer_width, self.outer_height)
        self.window.connect("draw", self.on_draw)
        self.window.connect("realize", self.on_realize)

    def tick(self):
        try:
            return self.update()
        except Exception:
            return True

    def update(self):
        status = voice_status()
        state = status.get("state") or ""
        token = str(status.get("token") or "")
        text = (status.get("text") or "").strip()
        if state in VOICE_STATES:
            self.suppress_until = time.monotonic() + 1.5
            self.seen_token = token
            self.state = state
            self.text = text
            self.park_fcitx_panel()
            self.visible = True
            safe = text.replace("\t", " ").replace("\n", " ")
            self.overlay_send("show\t%s\t%s\n" % (state or "recording", safe))
            return True
        if time.monotonic() < self.suppress_until:
            self.park_fcitx_panel()
        if state == "final" and text and token and token == self.seen_token and token != self.pasted_token:
            self.pasted_token = token
            self.hide_card()
            GLib.idle_add(lambda: paste_text(text) or False)
            return True
        if self.visible or self.parked:
            self.hide_card()
        return True

    def resize_for_text(self):
        layout = self.window.create_pango_layout(self.text or " ")
        layout.set_font_description(Pango.FontDescription("Sans 15"))
        layout.set_width(440 * Pango.SCALE)
        layout.set_wrap(Pango.WrapMode.WORD_CHAR)
        layout.set_ellipsize(Pango.EllipsizeMode.END)
        text_w, text_h = layout.get_pixel_size()
        if not self.text:
            text_w, text_h = 0, 0
        text_h = min(text_h, 22 * 3)
        content_w = 14 + 14 + 14 + max(text_w, 48)
        self.pill_w = min(520, max(122, content_w + 28))
        self.pill_h = max(42, text_h + 28)
        outer_w = self.pill_w + 16
        outer_h = self.pill_h + 16
        if outer_w != self.outer_width or outer_h != self.outer_height:
            self.outer_width, self.outer_height = outer_w, outer_h
            self.window.resize(outer_w, outer_h)

    def on_realize(self, widget):
        gdk_window = widget.get_window()
        if gdk_window is not None:
            gdk_window.set_override_redirect(True)
        self.pass_clicks()

    def place(self):
        stream = moonlight_rect()
        if stream:
            origin_x, origin_y, width, height = stream
        else:
            origin_x, origin_y, width, height = monitor_rect()
        x = origin_x + max(0, (width - self.outer_width) // 2)
        y = origin_y + height - self.outer_height - 100
        if y < origin_y:
            y = origin_y
        self.window.move(x, y)

    def park_fcitx_panel(self):
        for window_id in fcitx_panel_ids():
            geometry = window_geometry(window_id)
            if geometry and geometry[0] > -800:
                self.anchor = geometry
            subprocess.run(
                [
                    "xprop", "-id", window_id, "-f", "_NET_WM_WINDOW_OPACITY", "32c",
                    "-set", "_NET_WM_WINDOW_OPACITY", "0",
                ],
                check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            subprocess.run(
                ["xdotool", "windowunmap", window_id],
                check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            self.parked.add(window_id)

    def restore_fcitx_panel(self):
        for window_id in list(self.parked):
            subprocess.run(
                ["xdotool", "windowunmap", window_id],
                check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
        self.parked.clear()

    def raise_card(self):
        gdk_window = self.window.get_window()
        if gdk_window is None:
            return
        window_id = str(gdk_window.get_xid())
        subprocess.run(
            ["wmctrl", "-i", "-r", window_id, "-b", "add,above,sticky"],
            check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        subprocess.run(
            ["xdotool", "windowraise", window_id],
            check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )

    def pass_clicks(self):
        gdk_window = self.window.get_window()
        if gdk_window is None:
            return
        try:
            gdk_window.input_shape_combine_region(cairo.Region(), 0, 0)
        except (AttributeError, TypeError):
            pass

    def overlay_send(self, line):
        if OVERLAY_BIN is None:
            return
        if self.overlay is None or self.overlay.poll() is not None:
            self.overlay = subprocess.Popen(
                [str(OVERLAY_BIN)],
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        try:
            self.overlay.stdin.write(line.encode())
            self.overlay.stdin.flush()
        except (OSError, BrokenPipeError):
            self.overlay = None

    def hide_card(self):
        self.visible = False
        self.overlay_send("hide\n")
        self.restore_fcitx_panel()

    def on_draw(self, _widget, cr):
        cr.set_operator(cairo.OPERATOR_SOURCE)
        cr.set_source_rgba(0, 0, 0, 0)
        cr.paint()
        cr.set_operator(cairo.OPERATOR_OVER)
        x, y = 8, 8
        fill_round(cr, x, y, self.pill_w, self.pill_h, self.pill_h / 2)
        cr.set_source_rgba(20 / 255, 20 / 255, 20 / 255, 215 / 255)
        cr.fill()
        if self.state == "recording":
            dot = (1, 65 / 255, 65 / 255)
        else:
            dot = (1, 160 / 255, 70 / 255)
        cr.set_source_rgb(*dot)
        cr.arc(x + 14 + 7, y + self.pill_h / 2, 7, 0, 6.28318)
        cr.fill()
        if self.text:
            layout = PangoCairo.create_layout(cr)
            layout.set_font_description(Pango.FontDescription("Sans 15"))
            layout.set_width(max(1, self.pill_w - 14 - 14 - 14 - 18) * Pango.SCALE)
            layout.set_wrap(Pango.WrapMode.WORD_CHAR)
            layout.set_ellipsize(Pango.EllipsizeMode.END)
            layout.set_text(self.text, -1)
            _tw, th = layout.get_pixel_size()
            cr.set_source_rgb(245 / 255, 245 / 255, 245 / 255)
            cr.move_to(x + 14 + 14 + 14, y + max(8, (self.pill_h - th) / 2))
            PangoCairo.show_layout(cr, layout)
        return False


def main():
    signal.signal(signal.SIGTERM, lambda *_args: Gtk.main_quit())
    pill = VoicePill()
    GLib.timeout_add(50, pill.tick)
    Gtk.main()


if __name__ == "__main__":
    main()
