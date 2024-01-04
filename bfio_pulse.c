/*
 * (c) Copyright 2023 -- chipfunk
 *
 * PulseAudio integration.
 *
 * Implementation based on the, synchronous and blocking, pulse-simple-API.
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

#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

struct settings
{
	int io;

	int sample_rate;
	int open_channels;

	int dummypipe_fd;		// File-descriptor for dummy-pipe.
};

static struct settings *settings;

static pa_simple *pa_handle = NULL;

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
	else
	{
		close (dummypipe[0]);	// Close unused read-end
		settings->dummypipe_fd = dummypipe[1];
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
	settings->sample_rate = sample_rate;
	settings->open_channels = open_channels;

	return settings;
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
	*device_period_size = 4096;
	*isinterleaved = true;

	return dummypipe (io);
}

int
bfio_start (int io)
{
	const char *pa_server = NULL;
	const char *pa_app_name = "BruteFIR";

	const char *pa_device = NULL;
	const pa_sample_spec pa_sample_spec =
	{
	PA_SAMPLE_S16LE, 44100, 2 };
	const pa_channel_map *pa_channel_map = NULL;
	const pa_buffer_attr pa_buffer_attr =
	{ .maxlength = 65536, .tlength = 4096, .prebuf = 4096, .minreq = 0,
			.fragsize = 4096, };
	int errno = 0;

	const char *pa_stream_name = "BruteFIR";
	pa_stream_direction_t pa_stream_direction = PA_STREAM_NODIRECTION;

	if (io == BF_IN)
	{
		pa_stream_direction = PA_STREAM_RECORD;
	}
	else if (io == BF_OUT)
	{
		pa_stream_direction = PA_STREAM_PLAYBACK;
	}
	else
	{
		fprintf (
				stderr,
				"Pulse I/O module could not determine stream-direction, message: %d - %s.\n",
				errno, pa_strerror (errno));
		return -1;
	}

	pa_handle = pa_simple_new (pa_server, pa_app_name, pa_stream_direction,
															pa_device, pa_stream_name, &pa_sample_spec,
															pa_channel_map, &pa_buffer_attr, &errno);
	if (pa_handle == NULL)
	{
		fprintf (stderr,
							"Pulse I/O module failed to open input, message: %d - %s.\n",
							errno, pa_strerror (errno));
		return -1;
	}

	return 0;
}

void
bfio_stop (int io)
{
	close (settings->dummypipe_fd);
	settings->dummypipe_fd = -1;

	/*
	 * @todo: Close resources correctly, while avoiding getting segfault-ed OR
	 * threads deadlocking.
	 *
	 *   pa_simple_flush (pa_handle, NULL);
	 *   pa_simple_free (pa_handle);
	 */
	pa_handle = NULL;
}

int
bfio_read (int fd, void *buf, int offset, int count)
{
	if (pa_handle == NULL) return 0;

	int errno = 0;
	if ((pa_simple_read (pa_handle, buf, count, &errno)) < 0)
	{
		fprintf (stderr, "Pulse I/O module failed to read, message: %d - %s.\n",
							errno, pa_strerror (errno));
		return 0;
	}

	return count;
}

int
bfio_write (int fd, const void *buf, int offset, int count)
{
	if (pa_handle == NULL) return 0;

	int errno = 0;
	if ((pa_simple_write (pa_handle, buf, count, &errno)) < 0)
	{
		fprintf (stderr, "Pulse I/O module failed to write, message: %d - %s.\n",
							errno, pa_strerror (errno));
		return 0;
	}

	return count;
}
