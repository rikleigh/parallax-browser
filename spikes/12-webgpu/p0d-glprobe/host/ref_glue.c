/* ref_glue.c - refcheck.sh only: ps5-opengl's EGL facade over Mesa's software
 * rasteriser (softpipe) instead of the ps5 screen, so the probe's own logic
 * (shaders, expected values, GL call sequences) can be checked on the Mac.
 * It says nothing about the PS5 driver. */
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"

struct sw_winsys;
struct sw_winsys *null_sw_create(void);
struct pipe_screen *softpipe_create_screen(struct sw_winsys *winsys);

static struct pipe_resource *(*real_resource_create)(struct pipe_screen *, const struct pipe_resource *);

/* The facade makes its scanout buffers as display targets; the null winsys
 * has none, and nothing is presented here. */
static struct pipe_resource *resource_create(struct pipe_screen *screen, const struct pipe_resource *templ)
{
    struct pipe_resource copy = *templ;
    copy.bind &= ~PIPE_BIND_DISPLAY_TARGET;
    return real_resource_create(screen, &copy);
}

struct pipe_screen *ref_screen_create(void)
{
    struct pipe_screen *screen = softpipe_create_screen(null_sw_create());
    if (screen) {
        real_resource_create = screen->resource_create;
        screen->resource_create = resource_create;
    }
    return screen;
}

struct pipe_resource *ps5_display_target_alias(struct pipe_resource *resource, unsigned index)
{
    struct pipe_resource *alias = NULL;
    (void)index;
    pipe_resource_reference(&alias, resource);
    return alias;
}

/* What else ps5_egl.c calls in ps5_screen.c, for presenting and locking;
 * nothing the surfaceless probe reaches does any work. */
#define STUB(name) long name(void) { return 0; }
STUB(ps5_context_last_compute_status)
STUB(ps5_context_last_draw_status)
STUB(ps5_context_queue_present)
STUB(ps5_resource_export_descriptor)
STUB(ps5_screen_prepare_present)
STUB(ps5_screen_present_lock)
STUB(ps5_screen_submit_lock)
STUB(ps5_screen_submit_unlock)
