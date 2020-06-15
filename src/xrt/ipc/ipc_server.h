// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common server side code.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc_server
 */

#pragma once

#include "xrt/xrt_compiler.h"

#include "os/os_threading.h"

#include "ipc_protocol.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * Logging
 *
 */

/*!
 * Spew level logging.
 */
#define IPC_SPEW(c, ...)                                                       \
	do {                                                                   \
		if (c->print_spew) {                                           \
			fprintf(stderr, "%s - ", __func__);                    \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
		}                                                              \
	} while (false)

/*!
 * Debug level logging.
 */
#define IPC_DEBUG(c, ...)                                                      \
	do {                                                                   \
		if (c->print_debug) {                                          \
			fprintf(stderr, "%s - ", __func__);                    \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
		}                                                              \
	} while (false)


/*
 *
 * Structs
 *
 */

#define IPC_SERVER_NUM_XDEVS 8
#define IPC_MAX_CLIENT_SWAPCHAINS 32
#define IPC_MAX_CLIENTS 8

struct xrt_instance;
struct xrt_compositor;
struct xrt_compositor_fd;
struct ipc_wait;


/*!
 * Information about a single swapchain.
 *
 * @ingroup ipc_server
 */
struct ipc_swapchain_data
{
	uint32_t width;
	uint32_t height;
	uint64_t format;
	uint32_t num_images;

	bool active;
};

/*!
 * Holds the state for a single client.
 *
 * @ingroup ipc_server
 */
struct ipc_client_state
{
	//! Link back to the main server.
	struct ipc_server *server;

	//! Compositor for this client.
	struct xrt_compositor *xc;

	//! Number of swapchains in use by client
	uint32_t num_swapchains;

	//! Ptrs to the swapchains
	struct xrt_swapchain *xscs[IPC_MAX_CLIENT_SWAPCHAINS];

	//! Data for the swapchains.
	struct ipc_swapchain_data swapchain_data[IPC_MAX_CLIENT_SWAPCHAINS];

	//! Socket fd used for client comms
	int ipc_socket_fd;

	//! State for rendering.
	struct ipc_layer_slot render_state;

	//! Whether we are currently rendering @ref render_state
	bool rendering_state;

	bool active;
};

/*!
 * Main IPC object for the server.
 *
 * @ingroup ipc_server
 */
struct ipc_server
{
	struct xrt_instance *xinst;

	struct xrt_compositor *xc;
	struct xrt_compositor_fd *xcfd;

	struct xrt_device *xdevs[IPC_SERVER_NUM_XDEVS];
	struct xrt_tracking_origin *xtracks[IPC_SERVER_NUM_XDEVS];

	struct ipc_shared_memory *ism;
	int ism_fd;

	//! Socket that we accept connections on.
	int listen_socket;

	//! For waiting on various events in the main thread.
	int epoll_fd;

	// Is the mainloop supposed to run.
	volatile bool running;

	// Should we exit when a client disconnects.
	bool exit_on_disconnect;

	//! Were we launched by socket activation, instead of explicitly?
	bool launched_by_socket;

	//! The socket filename we bound to, if any.
	char *socket_filename;

	bool print_debug;
	bool print_spew;

	// Hack for now.
	struct ipc_wait *iw;
	struct os_thread thread;
	volatile bool thread_started;
	volatile bool thread_stopping;
	volatile struct ipc_client_state thread_state;
};

/*!
 * Main entrypoint to the compositor process.
 *
 * @ingroup ipc_server
 */
int
ipc_server_main(int argc, char **argv);

/*!
 * Thread function for the client side dispatching.
 *
 * @ingroup ipc_server
 */
void *
ipc_server_client_thread(void *_cs);

/*!
 * Create a single wait thread.
 *
 * @ingroup ipc_server
 * @public @memberof ipc_server
 * @relatesalso ipc_wait
 */
int
ipc_server_wait_alloc(struct ipc_server *s, struct ipc_wait **out_iw);

/*!
 * Destroy a wait thread, checks for NULL and sets to NULL.
 *
 * @ingroup ipc_server
 * @public @memberof ipc_wait
 */
void
ipc_server_wait_free(struct ipc_wait **out_iw);

/*!
 * Add a client to wait for wait frame, if need be start waiting for the next
 * wait frame.
 *
 * @ingroup ipc_server
 * @public @memberof ipc_wait
 */
void
ipc_server_wait_add_frame(struct ipc_wait *iw,
                          volatile struct ipc_client_state *cs);

/*!
 * Reset the wait state for wait frame, after the client disconnected
 *
 * @ingroup ipc_server
 * @public @memberof ipc_wait
 */
void
ipc_server_wait_reset_client(struct ipc_wait *iw,
                             volatile struct ipc_client_state *cs);

#ifdef __cplusplus
}
#endif
