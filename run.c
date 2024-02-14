#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <X11/Xlib.h>
#include <pulse/pulseaudio.h>
#include <x86intrin.h>

#define ARENA_IMPLEMENTATION
#include "arena.h"

#define ABORT(msg) do { fprintf(stderr, "%s:%d (%s): " msg "\n", __FILE__, __LINE__, __func__); __builtin_trap(); } while (0);

#define NOT_IMPLEMENTED() ABORT("not implemented")

#define LOGF(msg, ...) fprintf(stderr, "%s:%d(%s): " msg "\n", __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG(msg) fprintf(stderr, "%s:%d(%s): %s\n", __FILE__, __LINE__, __func__, msg)

#ifndef MAX
#define MAX(x, y) (((x) >= (y)) ? (x) : (y))
#endif

int32_t get_children_recursive(Arena *arena, pid_t parent, pid_t **children);

typedef struct SinkInput {
    unsigned int sink_input_index;
    pid_t pid;
    pa_cvolume true_volume;
} SinkInput;

#define NUM_MAX_SINK_INPUTS 1024

typedef struct {
    bool pulse_initialized;

    pa_context *context;
    pa_mainloop *main_loop;
    SinkInput sink_inputs[NUM_MAX_SINK_INPUTS];
} State;

static void state_init(State *state) {
    memset(state, 0, sizeof(*state));

    state->pulse_initialized = false;

    for (int i = 0; i < NUM_MAX_SINK_INPUTS; i++) {
        state->sink_inputs[i].sink_input_index = PA_INVALID_INDEX;
        state->sink_inputs[i].pid = -1;
    }
}

/**
 * Attempt to allocate a new SinkInput inside `state`.
 */
static SinkInput *
add_sink_input(State *state) {
    LOG("New sink input requested");
    for (int i = 0; i < NUM_MAX_SINK_INPUTS; i++) {
        if (state->sink_inputs[i].sink_input_index == PA_INVALID_INDEX) {
            assert(state->sink_inputs[i].pid == -1);
            return &state->sink_inputs[i];
        }
    }
    return NULL;
}

static SinkInput *
get_sink_input(State *state, unsigned int index) {
    for (int i = 0; i < NUM_MAX_SINK_INPUTS; i++) {
        if (state->sink_inputs[i].sink_input_index == index) {
            return &state->sink_inputs[i];
        }
    }
    return NULL;
}

static SinkInput *
get_sink_input_by_pid(State *state, int pid) {
    for (int i = 0; i < NUM_MAX_SINK_INPUTS; i++) {
        if (state->sink_inputs[i].pid == pid) {
            return &state->sink_inputs[i];
        }
    }
    return NULL;
}

static void
remove_sink_input(State *state, unsigned int index) {
    LOGF("removing sink input %u", index);
    for (int i = 0; i < NUM_MAX_SINK_INPUTS; i++) {
        SinkInput *input = &state->sink_inputs[i];
        if (input->sink_input_index == index) {
            input->sink_input_index = PA_INVALID_INDEX;
            input->pid = -1;
            memset(&input->true_volume, 0, sizeof(input->true_volume));
            break;
        }
    }
}

void
debug_print_sink_inputs(State *state) {
    LOG("------------------------------------------------------------");
    for (int i = 0; i < NUM_MAX_SINK_INPUTS; i++) {
        if (state->sink_inputs[i].sink_input_index != PA_INVALID_INDEX) {
            LOGF("{ .index = %u, .pid = %d }",
                    state->sink_inputs[i].sink_input_index,
                    state->sink_inputs[i].pid);
        }
    }
    LOG("------------------------------------------------------------");
}

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

static pid_t
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

