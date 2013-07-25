/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>

#include "compositor.h"

struct clipboard_source {
	struct weston_data_source base;
	struct wl_array contents;
	struct clipboard *clipboard;
	struct wl_event_source *event_source;
	uint32_t serial;
	int refcount;
	int fd;
};

struct clipboard {
	struct weston_seat *seat;
	struct wl_listener selection_listener;
	struct wl_listener destroy_listener;
	struct clipboard_source *source;
};

static void clipboard_client_create(struct clipboard_source *source, int fd);

static void
clipboard_source_unref(struct clipboard_source *source)
{
	char **s;

	source->refcount--;
	if (source->refcount > 0)
		return;

	if (source->event_source) {
		wl_event_source_remove(source->event_source);
		close(source->fd);
	}
	wl_signal_emit(&source->base.destroy_signal,
		       &source->base);
	s = source->base.mime_types.data;
	free(*s);
	wl_array_release(&source->base.mime_types);
	wl_array_release(&source->contents);
	free(source);
}

static int
clipboard_source_data(int fd, uint32_t mask, void *data)
{
	struct clipboard_source *source = data;
	struct clipboard *clipboard = source->clipboard;
	char *p;
	int len, size;

	if (source->contents.alloc - source->contents.size < 1024) {
		wl_array_add(&source->contents, 1024);
		source->contents.size -= 1024;
	}

	p = source->contents.data + source->contents.size;
	size = source->contents.alloc - source->contents.size;
	len = read(fd, p, size);
	if (len == 0) {
		wl_event_source_remove(source->event_source);
		close(fd);
		source->event_source = NULL;
	} else if (len < 0) {
		clipboard_source_unref(source);
		clipboard->source = NULL;
	} else {
		source->contents.size += len;
	}

	return 1;
}

static void
clipboard_source_accept(struct weston_data_source *source,
			uint32_t time, const char *mime_type)
{
}

static void
clipboard_source_send(struct weston_data_source *base,
		      const char *mime_type, int32_t fd)
{
	struct clipboard_source *source =
		container_of(base, struct clipboard_source, base);
	char **s;

	s = source->base.mime_types.data;
	if (strcmp(mime_type, s[0]) == 0)
		clipboard_client_create(source, fd);
	else
		close(fd);
}

static void
clipboard_source_cancel(struct weston_data_source *source)
{
}

static struct clipboard_source *
clipboard_source_create(struct clipboard *clipboard,
			const char *mime_type, uint32_t serial, int fd)
{
	struct wl_display *display = clipboard->seat->compositor->wl_display;
	struct wl_event_loop *loop = wl_display_get_event_loop(display);
	struct clipboard_source *source;
	char **s;

	source = malloc(sizeof *source);
	if (source == NULL)
		return NULL;

	wl_array_init(&source->contents);
	wl_array_init(&source->base.mime_types);
	source->base.resource = NULL;
	source->base.accept = clipboard_source_accept;
	source->base.send = clipboard_source_send;
	source->base.cancel = clipboard_source_cancel;
	wl_signal_init(&source->base.destroy_signal);
	source->refcount = 1;
	source->clipboard = clipboard;
	source->serial = serial;

	s = wl_array_add(&source->base.mime_types, sizeof *s);
	if (s == NULL)
		goto err_add;
	*s = strdup(mime_type);
	if (*s == NULL)
		goto err_strdup;
	source->event_source =
		wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				     clipboard_source_data, source);
	if (source->event_source == NULL)
		goto err_source;

	return source;

 err_source:
	free(*s);
 err_strdup:
	wl_array_release(&source->base.mime_types);
 err_add:
	free(source);

	return NULL;
}

struct clipboard_client {
	struct wl_event_source *event_source;
	size_t offset;
	struct clipboard_source *source;
};

static int
clipboard_client_data(int fd, uint32_t mask, void *data)
{
	struct clipboard_client *client = data;
	char *p;
	size_t size;
	int len;

	size = client->source->contents.size;
	p = client->source->contents.data;
	len = write(fd, p + client->offset, size - client->offset);
	if (len > 0)
		client->offset += len;

	if (client->offset == size || len <= 0) {
		close(fd);
		wl_event_source_remove(client->event_source);
		clipboard_source_unref(client->source);
		free(client);
	}

	return 1;
}

static void
clipboard_client_create(struct clipboard_source *source, int fd)
{
	struct weston_seat *seat = source->clipboard->seat;
	struct clipboard_client *client;
	struct wl_event_loop *loop =
		wl_display_get_event_loop(seat->compositor->wl_display);

	client = malloc(sizeof *client);

	client->offset = 0;
	client->source = source;
	source->refcount++;
	client->event_source =
		wl_event_loop_add_fd(loop, fd, WL_EVENT_WRITABLE,
				     clipboard_client_data, client);
}

static void
clipboard_set_selection(struct wl_listener *listener, void *data)
{
	struct clipboard *clipboard =
		container_of(listener, struct clipboard, selection_listener);
	struct weston_seat *seat = data;
	struct weston_data_source *source = seat->selection_data_source;
	const char **mime_types;
	int p[2];

	if (source == NULL) {
		if (clipboard->source)
			weston_seat_set_selection(seat,
						  &clipboard->source->base,
						  clipboard->source->serial);
		return;
	} else if (source->accept == clipboard_source_accept) {
		/* Callback for our data source. */
		return;
	}

	if (clipboard->source)
		clipboard_source_unref(clipboard->source);

	clipboard->source = NULL;

	mime_types = source->mime_types.data;

	if (pipe2(p, O_CLOEXEC) == -1)
		return;

	source->send(source, mime_types[0], p[1]);

	clipboard->source =
		clipboard_source_create(clipboard, mime_types[0],
					seat->selection_serial, p[0]);
	if (clipboard->source == NULL) {
		close(p[0]);
		return;
	}
}

static void
clipboard_destroy(struct wl_listener *listener, void *data)
{
	struct clipboard *clipboard =
		container_of(listener, struct clipboard, destroy_listener);

	wl_list_remove(&clipboard->selection_listener.link);

	free(clipboard);
}

struct clipboard *
clipboard_create(struct weston_seat *seat)
{
	struct clipboard *clipboard;

	clipboard = malloc(sizeof *clipboard);
	if (clipboard == NULL)
		return NULL;
	memset(clipboard, 0, sizeof *clipboard);

	clipboard->seat = seat;
	clipboard->selection_listener.notify = clipboard_set_selection;
	clipboard->destroy_listener.notify = clipboard_destroy;

	wl_signal_add(&seat->selection_signal,
		      &clipboard->selection_listener);
	wl_signal_add(&seat->destroy_signal,
		      &clipboard->destroy_listener);

	return clipboard;
}
