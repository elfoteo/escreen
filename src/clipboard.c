#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "escreen.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

struct clipboard_context {
	struct escreen_state *state;
	void *data;
	size_t size;
	bool cancelled;
};

static void source_handle_send(void *data, struct zwlr_data_control_source_v1 *source,
		const char *mime_type, int32_t fd) {
	(void)source;
	struct clipboard_context *ctx = data;
	if (strcmp(mime_type, "image/png") == 0) {
		write(fd, ctx->data, ctx->size);
	}
	close(fd);
}

static void source_handle_cancelled(void *data, struct zwlr_data_control_source_v1 *source) {
	(void)source;
	struct clipboard_context *ctx = data;
	ctx->cancelled = true;
}

static const struct zwlr_data_control_source_v1_listener source_listener = {
	.send = source_handle_send,
	.cancelled = source_handle_cancelled,
};

void clipboard_send_data(struct escreen_state *state, void *data, size_t size) {
	if (!state->data_control_manager) {
		fprintf(stderr, "Compositor does not support wlr-data-control\n");
		return;
	}

	struct escreen_seat *seat = wl_container_of(state->seats.next, seat, link);
	struct zwlr_data_control_device_v1 *device = zwlr_data_control_manager_v1_get_data_device(state->data_control_manager, seat->wl_seat);

	struct zwlr_data_control_source_v1 *source = zwlr_data_control_manager_v1_create_data_source(state->data_control_manager);
	struct clipboard_context ctx = {
		.state = state,
		.data = malloc(size),
		.size = size,
		.cancelled = false,
	};
	memcpy(ctx.data, data, size);

	zwlr_data_control_source_v1_add_listener(source, &source_listener, &ctx);
	zwlr_data_control_source_v1_offer(source, "image/png");
	zwlr_data_control_device_v1_set_selection(device, source);

	printf("Screenshot copied to clipboard. Keeping process alive until next clipboard event...\n");

	// Fork to background to serve the clipboard
	pid_t pid = fork();
	if (pid != 0) {
		if (pid < 0) perror("fork failed");
		// Parent exits
		exit(0);
	}

	// Child stays alive
	while (!ctx.cancelled && wl_display_dispatch(state->display) != -1) {
		// Event loop
	}

	zwlr_data_control_source_v1_destroy(source);
	zwlr_data_control_device_v1_destroy(device);
	free(ctx.data);
}
