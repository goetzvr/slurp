#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <linux/input-event-codes.h>
#elif __FreeBSD__
#include <dev/evdev/input-event-codes.h>
#endif

#include "slurg.h"
#include "render.h"

static void noop() {
	// This space intentionally left blank
}


static void send_frame(struct slurg_output *output);

static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct slurg_pointer *pointer = data;
	struct slurg_state *state = pointer->state;

	pointer->x = wl_fixed_to_int(surface_x);
	pointer->y = wl_fixed_to_int(surface_y);

	if (pointer->button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
		// TODO: listen for frame events instead
		struct slurg_output *output;
		wl_list_for_each(output, &state->outputs, link) {
			send_frame(output);
		}
	}
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button,
		uint32_t button_state) {
	struct slurg_pointer *pointer = data;
	struct slurg_state *state = pointer->state;

	pointer->button_state = button_state;

	switch (button_state) {
	case WL_POINTER_BUTTON_STATE_PRESSED:
		pointer->pressed_x = pointer->x;
		pointer->pressed_y = pointer->y;

		struct slurg_output *output;
		wl_list_for_each(output, &state->outputs, link) {
			send_frame(output);
		}
		break;
	case WL_POINTER_BUTTON_STATE_RELEASED:
		pointer_get_box(pointer, &state->result.x, &state->result.y,
			&state->result.width, &state->result.height);
		state->running = false;
		break;
	}
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = noop,
	.leave = noop,
	.motion = pointer_handle_motion,
	.button = pointer_handle_button,
	.axis = noop,
};

static void create_pointer(struct slurg_state *state,
		struct wl_pointer *wl_pointer) {
	struct slurg_pointer *pointer = calloc(1, sizeof(struct slurg_pointer));
	if (pointer == NULL) {
		fprintf(stderr, "allocation failed\n");
		return;
	}
	pointer->state = state;
	pointer->wl_pointer = wl_pointer;
	wl_list_insert(&state->pointers, &pointer->link);

	wl_pointer_add_listener(wl_pointer, &pointer_listener, pointer);
}

static void destroy_pointer(struct slurg_pointer *pointer) {
	wl_list_remove(&pointer->link);
	wl_pointer_destroy(pointer->wl_pointer);
	free(pointer);
}

static int min(int a, int b) {
	return (a < b) ? a : b;
}

void pointer_get_box(struct slurg_pointer *pointer, int *x, int *y,
		int *width, int *height) {
	*x = min(pointer->pressed_x, pointer->x);
	*y = min(pointer->pressed_y, pointer->y);
	*width = abs(pointer->x - pointer->pressed_x);
	*height = abs(pointer->y - pointer->pressed_y);
}


static void seat_handle_capabilities(void *data, struct wl_seat *seat,
		uint32_t capabilities) {
	struct slurg_state *state = data;

	if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
		struct wl_pointer *wl_pointer = wl_seat_get_pointer(seat);
		create_pointer(state, wl_pointer);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
};


static void send_frame(struct slurg_output *output) {
	struct slurg_state *state = output->state;

	if (!output->configured) {
		return;
	}

	output->current_buffer = get_next_buffer(state->shm, output->buffers,
		output->width, output->height);
	if (output->current_buffer == NULL) {
		return;
	}

	render(state, output->current_buffer);

	wl_surface_attach(output->surface, output->current_buffer->buffer, 0, 0);
	wl_surface_damage(output->surface, 0, 0, output->width, output->height);
	wl_surface_commit(output->surface);
}

static void destroy_output(struct slurg_output *output) {
	if (output == NULL) {
		return;
	}
	zwlr_layer_surface_v1_destroy(output->layer_surface);
	wl_surface_destroy(output->surface);
	wl_output_destroy(output->wl_output);
	free(output);
}


static void layer_surface_handle_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct slurg_output *output = data;

	output->configured = true;
	output->width = width;
	output->height = height;

	zwlr_layer_surface_v1_ack_configure(surface, serial);
	send_frame(output);
}

static void layer_surface_handle_closed(void *data,
		struct zwlr_layer_surface_v1 *surface) {
	struct slurg_output *output = data;
	destroy_output(output);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_handle_configure,
	.closed = layer_surface_handle_closed,
};


static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct slurg_state *state = data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name,
			&wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat *seat =
			wl_registry_bind(registry, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, state);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wl_output *wl_output =
			wl_registry_bind(registry, name, &wl_output_interface, 3);
		struct slurg_output *output = calloc(1, sizeof(struct slurg_output));
		output->wl_output = wl_output;
		output->state = state;
		wl_list_insert(&state->outputs, &output->link);
		// TODO: add wl_output listener
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = noop,
};

int main(int argc, char *argv[]) {
	struct slurg_state state = {0};
	wl_list_init(&state.outputs);
	wl_list_init(&state.pointers);

	state.display = wl_display_connect(NULL);
	if (state.display == NULL) {
		fprintf(stderr, "failed to create display\n");
		return EXIT_FAILURE;
	}

	state.registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(state.registry, &registry_listener, &state);
	wl_display_dispatch(state.display);
	wl_display_roundtrip(state.display);

	if (state.compositor == NULL) {
		fprintf(stderr, "compositor doesn't support wl_compositor\n");
		return EXIT_FAILURE;
	}
	if (state.shm == NULL) {
		fprintf(stderr, "compositor doesn't support wl_shm\n");
		return EXIT_FAILURE;
	}
	if (state.layer_shell == NULL) {
		fprintf(stderr, "compositor doesn't support zwlr_layer_shell_v1\n");
		return EXIT_FAILURE;
	}
	if (wl_list_empty(&state.outputs)) {
		fprintf(stderr, "no wl_output\n");
		return EXIT_FAILURE;
	}

	struct slurg_output *output;
	wl_list_for_each(output, &state.outputs, link) {
		output->surface = wl_compositor_create_surface(state.compositor);
		//wl_surface_add_listener(output->surface, &surface_listener, output);

		output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			state.layer_shell, output->surface, output->wl_output,
			ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "selection");
		zwlr_layer_surface_v1_add_listener(output->layer_surface,
			&layer_surface_listener, output);

		zwlr_layer_surface_v1_set_anchor(output->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
		zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
		wl_surface_commit(output->surface);
	}

	state.running = true;
	while (state.running && wl_display_dispatch(state.display) != -1) {
		// This space intentionally left blank
	}

	struct slurg_pointer *pointer, *pointer_tmp;
	wl_list_for_each_safe(pointer, pointer_tmp, &state.pointers, link) {
		destroy_pointer(pointer);
	}
	struct slurg_output *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &state.outputs, link) {
		destroy_output(output);
	}

	if (state.result.width == 0 && state.result.height == 0) {
		fprintf(stderr, "selection cancelled\n");
		return EXIT_FAILURE;
	}

	printf("%d,%d %dx%d\n", state.result.x, state.result.y,
		state.result.width, state.result.height);
	return EXIT_SUCCESS;
}