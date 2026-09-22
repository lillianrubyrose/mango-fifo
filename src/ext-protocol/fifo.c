#include "mango/ext-protocol/fifo.h"
#include "fifo-v1-protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_subcompositor.h>

struct mango_fifo_state {
	bool set_barrier;
	bool wait_barrier;
};

struct mango_fifo_lock {
	struct wl_list link;
	uint32_t seq;
	bool released;
};

struct mango_fifo_surface;

struct mango_fifo_object {
	struct mango_fifo_surface *surface;
};

struct mango_fifo_manager_v1 {
	struct wl_global *global;
	struct wl_display *display;
	struct wl_listener display_destroy;
	struct wl_list surfaces;
	struct wl_event_source *resolve_idle;
};

struct mango_fifo_surface {
	struct mango_fifo_manager_v1 *manager;
	struct wlr_surface *surface;
	struct mango_fifo_object *object;
	struct wlr_surface_synced synced;
	struct mango_fifo_state pending;
	struct mango_fifo_state current;
	struct wl_listener client_commit;
	struct wl_listener surface_destroy;
	struct wl_listener output_destroy;
	struct wl_list locks;
	struct wl_list link;
	struct wlr_output *output;
	struct wlr_output *destroying_output;
	bool barrier;
	bool advancing;
};

static void fifo_surface_clear_barrier(struct mango_fifo_surface *fifo);
static void fifo_surface_advance(struct mango_fifo_surface *fifo);

static bool
fifo_surface_is_effectively_synchronized(struct wlr_surface *surface) {
	struct wlr_subsurface *subsurface;

	while ((subsurface = wlr_subsurface_try_from_wlr_surface(surface))) {
		if (subsurface->synchronized)
			return true;
		surface = subsurface->parent;
	}

	return false;
}

static struct wlr_output *
fifo_surface_first_output(struct mango_fifo_surface *fifo) {
	struct wlr_surface_output *surface_output;
	wl_list_for_each(surface_output, &fifo->surface->current_outputs, link) {
		if (surface_output->output->enabled &&
			surface_output->output != fifo->destroying_output)
			return surface_output->output;
	}
	return NULL;
}

static void fifo_surface_set_output(struct mango_fifo_surface *fifo,
									struct wlr_output *output) {
	if (fifo->output) {
		wl_list_remove(&fifo->output_destroy.link);
		wl_list_init(&fifo->output_destroy.link);
	}

	fifo->output = output;
	if (output) {
		wl_signal_add(&output->events.destroy, &fifo->output_destroy);
		wlr_output_schedule_frame(output);
	}
}

static void fifo_resolve_outputs_idle(void *data) {
	struct mango_fifo_manager_v1 *manager = data;
	struct mango_fifo_surface *fifo, *tmp;
	manager->resolve_idle = NULL;

	// resolve newly mapped surfaces once the current Wayland dispatch has completed
	wl_list_for_each_safe(fifo, tmp, &manager->surfaces, link) {
		if (!fifo->barrier || fifo->output) {
			continue;
		}

		struct wlr_output *output = fifo_surface_first_output(fifo);
		if (output) {
			fifo_surface_set_output(fifo, output);
		} else {
			// off screen surfaces must not be stalled indefinitely
			fifo_surface_clear_barrier(fifo);
		}
	}
}

static void fifo_schedule_output_resolution(struct mango_fifo_surface *fifo) {
	struct mango_fifo_manager_v1 *manager = fifo->manager;
	if (manager->resolve_idle) {
		return;
	}

	manager->resolve_idle = wl_event_loop_add_idle(wl_display_get_event_loop(manager->display), fifo_resolve_outputs_idle, manager);
	if (!manager->resolve_idle) {
		fifo_surface_clear_barrier(fifo);
	}
}

static void fifo_surface_set_barrier(struct mango_fifo_surface *fifo) {
	if (fifo->barrier) {
		fifo->barrier = false;
		fifo_surface_set_output(fifo, NULL);
	}

	fifo->barrier = true;
	struct wlr_output *output = fifo_surface_first_output(fifo);
	if (output) {
		fifo_surface_set_output(fifo, output);
	} else {
		fifo_schedule_output_resolution(fifo);
	}
}

static void fifo_surface_advance(struct mango_fifo_surface *fifo) {
	if (fifo->advancing) {
		return;
	}

	fifo->advancing = true;
	while (!fifo->barrier && !wl_list_empty(&fifo->locks)) {
		struct mango_fifo_lock *lock = wl_container_of(fifo->locks.next, lock, link);
		if (lock->released) {
			break;
		}

		lock->released = true;
		uint32_t seq = lock->seq;
		wlr_surface_unlock_cached(fifo->surface, seq);

		if (!wl_list_empty(&fifo->locks)) {
			lock = wl_container_of(fifo->locks.next, lock, link);
			if (lock->released && lock->seq == seq) {
				break;
			}
		}
	}
	fifo->advancing = false;
}