void
debug_dump_properties(Display *d, Window w) {
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

static pid_t
find_window_pid(Display *display, Window window) {
    pid_t pid = get_window_pid(display, window);
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

#if 0
static void
server_info_callback(pa_context *context, const pa_server_info *si, void *userdata) {
    (void) context;
    (void) userdata;
    (void) si;
    LOG("got server info.");
}
#endif

void
debug_dump_proplist(pa_proplist *list) {
    void *state = NULL;
    const char *key = NULL;
    while ((key = pa_proplist_iterate(list, &state))) {
        printf("%s => %p\n", key, state);
    }
}

static void
client_info_callback(pa_context *context, const pa_client_info *ci, int eol, void *userdata) {
    (void) context;
    (void) eol;

    if (!ci) {
        // why are we called?
        return;
    }

    SinkInput *input = userdata;
    assert(input);
    LOGF("Got client_info for sink_index = %u", input->sink_input_index);
    const void *data = NULL;
    size_t nbytes = 0;
    int found_key = pa_proplist_get(
            ci->proplist,
            PA_PROP_APPLICATION_PROCESS_ID,
            &data, &nbytes
            ) == 0;

    if (found_key) {
        // TODO: verify is integer
        int pid = atoi(data);

        LOGF("Setting PID for sink_index = %u to %d", input->sink_input_index, pid);
        input->pid = pid;
    }
}

static void
operation_callback(pa_operation *op, void *_user) {
    (void) _user;
    switch (pa_operation_get_state(op)) {
        case PA_OPERATION_DONE:
        case PA_OPERATION_CANCELLED:
            pa_operation_unref(op);
            break;
        default:
            break;
    }
}

static void
init_sink_input(SinkInput *input, pa_context *context, const pa_sink_input_info *sii) {
    input->sink_input_index = sii->index;

    // update the volume?
    memcpy(&input->true_volume, &sii->volume, sizeof(sii->volume));

    // if PID not yet set (i.e. new sink input), request client info from
    // which PID will be determined
    if (input->pid == -1) {
        if (sii->client != PA_INVALID_INDEX) {
            LOGF("Requesting client info for sink input %d", sii->index);
            pa_operation *op = pa_context_get_client_info(
                    context, sii->client, client_info_callback, input);
            pa_operation_set_state_callback(op, operation_callback, NULL);
        }
        else {
            LOGF("WARNING: sink input %d has no client set; cannot determine PID!",
                    sii->index);
        }
    } else {
        LOGF("sink_input has PID = %d", input->pid);
        assert(input->pid != 0);
    }


}

static void
sink_input_info_callback(
        pa_context *context,
        const pa_sink_input_info *sii,
        int eol,
        /* (State *) */ void *_state)
{
    (void) eol;
    if (!sii) return;

    State *state = _state;
    SinkInput *input = get_sink_input(state, sii->index);
    if (!input) {
        input = add_sink_input(state);
        init_sink_input(input, context, sii);
    }
    LOGF("got sink_input_info_callback, setting true_volume = { .left = %d, .right = %d }",
            sii->volume.values[0],
            sii->volume.values[1]);
    memcpy(&input->true_volume, &sii->volume, sizeof(sii->volume));
}

static void
get_initial_sink_inputs(pa_context *context, State *state) {
#if 0
    printf("Requesting initial server info...\n");
    pa_context_get_server_info(context, server_info_callback, userdata);
#endif
    printf("Requesting initial sink input info...\n");
    pa_context_get_sink_input_info_list(context, sink_input_info_callback, state);
}

static void sub_callback(pa_context *context, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    int facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    int event = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

    const char *facility_str = NULL;
    switch (facility) {
        case PA_SUBSCRIPTION_EVENT_SINK: facility_str = "sink"; break;
        case PA_SUBSCRIPTION_EVENT_SOURCE: facility_str = "source"; break;
        case PA_SUBSCRIPTION_EVENT_SINK_INPUT: facility_str = "sink_input"; break;
        case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT: facility_str = "source_output"; break;
        case PA_SUBSCRIPTION_EVENT_MODULE: facility_str = "module"; break;
        case PA_SUBSCRIPTION_EVENT_CLIENT: facility_str = "client"; break;
        case PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE: facility_str = "sample_cache"; break;
        case PA_SUBSCRIPTION_EVENT_SERVER: facility_str = "server"; break;
        case PA_SUBSCRIPTION_EVENT_CARD: facility_str = "card"; break;
        default: facility_str = "UNKNOWN"; break;
    }

    const char *event_str = NULL;
    switch (event) {
        case PA_SUBSCRIPTION_EVENT_NEW: event_str = "new"; break;
        case PA_SUBSCRIPTION_EVENT_CHANGE: event_str = "change"; break;
        case PA_SUBSCRIPTION_EVENT_REMOVE: event_str = "remove"; break;
        default: event_str = "UNKNOWN"; break;
    }

    if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        State *state = userdata;
        if (event == PA_SUBSCRIPTION_EVENT_NEW || event == PA_SUBSCRIPTION_EVENT_CHANGE) {
            pa_operation *op = pa_context_get_sink_input_info(
                    context,
                    idx,
                    sink_input_info_callback,
                    state
                    );
            pa_operation_set_state_callback(op, operation_callback, NULL);
        } else if (event == PA_SUBSCRIPTION_EVENT_REMOVE) {
            remove_sink_input(state, idx);
        }
    }
#if 0
    LOGF("facility = %s, event_type = %s", facility_str, event_str);
#else
    (void) facility_str;
    (void) event_str;
#endif
}

static void
context_state_callback(pa_context *context, void *userdata) {
    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_UNCONNECTED:
        printf("PA_CONTEXT_UNCONNECTED");
        break; /**< The context hasn't been connected yet */
    case PA_CONTEXT_CONNECTING:
        printf("PA_CONTEXT_CONNECTING");
        break; /**< A connection is being established */
    case PA_CONTEXT_AUTHORIZING:
        printf("PA_CONTEXT_AUTHORIZING");
        break; /**< The client is authorizing itself to the daemon */
    case PA_CONTEXT_SETTING_NAME:
        printf("PA_CONTEXT_SETTING_NAME");
        break; /**< The client is passing its application name to the daemon */
    case PA_CONTEXT_READY:
        printf("PA_CONTEXT_READY");
        pa_context_set_subscribe_callback(context, sub_callback, userdata);
        pa_context_subscribe(context, PA_SUBSCRIPTION_MASK_SINK_INPUT, NULL, userdata);
        get_initial_sink_inputs(context, (State *) userdata);
        break; /**< The connection is established, the context is ready to execute operations */
    case PA_CONTEXT_FAILED:
        printf("PA_CONTEXT_FAILED");
        break; /**< The connection failed or was disconnected */
    case PA_CONTEXT_TERMINATED:
        printf("PA_CONTEXT_TERMINATED");
        break; /**< The connection was terminated cleanly */
    }
    puts("");
}

