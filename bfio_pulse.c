/*
 * bfio_pulse.c
 *
 */
#include <stdio.h>
#include <string.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bit.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

struct settings
{
  bool_t initialized;
};

static pa_simple *pa_input_sink = NULL;
static pa_simple *pa_output_source = NULL;

void*
bfio_preinit (int *version_major, int *version_minor, int
(*get_config_token) (union bflexval *lexval),
	      int io, int *sample_format, int sample_rate, int open_channels,
	      int *uses_sample_clock, int *callback_sched_policy,
	      struct sched_param *callback_sched_param, int _debug)
{
  fprintf (stderr, "pulse::pre-init, message: %d.\n", io);

  int ver = *version_major;
  *version_major = BF_VERSION_MAJOR;
  *version_minor = BF_VERSION_MINOR;
  if (ver != BF_VERSION_MAJOR)
    {
      return NULL;
    }

  struct settings *settings;
  union bflexval lexval;
  int n, token;

  settings = malloc (sizeof(struct settings));
  memset (settings, 0, sizeof(struct settings));

  if (*sample_format == BF_SAMPLE_FORMAT_AUTO)
    {
      fprintf (stderr, "Pulse I/O: No support for AUTO sample format.\n");
      return NULL;
    }

  *uses_sample_clock = 0;

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
  fprintf (stderr, "pulse::init, message: %d.\n", io);

  *device_period_size = 4096;
  *isinterleaved = true;

  return 0;
}

int
bfio_read (int fd, void *buf, int offset, int count)
{
  fprintf (stderr, "pulse::read, message: %d, %d, %d.\n", fd, offset, count);

  int errno = 0;

  if (pa_input_sink == NULL)
    return 0;

  if ((pa_simple_read (pa_input_sink, buf, count, &errno)) < 0)
    {
      fprintf (stderr, "Pulse I/O module failed to read, message: %d - %s.\n",
	       errno, pa_strerror (errno));
      return -1;
    }

  return count;
}

int
bfio_write (int fd, const void *buf, int offset, int count)
{
  fprintf (stderr, "pulse::write, message: %d, %d, %d.\n", fd, offset, count);

  int errno = 0;

  if (pa_output_source == NULL)
    return 0;

  if ((pa_simple_write (pa_output_source, buf, count, &errno)) < 0)
    {
      fprintf (stderr, "Pulse I/O module failed to write, message: %d - %s.\n",
	       errno, pa_strerror (errno));
      return -1;
    }

  return count;
}

int
bfio_start (int io)
{
  fprintf (stderr, "pulse::start, %d.\n", io);

  const char *pa_server = NULL;
  const char *pa_app_name = "BruteFIR";

  const char *pa_device = NULL;
  const pa_sample_spec pa_sample_spec =
    {
    PA_SAMPLE_S16LE, 44100, 2 };
  const pa_channel_map *pa_channel_map = NULL;
  const pa_buffer_attr *pa_buffer_attr = NULL;
  int errno = 0;

  if (io == BF_IN)
    {
      pa_input_sink = pa_simple_new (NULL, pa_app_name,
      PA_STREAM_RECORD,
				     NULL, "Input to BruteFIR", &pa_sample_spec,
				     NULL,
				     NULL, &errno);
      if (pa_input_sink == NULL)
	{
	  fprintf (stderr,
		   "Pulse I/O module failed to open input, message: %d - %s.\n",
		   errno, pa_strerror (errno));
	}
    }
  else if (io == BF_OUT)
    {
      pa_output_source = pa_simple_new (pa_server, pa_app_name,
      PA_STREAM_PLAYBACK,
					pa_device, "Output from BruteFIR",
					&pa_sample_spec, pa_channel_map,
					pa_buffer_attr, &errno);
      if (pa_output_source == NULL)
	{
	  fprintf (
	      stderr,
	      "Pulse I/O module failed to open output, message: %d - %s.\n",
	      errno, pa_strerror (errno));
	}
    }

  return 0;
}

void
bfio_stop (int io)
{
  fprintf (stderr, "pulse::stop, %d.\n", io);

  if (io == BF_IN)
    {
      pa_simple_free (pa_input_sink);
      pa_input_sink = NULL;
    }
  else if (io == BF_OUT)
    {
      pa_simple_free (pa_output_source);
      pa_output_source = NULL;
    }
}
