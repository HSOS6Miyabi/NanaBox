"""Exercise the host keyboard encoder against an actual GTK 3 text entry."""

import ctypes as C
import os
import subprocess
import sys
import time

os.environ["GTK_IM_MODULE"] = "simple"
os.environ["GDK_BACKEND"] = "x11"

gtk = C.CDLL("libgtk-3.so.0")
gdk = C.CDLL("libgdk-3.so.0")
x11 = C.CDLL("libX11.so.6")
xtest = C.CDLL("libXtst.so.6")
pointer = C.c_void_p


def bind(lib, name, result, *args):
    fn = getattr(lib, name)
    fn.restype = result
    fn.argtypes = list(args)
    return fn


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: KeyboardGtkIntegration.py KEYBOARD_TEST_EXECUTABLE")
    events = subprocess.check_output([sys.argv[1], "--emit-gtk"], text=True)
    assert bind(gtk, "gtk_init_check", C.c_int, pointer, pointer)(None, None)
    window = bind(gtk, "gtk_window_new", pointer, C.c_int)(0)
    entry = bind(gtk, "gtk_entry_new", pointer)()
    bind(gtk, "gtk_container_add", None, pointer, pointer)(window, entry)
    bind(gtk, "gtk_widget_show_all", None, pointer)(window)
    bind(gtk, "gtk_widget_grab_focus", None, pointer)(entry)
    bind(gtk, "gtk_window_present", None, pointer)(window)
    pending = bind(gtk, "gtk_events_pending", C.c_int)
    iteration = bind(gtk, "gtk_main_iteration_do", C.c_int, C.c_int)

    def pump():
        for _ in range(40):
            while pending():
                iteration(0)
            time.sleep(0.005)

    pump()
    native = bind(gtk, "gtk_widget_get_window", pointer, pointer)(window)
    xid = bind(gdk, "gdk_x11_window_get_xid", C.c_ulong, pointer)(native)
    display = bind(x11, "XOpenDisplay", pointer, C.c_char_p)(None)
    assert display
    try:
        bind(x11, "XSetInputFocus", C.c_int, pointer, C.c_ulong, C.c_int, C.c_ulong)(display, xid, 1, 0)
        flush = bind(x11, "XFlush", C.c_int, pointer)
        flush(display)
        pump()
        fake = bind(xtest, "XTestFakeKeyEvent", C.c_int, pointer, C.c_uint, C.c_int, C.c_ulong)
        for line in events.splitlines():
            scan, up, extended = map(int, line.split())
            assert not extended
            # The Xvfb US evdev keyboard maps set-1 scan codes at offset 8.
            assert fake(display, scan + 8, not up, 0)
            flush(display)
            while pending():
                iteration(0)
        pump()
        result = bind(gtk, "gtk_entry_get_text", C.c_char_p, pointer)(entry).decode()
        assert result == "Hello 世界 😀 € A!", repr(result)
        print("GTK text-entry integration passed.")
    finally:
        bind(x11, "XCloseDisplay", C.c_int, pointer)(display)
        bind(gtk, "gtk_widget_destroy", None, pointer)(window)


if __name__ == "__main__":
    main()
