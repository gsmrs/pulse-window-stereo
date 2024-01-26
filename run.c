#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <X11/Xlib.h>

static bool
get_first_child(Display *d, Window w, Window *child) {
    Window root, parent, *children = NULL;
    unsigned int nchildren = 0;
    XQueryTree(d, w, &root, &parent, &children, &nchildren);
    if (nchildren > 0) {
        *child = children[0];
        XFree(children);
        return true;
    } else {
        return false;
    }
}

static int
get_window_pid(Display *display, Window window) {
    Atom actual_type_return;
    int actual_format_return;
    unsigned long nitems_return;
    unsigned long bytes_after_return;
    int *prop_return;

    Atom property = XInternAtom(display, "_NET_WM_PID", 1);
    int ret = XGetWindowProperty(
            display,
            window,
            property,
            0,
            1,
            False,
            AnyPropertyType,
            &actual_type_return,
            &actual_format_return,
            &nitems_return,
            &bytes_after_return,
            (unsigned char **) &prop_return);
    assert(ret == Success);
    if (!prop_return) {
        return -1;
    } else {
        return *prop_return;
    }
}

static void debug_dump_properties(Display *d, Window w) {
    Window root, parent, *children = NULL;
    unsigned int nchildren = 0;
    XQueryTree(d, w, &root, &parent, &children, &nchildren);
    assert(children);

    int num_props = 0;
    Atom *props = XListProperties(d, children[0], &num_props);
    printf("Window %lu has %d properties\n", w, num_props);
    for (int i = 0; i < num_props; i++) {
        char *name = XGetAtomName(d, props[i]);
        printf("%d) %s\n", i, name);
    }
    XFree(children);
    XFree(props);
}

static char *
get_window_name(Display *display, Window window) {
    Atom actual_type_return;
    int actual_format_return;
    unsigned long nitems_return;
    unsigned long bytes_after_return;
    unsigned char *prop_return = NULL;

    Atom property = XInternAtom(display, "_NET_WM_NAME", 1);
    int ret = XGetWindowProperty(
            display,
            window,
            property,
            0,
            1024,
            False,
            XInternAtom(display, "UTF8_STRING", True),
            &actual_type_return,
            &actual_format_return,
            &nitems_return,
            &bytes_after_return,
            &prop_return);

    assert(ret == Success);
    return (char *) prop_return;
}

static char *
find_window_name(Display *display, Window window) {
    char *name = get_window_name(display, window);
    if (name) {
        return name;
    }

    Window first_child;
    if (get_first_child(display, window, &first_child)) {
        return get_window_name(display, first_child);
    } else {
        return NULL;
    }
}

static int
find_window_pid(Display *display, Window window) {
    int pid = get_window_pid(display, window);
    if (pid != -1) {
        return pid;
    }

    Window child;
    if (get_first_child(display, window, &child)) {
        return get_window_pid(display, child);
    } else {
        return -1;
    }
}

int main() {
    Display *dsp = XOpenDisplay(NULL);
    assert(dsp);

    Window root = DefaultRootWindow(dsp);
    assert(root);

    int status = XSelectInput(dsp, root, SubstructureNotifyMask);
    printf("status = %d\n", status);

    for (;;) {
        for (int num_pending = XPending(dsp); num_pending >= 0; num_pending--) {
            XEvent event;
            XNextEvent(dsp, &event);
            if (event.type == ConfigureNotify) {
                XConfigureEvent *conf = &event.xconfigure;
                char *name = find_window_name(dsp, conf->window);
                printf("Window (%s) is at (%d, %d)\n", name, conf->x, conf->y);
                if (name) XFree(name);
                /* debug_dump_properties(dsp, conf->window); */
                int pid = find_window_pid(dsp, conf->window);
                printf("PID = %d\n", pid);
            }
        }
    }
}