/**
 * balance: left-right-balance, from 0.0f (only left) to 1.0f (only right)
 */
void adjust_volume_for_sink_input(pa_context *context, SinkInput *input, float balance) {
    pa_cvolume volume;
    memcpy(&volume, &input->true_volume, sizeof(volume));
    if (volume.channels >= 2) {
#if 0
        pa_volume_t total = input->true_volume.values[0] + input->true_volume.values[1];
        pa_volume_t left  = (1.0f - balance) * total;
        pa_volume_t right = balance * total;
#else
        pa_volume_t max = MAX(input->true_volume.values[0], input->true_volume.values[1]);
        pa_volume_t left = (balance > 0.5f) ? ((1.0f - balance) * max) : max;
        pa_volume_t right = (balance > 0.5f) ? max : (balance * max);
#endif
        volume.values[0] = left;
        volume.values[1] = right;

        pa_operation *op = pa_context_set_sink_input_volume(
                context,
                input->sink_input_index,
                &volume,
                NULL,
                NULL);
        pa_operation_set_state_callback(op, operation_callback, NULL);
    }
}

static inline float
clampf(float x, float lo, float hi) {
    if (x < lo) return 0.0f;
    if (x > hi) return 1.0f;
    return x;
}

void adjust_volume(State *state, pa_context *context, pid_t pid, XConfigureEvent conf, Arena *arena) {
    (void) conf;

    float center = (float) conf.x + (float) conf.width / 2.0f;
    float balance = clampf((float) center / (2 * 1920), 0.0f, 1.0f);


    SinkInput *input = get_sink_input_by_pid(state, pid);
    if (input) {
        adjust_volume_for_sink_input(context, input, balance);
    }

    //alternative: walk up process hierarchy for each sink input's process
    //until `pid` is found

    // Also check children of `pid` for sink inputs
    pid_t *children = NULL;
    /* uint64_t _start = _rdtsc(); */
    int32_t child_count = get_children_recursive(arena, pid, &children);
    /* uint64_t _elapsed = _rdtsc() - _start; */
    /* LOGF("get_process_children() took %lf Mcycles", (float) _elapsed / 1e6); */

    for (int32_t i = 0; i < child_count; i++) {
        SinkInput *input = get_sink_input_by_pid(state, children[i]);
        if (input) {
            adjust_volume_for_sink_input(context, input, balance);
        }
    }
}