static void fifo_surface_clear_barrier(struct mango_fifo_surface *fifo) {
	if (!fifo->barrier) {
		return;
	}

	fifo->barrier = false;
	fifo_surface_set_output(fifo, NULL);
	fifo_surface_advance(fifo);
}

static void fifo_synced_move_state(void *dst_data, void *src_data) {
	struct mango_fifo_state *dst = dst_data;
	struct mango_fifo_state *src = src_data;
	*dst = *src;
	*src = (struct mango_fifo_state){0};
}

static void fifo_synced_commit(struct wlr_surface_synced *synced) {
	struct mango_fifo_surface *fifo = wl_container_of(synced, fifo, synced);
	struct mango_fifo_state *state = wlr_surface_synced_get_state(&fifo->synced, &fifo->surface->current);

	if (state->set_barrier) {
		fifo_surface_set_barrier(fifo);
	}

	// a released FIFO lock is only complete when all readiness constraints have allowed this surface state to apply
	if (!wl_list_empty(&fifo->locks)) {
		struct mango_fifo_lock *lock = wl_container_of(fifo->locks.next, lock, link);
		if (lock->released && lock->seq == fifo->surface->current.seq) {
			wl_list_remove(&lock->link);
			free(lock);
		}
	}

	fifo_surface_advance(fifo);
}

static const struct wlr_surface_synced_impl fifo_synced_impl = {
	.state_size = sizeof(struct mango_fifo_state),
	.move_state = fifo_synced_move_state,
	.commit = fifo_synced_commit,
};

static void fifo_handle_client_commit(struct wl_listener *listener,
									  void *data) {
	struct mango_fifo_surface *fifo = wl_container_of(listener, fifo, client_commit);
	struct mango_fifo_state *state = wlr_surface_synced_get_state(&fifo->synced, &fifo->surface->pending);

	if (!state->wait_barrier || !fifo->barrier) {
		return;
	}

	// synchronized subsurfaces are applied atomically with an ancestor and must ignore constraints
	if (fifo_surface_is_effectively_synchronized(fifo->surface)) {
		return;
	}

	struct mango_fifo_lock *lock = calloc(1, sizeof(*lock));
	if (!lock) {
		wl_client_post_no_memory(wl_resource_get_client(fifo->surface->resource));
		return;
	}

	lock->seq = wlr_surface_lock_pending(fifo->surface);
	wl_list_insert(fifo->locks.prev, &lock->link);
}

static void fifo_handle_output_destroy(struct wl_listener *listener,
									   void *data) {
	struct mango_fifo_surface *fifo = wl_container_of(listener, fifo, output_destroy);
	fifo->destroying_output = fifo->output;
	fifo_surface_clear_barrier(fifo);
	fifo->destroying_output = NULL;
}

static void fifo_object_detach(struct mango_fifo_object *object) {
	if (object->surface && object->surface->object == object) {
		object->surface->object = NULL;
	}
	object->surface = NULL;
}

static void fifo_object_resource_destroy(struct wl_resource *resource) {
	struct mango_fifo_object *object = wl_resource_get_user_data(resource);
	fifo_object_detach(object);
	free(object);
}

static void fifo_handle_set_barrier(struct wl_client *client,
									struct wl_resource *resource) {
	struct mango_fifo_object *object = wl_resource_get_user_data(resource);
	if (!object->surface) {
		wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
							   "the associated wl_surface has been destroyed");
		return;
	}
	object->surface->pending.set_barrier = true;
}

static void fifo_handle_wait_barrier(struct wl_client *client,
									 struct wl_resource *resource) {
	struct mango_fifo_object *object = wl_resource_get_user_data(resource);
	if (!object->surface) {
		wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
							   "the associated wl_surface has been destroyed");
		return;
	}
	object->surface->pending.wait_barrier = true;
}

