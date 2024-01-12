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

#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bfconf.h"
#include "bit.h"

#define GET_TOKEN(token, errstr)                                               \
    if (get_config_token(&lexval) != token) {                                  \
        fprintf(stderr, "Pulse I/O: Parse error: " errstr);                     \
        return -1;                                                           \
    }

struct settings
{
  // Dummy-pipe value
  int dummypipe_fd;		// File-descriptor for dummy-pipe.

  // PulseAUdio values
  char *app_name;		// The name of this application as shown in PA
  char *server;		// Name of server to connect to, NULL for default
  char *stream_name;	// The stream-name as shown in PA
  char *device;		// Device-name to connect to, or NULL for default
  struct pa_sample_spec sample_spec;
  struct pa_buffer_attr *buffer_attr;
};

static struct settings *my_params[2];// Keep per in/out-stream, because ... fork()/threading?
static pa_simple *pa_handle[2];	// Keep per in/out-stream, because ... fork()/threading?

bool_t debug = false;

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
create_dummypipe (const int io)
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
      my_params[io]->dummypipe_fd = dummypipe[0];
    }
  else if (io == BF_OUT)
    {
      close (dummypipe[0]);	// Close unused read-end
      my_params[io]->dummypipe_fd = dummypipe[1];
    }
  else
    {
      fprintf (stderr, "Pulse I/O: Invalid IO direction.\n");
      return -1;
    }

  return my_params[io]->dummypipe_fd;
}

static int
check_version (const int *version_major, const int *version_minor)
{
  if (*version_major != BF_VERSION_MAJOR)
    {
      return false;
    }

  if (*version_minor != BF_VERSION_MINOR)
    {
      return false;
    }

  return true;
}

static void
init_settings (const int io, const int sample_rate, const int open_channels)
{
  my_params[io] = malloc (sizeof(struct settings));
  memset (my_params[io], 0, sizeof(struct settings));

  my_params[io]->buffer_attr = NULL;
}

static int
parse_config_options_buffer_attr (const int io, pa_buffer_attr *buffer_attr, int
(*get_config_token) (union bflexval *lexval))
{
  buffer_attr->maxlength = -1;
  buffer_attr->tlength = -1;
  buffer_attr->prebuf = -1;
  buffer_attr->minreq = -1;
  buffer_attr->fragsize = -1;

  union bflexval lexval;
  int token;

  if (BF_LEX_LBRACE != get_config_token (&lexval))
    {
      fprintf (stderr, "Pulse I/O: Parse error: expected {.\n");
      return -1;
    }

  while ((token = get_config_token (&lexval)) > 0)
    {
      if (token == BF_LEX_RBRACE)
	{
	  return 0;
	}
      else if (token == BF_LEXVAL_FIELD)
	{
	  if (strcmp (lexval.field, "maxlength") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_REAL,
			"\"maxlength\" expects integer from -1 and up.\n");
	      buffer_attr->maxlength = (int) lexval.real;
	    }
	  else if (strcmp (lexval.field, "tlength") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_REAL, "expected integer from -1 and up.\n");
	      buffer_attr->tlength = (int) lexval.real;
	    }
	  else if (strcmp (lexval.field, "prebuf") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_REAL, "expected integer from -1 and up.\n");
	      buffer_attr->prebuf = (int) lexval.real;
	    }
	  else if (strcmp (lexval.field, "minreq") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_REAL, "expected integer from -1 and up.\n");
	      buffer_attr->minreq = (int) lexval.real;
	    }
	  else if (strcmp (lexval.field, "fragsize") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_REAL, "expected integer from -1 and up.\n");
	      buffer_attr->fragsize = (int) lexval.real;
	    }
	  else
	    {
	      fprintf (stderr, "Pulse I/O: Parse error: unknown field.\n");
	      return -1;
	    }
	  GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
	}
      else
	{
	  fprintf (stderr, "Pulse I/O: Parse error: expected field.\n");
	  return -1;
	}
    }

  return 0;
}

/**
 * Read settings from config-file.
 *
 * Returns 0 on success, negative values on failure.
 */