// grr, state needs to be global for signal handler...
// NOTE: the state will _only_ ever be referred to by `global_state`
// during `exit_handler`.
// Otherwise, it will _always_ be passed around explicitly.
State *global_state = NULL;

static void
exit_handler(int signo) {
    // TODO: not working correctly yet
    (void) signo;
    printf("terminating.\n");

    if (global_state && global_state->pulse_initialized) {
        LOG("hi");
        for (int i = 0; i < NUM_MAX_SINK_INPUTS; i++) {
            SinkInput *input = &global_state->sink_inputs[i];
            if (input->sink_input_index != PA_INVALID_INDEX) {
                LOGF("resetting volume for sink input %u", input->sink_input_index);
                // reset volume
                pa_cvolume volume;
                memcpy(&volume, &input->true_volume, sizeof(input->true_volume));
                pa_volume_t total = input->true_volume.values[0] + input->true_volume.values[1];
                pa_volume_t left  = total / 2.0f;
                pa_volume_t right = total / 2.0f;
                volume.values[0] = left;
                volume.values[1] = right;

                pa_operation *op = pa_context_set_sink_input_volume(
                        global_state->context,
                        input->sink_input_index,
                        &volume,
                        NULL,
                        NULL);
                while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
                    pa_mainloop_iterate(global_state->main_loop, 0, NULL);
                }
                pa_operation_unref(op);
            }
        }

        pa_context_disconnect(global_state->context);
    }

    exit(0);
}


int
main() {
    global_state = malloc(sizeof(*global_state));
    State *state = global_state;
    state_init(state);

    signal(SIGINT, exit_handler);
    signal(SIGTERM, exit_handler);

    pa_mainloop *ml = pa_mainloop_new();
    assert(ml);

    pa_mainloop_api *ml_api = pa_mainloop_get_api(ml);
    assert(ml_api);

    pa_context *context = pa_context_new(ml_api, "helloworld");
    assert(context);

    pa_context_set_state_callback(context, context_state_callback, state);
    assert(pa_context_connect(context, NULL, 0, NULL) >= 0);

    state->context = context;
    state->main_loop = ml;
    state->pulse_initialized = true;

    // ------------------------------------------------------------

    Arena temp;
    size_t arena_size = 1024 * 1024;
    void *memory = calloc(arena_size, 1);
    arena_init(&temp, memory, arena_size);

    Display *dsp = XOpenDisplay(NULL);
    assert(dsp);
    // TODO: close display on exit?

    Window root = DefaultRootWindow(dsp);
    assert(root);

    int status = XSelectInput(dsp, root, SubstructureNotifyMask);
    printf("status = %d\n", status);

    for (;;) {
        for (int num_pending = XPending(dsp); num_pending > 0; num_pending--) {
            XEvent event;
            XNextEvent(dsp, &event);
            if (event.type == ConfigureNotify) {
                XConfigureEvent *conf = &event.xconfigure;
                pid_t pid = find_window_pid(dsp, conf->window);
                adjust_volume(state, context, pid, *conf, &temp);
            }
        }
        arena_clear(&temp);

        pa_mainloop_iterate(ml, 0, NULL);
    }
}