static void fifo_handle_destroy(struct wl_client *client,
								struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_fifo_v1_interface fifo_impl = {
	.set_barrier = fifo_handle_set_barrier,
	.wait_barrier = fifo_handle_wait_barrier,
	.destroy = fifo_handle_destroy,
};

static void fifo_surface_destroy(struct mango_fifo_surface *fifo) {
	if (fifo->object) {
		fifo_object_detach(fifo->object);
	}

	if (fifo->output) {
		wl_list_remove(&fifo->output_destroy.link);
		fifo->output = NULL;
	}

	struct mango_fifo_lock *lock, *tmp;
	wl_list_for_each_safe(lock, tmp, &fifo->locks, link) {
		wl_list_remove(&lock->link);
		free(lock);
	}

	wl_list_remove(&fifo->client_commit.link);
	wl_list_remove(&fifo->surface_destroy.link);
	wlr_surface_synced_finish(&fifo->synced);
	wl_list_remove(&fifo->link);
	free(fifo);
}

static void fifo_handle_surface_destroy(struct wl_listener *listener,
										void *data) {
	struct mango_fifo_surface *fifo = wl_container_of(listener, fifo, surface_destroy);
	fifo_surface_destroy(fifo);
}

static struct mango_fifo_surface *
fifo_surface_find(struct mango_fifo_manager_v1 *manager,
				  struct wlr_surface *surface) {
	struct mango_fifo_surface *fifo;
	wl_list_for_each(fifo, &manager->surfaces, link) {
		if (fifo->surface == surface) {
			return fifo;
		}
	}
	return NULL;
}

static struct mango_fifo_surface *
fifo_surface_create(struct mango_fifo_manager_v1 *manager,
					struct wlr_surface *surface) {
	struct mango_fifo_surface *fifo = calloc(1, sizeof(*fifo));
	if (!fifo) {
		return NULL;
	}

	fifo->manager = manager;
	fifo->surface = surface;
	wl_list_init(&fifo->locks);
	wl_list_init(&fifo->output_destroy.link);

	if (!wlr_surface_synced_init(&fifo->synced, surface, &fifo_synced_impl,
								 &fifo->pending, &fifo->current)) {
		free(fifo);
		return NULL;
	}

	fifo->client_commit.notify = fifo_handle_client_commit;
	wl_signal_add(&surface->events.client_commit, &fifo->client_commit);
	fifo->surface_destroy.notify = fifo_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &fifo->surface_destroy);
	wl_list_insert(&manager->surfaces, &fifo->link);
	return fifo;
}

static void fifo_manager_handle_get_fifo(struct wl_client *client,
										 struct wl_resource *manager_resource,
										 uint32_t id,
										 struct wl_resource *surface_resource) {
	struct mango_fifo_manager_v1 *manager = wl_resource_get_user_data(manager_resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	struct mango_fifo_surface *fifo = fifo_surface_find(manager, surface);

	if (fifo && fifo->object) {
		wl_resource_post_error(
			manager_resource, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
			"a FIFO object already exists for this wl_surface");
		return;
	}

	struct wl_resource *resource =wl_resource_create(client, &wp_fifo_v1_interface, wl_resource_get_version(manager_resource), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	struct mango_fifo_object *object = calloc(1, sizeof(*object));
	if (!object) {
		wl_resource_destroy(resource);
		wl_client_post_no_memory(client);
		return;
	}

	if (!fifo) {
		fifo = fifo_surface_create(manager, surface);
		if (!fifo) {
			free(object);
			wl_resource_destroy(resource);
			wl_client_post_no_memory(client);
			return;
		}
	}

	object->surface = fifo;
	fifo->object = object;
	wl_resource_set_implementation(resource, &fifo_impl, object, fifo_object_resource_destroy);
}

static void fifo_manager_handle_destroy(struct wl_client *client,
										struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_fifo_manager_v1_interface fifo_manager_impl = {
	.destroy = fifo_manager_handle_destroy,
	.get_fifo = fifo_manager_handle_get_fifo,
};

static void fifo_manager_bind(struct wl_client *client, void *data,
							  uint32_t version, uint32_t id) {
	struct mango_fifo_manager_v1 *manager = data;
	struct wl_resource *resource = wl_resource_create(client, &wp_fifo_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &fifo_manager_impl, manager, NULL);
}

static void fifo_manager_handle_display_destroy(struct wl_listener *listener,
												void *data) {
	struct mango_fifo_manager_v1 *manager = wl_container_of(listener, manager, display_destroy);

	if (manager->resolve_idle) {
		wl_event_source_remove(manager->resolve_idle);
	}

	struct mango_fifo_surface *fifo, *tmp;
	wl_list_for_each_safe(fifo, tmp, &manager->surfaces, link) {
		fifo_surface_destroy(fifo);
	}

	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct mango_fifo_manager_v1 *
mango_fifo_manager_v1_create(struct wl_display *display) {
	struct mango_fifo_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		return NULL;
	}

	manager->display = display;
	wl_list_init(&manager->surfaces);
	manager->global = wl_global_create(display, &wp_fifo_manager_v1_interface, 1, manager, fifo_manager_bind);
	if (!manager->global) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = fifo_manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);
	return manager;
}

void mango_fifo_manager_v1_output_latched(struct mango_fifo_manager_v1 *manager,
										  struct wlr_output *output) {
	if (!manager) {
		return;
	}

	struct mango_fifo_surface *fifo, *tmp;
	wl_list_for_each_safe(fifo, tmp, &manager->surfaces, link) {
		if (fifo->barrier && fifo->output == output) {
			fifo_surface_clear_barrier(fifo);
		}
	}
}