static int
parse_config_options (const int io, int
(*get_config_token) (union bflexval *lexval))
{
  union bflexval lexval;
  int token;
  while ((token = get_config_token (&lexval)) > 0)
    {
      if (token == BF_LEXVAL_FIELD)
	{
	  if (strcmp (lexval.field, "server") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
	      my_params[io]->server = strdup (lexval.string);
	    }
	  else if (strcmp (lexval.field, "device") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
	      my_params[io]->device = strdup (lexval.string);
	    }
	  else if (strcmp (lexval.field, "buffer_attr") == 0)
	    {
	      my_params[io]->buffer_attr = malloc (sizeof(pa_buffer_attr));
	      memset (my_params[io]->buffer_attr, 0, sizeof(pa_buffer_attr));

	      parse_config_options_buffer_attr (io, my_params[io]->buffer_attr,
						get_config_token);

	      if (debug)
		fprintf (
		    stderr,
		    "Pulse I/O: configure buffer attributes, maxlength: %d, tlength: %d, prebuf: %d, minreq: %d, fragsize: %d\n",
		    my_params[io]->buffer_attr->maxlength,
		    my_params[io]->buffer_attr->tlength,
		    my_params[io]->buffer_attr->prebuf,
		    my_params[io]->buffer_attr->minreq,
		    my_params[io]->buffer_attr->fragsize);
	    }
	  else if (strcmp (lexval.field, "app_name") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
	      my_params[io]->app_name = strdup (lexval.string);
	    }
	  else if (strcmp (lexval.field, "stream_name") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
	      my_params[io]->stream_name = strdup (lexval.string);
	    }
	  else
	    {
	      fprintf (stderr, "Pulse I/O: Parse error: unknown field.\n");
	      return -1;
	    }
	  GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
	}
      else
	{
	  fprintf (stderr, "Pulse I/O: Parse error: expected field.\n");
	  return -1;
	}
    }

  if (my_params[io]->app_name == NULL)
    my_params[io]->app_name = "BruteFIR";
  if (my_params[io]->stream_name == NULL)
    my_params[io]->stream_name = "BruteFIR stream";

  return 0;
}

/**
 * Detect appropriate sample-format for PA.
 *
 * @param bf_sample_format The sample-format requested by BruteFIR
 * @return PA sample-format to use, or PA_SAMPLE_INVALID if no sample-format could be found.
 */
static const pa_sample_format_t
detect_pa_sample_format (const int bf_sample_format)
{
  switch (bf_sample_format)
    {
    case BF_SAMPLE_FORMAT_AUTO:
#ifdef LITTLE_ENDIAN
      return PA_SAMPLE_S32LE;
#else if BIG_ENDIAN
			return PA_SAMPLE_S32BE;
#endif
    case BF_SAMPLE_FORMAT_S8:
      return PA_SAMPLE_U8;
    case BF_SAMPLE_FORMAT_S16_LE:
      return PA_SAMPLE_S16LE;
    case BF_SAMPLE_FORMAT_S16_BE:
      return PA_SAMPLE_S16BE;
    case BF_SAMPLE_FORMAT_S24_LE:
      return PA_SAMPLE_S24LE;
    case BF_SAMPLE_FORMAT_S24_BE:
      return PA_SAMPLE_S24BE;
    case BF_SAMPLE_FORMAT_S24_4LE:
      return PA_SAMPLE_S24_32LE;
    case BF_SAMPLE_FORMAT_S24_4BE:
      return PA_SAMPLE_S24_32BE;
    case BF_SAMPLE_FORMAT_S32_LE:
      return PA_SAMPLE_S32LE;
    case BF_SAMPLE_FORMAT_S32_BE:
      return PA_SAMPLE_S32BE;
    case BF_SAMPLE_FORMAT_FLOAT_LE:
      return PA_SAMPLE_FLOAT32LE;
    case BF_SAMPLE_FORMAT_FLOAT_BE:
      return PA_SAMPLE_FLOAT32BE;
    default:
      // noop
    }

  return PA_SAMPLE_INVALID;
}

void*
bfio_preinit (int *version_major, int *version_minor, int
(*get_config_token) (union bflexval *lexval),
	      int io, int *sample_format, int sample_rate, int open_channels,
	      int *uses_sample_clock, int *callback_sched_policy,
	      struct sched_param *callback_sched_param, int _debug)
{
  debug = _debug;

  if (!check_version (version_major, version_minor))
    {
      fprintf (
	  stderr,
	  "Pulse I/O: Mismatching version-numbers. Expected BF_VERSION_MAJOR.BF_VERSION_MINOR, got %d.%d.\n",
	  *version_major, *version_minor);
      return NULL;
    }

  init_settings (io, sample_rate, open_channels);

  if (parse_config_options (io, get_config_token) < 0)
    {
      fprintf (stderr, "Pulse I/O: Error parsing options.\n");
      return NULL;
    }

  *uses_sample_clock = 0;

  return (void*) my_params[io];
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
  *device_period_size = period_size;
  *isinterleaved = true;

  pa_sample_format_t pa_sample_format = detect_pa_sample_format (sample_format);
  if (pa_sample_format == PA_SAMPLE_INVALID)
    {
      fprintf (stderr,
	       "Pulse I/O: Could not find appropriate sample-format for PA.\n");
      return -1;
    }

