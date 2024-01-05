/*
 * (c) Copyright 2023 -- chipfunk
 *
 * PulseAudio integration.
 *
 * Implementation based on the asynchronous, callback-based pulse-API.
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bit.h"

#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/stream.h>

struct settings
{
	int io;

	int sample_rate;
	int open_channels;

	int dummypipe_fd;		// File-descriptor for dummy-pipe.

	const char *pa_device;
	const char *pa_stream_name;
};

static struct settings *settings;

static pa_mainloop *my_pa_mainloop = NULL;	// primary pulse-API access
static pa_context *my_pa_ctx = NULL;
static pa_stream *my_pa_stream_in;		// keep playback-stream
static pa_stream *my_pa_stream_out;	// keep recording-stream

/**
 * Create a pipe to trap BruteFIR into thinking there is data-available.
 *
 * Code shamelessly copied from `bfio_file.c`.
 *
 * Assumption: For PulseAudio there is always data available or ready
 * to write. If not, the blocking characteristics of pulse-simple-API take
 * care of that.
 *
 * @param io determines the direction of created file-descriptor.
 * @return a file-descriptor to the pipes read OR write end. Returns -1 in case of error.
 */
static int
dummypipe (int io)
{
	int dummypipe[2];
	if (pipe (dummypipe) == -1)
	{
		fprintf (stderr, "Pulse I/O: Could not create pipe.\n");
		return -1;
	}

	if (io == BF_IN)
	{
		close (dummypipe[1]);	// close unused write-end
		settings->dummypipe_fd = dummypipe[0];
	}
	else if (io == BF_OUT)
	{
		close (dummypipe[0]);	// Close unused read-end
		settings->dummypipe_fd = dummypipe[1];
	}
	else
	{
		fprintf (stderr, "Pulse I/O: Invalid IO direction.\n");
		return -1;
	}

	return settings->dummypipe_fd;
}

void*
bfio_preinit (int *version_major, int *version_minor, int
(*get_config_token) (union bflexval *lexval),
							int io, int *sample_format, int sample_rate, int open_channels,
							int *uses_sample_clock, int *callback_sched_policy,
							struct sched_param *callback_sched_param, int _debug)
{
	int ver = *version_major;
	*version_major = BF_VERSION_MAJOR;
	*version_minor = BF_VERSION_MINOR;
	if (ver != BF_VERSION_MAJOR)
	{
		return NULL;
	}

// @todo parse config-params
//  union bflexval lexval;
//  int n, token;

	if (*sample_format == BF_SAMPLE_FORMAT_AUTO)
	{
		fprintf (stderr, "Pulse I/O: No support for AUTO sample format.\n");
		return NULL;
	}

	*sample_format = BF_SAMPLE_FORMAT_S16_LE;

	*uses_sample_clock = 0;

	settings = malloc (sizeof(struct settings));
	memset (settings, 0, sizeof(struct settings));

	settings->io = io;
	settings->pa_device = NULL;
	settings->pa_stream_name = "Asynchronous BruteFIR";
	settings->sample_rate = sample_rate;
	settings->open_channels = open_channels;

	return settings;
}

static void
_pa_context_event_cb (pa_context *c, const char *name, pa_proplist *p,
											void *userdata)
{
	fprintf (stderr, "Pulse I/O context event callback, state %s.\n",
						pa_proplist_to_string (p));
}

static void
_pa_stream_event_cb (pa_stream *p, const char *name, pa_proplist *pl,
											void *userdata)
{
	pa_stream_state_t state = pa_stream_get_state (my_pa_stream_in);
	fprintf (stderr, "Pulse I/O stream-event callback, state %d.\n", state);
}

static int
_pa_open_stream (int io, const char *pa_device, const char *pa_stream_name)
{
	const pa_sample_spec pa_sample_spec =
	{ PA_SAMPLE_S16LE, 44100, 2 };
	const pa_channel_map *pa_channel_map = NULL;

	pa_proplist *my_pa_stream_proplist = pa_proplist_new ();
	pa_stream_flags_t my_pa_stream_flags = PA_STREAM_NOFLAGS;

	const pa_buffer_attr pa_buffer_attr =
	{ .maxlength = 65536, .tlength = -1, .prebuf = -1, .minreq = -1,
			.fragsize = -1, };

	my_pa_stream_out = pa_stream_new_with_proplist (my_pa_ctx, pa_stream_name,
																									&pa_sample_spec,
																									pa_channel_map,
																									my_pa_stream_proplist);

	if (io == BF_IN)
	{
		if (pa_stream_connect_record (my_pa_stream_in, pa_device, &pa_buffer_attr,
																	my_pa_stream_flags) != 0)
		{
			fprintf (stderr,
								"Pulse I/O error connecting recording-stream, code %d.\n",
								pa_context_errno (my_pa_ctx));
			return -1;
		}
	}
	else if (io == BF_OUT)
	{
		pa_cvolume *my_pa_volume = NULL;
		pa_stream *my_pa_sync_stream = NULL;

		if (pa_stream_connect_playback (my_pa_stream_out, pa_device,
																		&pa_buffer_attr, my_pa_stream_flags,
																		my_pa_volume, my_pa_sync_stream) != 0)
		{
			fprintf (stderr, "Pulse I/O error connecting playback-stream, code %d.\n",
								pa_context_errno (my_pa_ctx));
			return -1;
		}
	}
	else
	{
		fprintf ( stderr,
							"Pulse I/O module could not determine stream-direction.\n");
		return -1;
	}

	pa_stream_set_event_callback (my_pa_stream_in, _pa_stream_event_cb,
	NULL);
	pa_stream_set_event_callback (my_pa_stream_out, _pa_stream_event_cb,
	NULL);

	return 0;
}

