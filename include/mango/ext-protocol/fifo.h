#ifndef __MANGO_EXT_PROTOCOL_FIFO_H__
#define __MANGO_EXT_PROTOCOL_FIFO_H__ 1

#include "mango/common/types.h"

struct mango_fifo_manager_v1;

struct mango_fifo_manager_v1 *
mango_fifo_manager_v1_create(struct wl_display *display);

// notify fifo surfaces that an output has passed a latch deadline
void mango_fifo_manager_v1_output_latched(struct mango_fifo_manager_v1 *manager,
										  struct wlr_output *output);

#endif