  my_params[io]->sample_spec.format = pa_sample_format;
  my_params[io]->sample_spec.rate = sample_rate;
  my_params[io]->sample_spec.channels = open_channels;

  // Set low-latency PA-buffer-attribs if none configured
  if (my_params[io]->buffer_attr == NULL)
    {
      my_params[io]->buffer_attr = malloc (sizeof(pa_buffer_attr));
      memset (my_params[io]->buffer_attr, 0, sizeof(pa_buffer_attr));

      my_params[io]->buffer_attr->maxlength = -1;
      my_params[io]->buffer_attr->tlength = -1;
      my_params[io]->buffer_attr->prebuf = -1;
      my_params[io]->buffer_attr->minreq = -1;
      my_params[io]->buffer_attr->fragsize = -1;

      uint32_t nbytes = period_size
	  * pa_sample_size (&my_params[io]->sample_spec);
      if (io == BF_IN)
	{
	  my_params[io]->buffer_attr->fragsize = nbytes;
	}
      else if (io == BF_OUT)
	{
	  my_params[io]->buffer_attr->tlength = nbytes;
	}
      else
	{
	  fprintf (stderr, "Pulse I/O: Cannot determine stream-direction.\n");
	  return -1;
	}
    }

  return create_dummypipe (io);
}

static pa_simple*
_pa_simple_open (const char *server, const char *app_name, const char *device,
		 const char *stream_name,
		 const pa_stream_direction_t stream_direction,
		 const pa_sample_spec *sample_spec,
		 const pa_channel_map *channel_map,
		 const pa_buffer_attr *buffer_attr)
{
  if (debug)
    fprintf (
	stderr,
	"Pulse I/O: buffer attributes, maxlength: %d, tlength: %d, prebuf: %d, minreq: %d, fragsize: %d\n",
	buffer_attr->maxlength, buffer_attr->tlength, buffer_attr->prebuf,
	buffer_attr->minreq, buffer_attr->fragsize);

  int errno = 0;
  pa_simple *handle = pa_simple_new (server, app_name, stream_direction, device,
				     stream_name, sample_spec, channel_map,
				     buffer_attr, &errno);

  if (handle == NULL)
    {
      fprintf (stderr,
	       "Pulse I/O could not open connection/stream, code %d - %s.\n",
	       errno, pa_strerror (errno));
      return NULL;

    }

  return handle;
}

/**
 * Initializing PA-connection here to avoid fork()-ing after bfio_init().
 */
int
bfio_start (const int io)
{
  pa_stream_direction_t stream_direction;
  if (io == BF_IN)
    {
      stream_direction = PA_STREAM_RECORD;
    }
  else if (io == BF_OUT)
    {
      stream_direction = PA_STREAM_PLAYBACK;
    }
  else
    {
      fprintf (stderr,
	       "Pulse I/O module could not determine stream-direction.\n");
      return -1;
    }

  pa_handle[io] = _pa_simple_open (my_params[io]->server,
				   my_params[io]->app_name,
				   my_params[io]->device,
				   my_params[io]->stream_name, stream_direction,
				   &my_params[io]->sample_spec,
				   NULL,
				   my_params[io]->buffer_attr);

  if (pa_handle[io] == NULL)
    {
      return -1;
    }

  return 0;
}

void
bfio_stop (const int io)
{
  close (my_params[io]->dummypipe_fd);
  my_params[io]->dummypipe_fd = -1;

  /*
   * @todo: Close resources correctly, while avoiding getting segfault-ed OR
   * threads deadlocking.
   *
   *
   *   pa_simple_flush (pa_handle[io], NULL);
   *   pa_simple_free (pa_handle[io]);
   */
  pa_handle[io] = NULL;
}

int
bfio_read (const int fd, void *buf, const int offset, const int count)
{
  if (pa_handle[BF_IN] == NULL)
    return 0;

  int errno = 0;
  if ((pa_simple_read (pa_handle[BF_IN], buf, count, &errno)) < 0)
    {
      fprintf (stderr, "Pulse I/O module failed to read, message: %d - %s.\n",
	       errno, pa_strerror (errno));
      return 0;
    }

  return count;
}

int
bfio_write (const int fd, const void *buf, const int offset, const int count)
{
  if (pa_handle[BF_OUT] == NULL)
    return 0;

  int errno = 0;
  if ((pa_simple_write (pa_handle[BF_OUT], buf, count, &errno)) < 0)
    {
      fprintf (stderr, "Pulse I/O module failed to write, message: %d - %s.\n",
	       errno, pa_strerror (errno));
      return 0;
    }

  return count;
}