/**
 * Callback whenever pulseaudio-context/-connection changes state.
 */
static void
_pa_state_callback (pa_context *c, void *userdata)
{
	pa_context_state_t state = pa_context_get_state (my_pa_ctx);
	fprintf (stderr, "Pulse I/O context callback, state %d.\n", state);

	switch (state)
	{
		case PA_CONTEXT_UNCONNECTED:
			fprintf (stderr, "Pulse I/O context not connected, state %d.\n", state);
			break; /**< The context hasn't been connected yet */
		case PA_CONTEXT_CONNECTING:
			fprintf (stderr, "Pulse I/O connecting, state %d.\n", state);
			break; /**< A connection is being established */
		case PA_CONTEXT_AUTHORIZING:
			fprintf (stderr, "Pulse I/O authorizing, state %d.\n", state);
			break; /**< The client is authorizing itself to the daemon */
		case PA_CONTEXT_SETTING_NAME:
			fprintf (stderr, "Pulse I/O setting name, state %d.\n", state);
			break; /**< The client is passing its application name to the daemon */
		case PA_CONTEXT_READY:
			fprintf (stderr, "Pulse I/O context ready, state %d.\n", state);
			_pa_open_stream (BF_IN, settings->pa_device, settings->pa_stream_name);
			_pa_open_stream (BF_OUT, settings->pa_device, settings->pa_stream_name);
			break; /**< The connection is established, the context is ready to execute operations */
		case PA_CONTEXT_FAILED:
			fprintf (stderr, "Pulse I/O context failed, state %d.\n", state);
			break; /**< The connection failed or was disconnected */
		case PA_CONTEXT_TERMINATED:
			fprintf (stderr, "Pulse I/O context terminated, state %d.\n", state);
			break; /**< The connection was terminated cleanly */
	}
}

static void
_pa_context_subscribe_cb (pa_context *c, pa_subscription_event_type_t t,
													uint32_t idx, void *userdata)
{
	fprintf (stderr, "Pulse I/O context subscribe.\n");
}

static int
_pa_open_connection (pa_mainloop_api *my_pa_api)
{
	const char *pa_server = NULL;
	const char *pa_app_name = "BruteFIR";

	const char *pa_device = NULL;

	const char *pa_stream_name = "BruteFIR";

	pa_proplist *my_pa_ctx_proplist = pa_proplist_new ();
	my_pa_ctx = pa_context_new_with_proplist (my_pa_api, "my context",
																						my_pa_ctx_proplist);

	pa_context_set_state_callback (my_pa_ctx, _pa_state_callback, NULL);
	pa_context_set_subscribe_callback (my_pa_ctx, _pa_context_subscribe_cb, NULL);
	pa_context_set_event_callback (my_pa_ctx, _pa_context_event_cb, NULL);

	pa_context_flags_t my_pa_context_flags = PA_CONTEXT_NOFLAGS;
	my_pa_context_flags |= PA_CONTEXT_NOAUTOSPAWN;

	pa_spawn_api my_ctx_spawn_api = {};

	if (pa_context_connect (my_pa_ctx, pa_server, my_pa_context_flags,
													&my_ctx_spawn_api) < 0)
	{
		fprintf (stderr, "Pulse I/O connection error, code %d.\n",
							pa_context_errno (my_pa_ctx));
		return -1;
	}

	return 0;
}

int
bfio_init (
		void *params,
		int io,
		int sample_format,
		int sample_rate,
		int open_channels,
		int used_channels,
		const int channel_selection[],
		int period_size,
		int *device_period_size,
		int *isinterleaved,
		void *callback_state,
		int
		(*process_callback) (void **callback_states[2], int callback_state_count[2],
													void **buffers[2], int frame_count, int event))
{

	my_pa_mainloop = pa_mainloop_new ();
	pa_mainloop_api *my_pa_api = pa_mainloop_get_api (my_pa_mainloop);

	_pa_open_connection (my_pa_api);

	pa_mainloop_run(my_pa_mainloop, 0);

	*device_period_size = 4096;
	*isinterleaved = true;

	return dummypipe (io);
}

int
bfio_start (int io)
{
	return 0;
}

void
bfio_stop (int io)
{
	close (settings->dummypipe_fd);
	settings->dummypipe_fd = -1;

	if (pa_stream_disconnect (my_pa_stream_out) != 0)
	{
		fprintf (stderr,
							"Pulse I/O error disconnecting playback-stream, code %d.\n",
							pa_context_errno (my_pa_ctx));
	}

	if (pa_stream_disconnect (my_pa_stream_in) != 0)
	{
		fprintf (stderr,
							"Pulse I/O error disconnecting recording-stream, code %d.\n",
							pa_context_errno (my_pa_ctx));
	}

	pa_mainloop_free (my_pa_mainloop);
}

int
bfio_read (int fd, void *buf, int offset, int count)
{
	size_t nbytes = count;
	if (pa_stream_peek (my_pa_stream_in, buf, &nbytes) < 0)
	{
		fprintf (stderr, "Pulse I/O error reading recording-stream, code %d.\n",
							pa_context_errno (my_pa_ctx));
		return -1;
	}

	return count;
}

int
bfio_write (int fd, const void *buf, int offset, int count)
{
	if (pa_stream_write (my_pa_stream_out, buf, count, NULL, 0, PA_SEEK_RELATIVE)
			!= 0)
	{
		fprintf (stderr, "Pulse I/O error connecting playback-stream, code %d.\n",
							pa_context_errno (my_pa_ctx));
		return -1;
	}

	return count;
}
