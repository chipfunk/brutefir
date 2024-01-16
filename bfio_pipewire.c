/*
 * (c) Copyright 2024 -- chipfunk
 *
 * PipeWire I/O module.
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bfconf.h"

#define GET_TOKEN(token, errstr)                                   \
    if (get_config_token(&lexval) != token) {                      \
        fprintf(stderr, "Pulse I/O: Parse error: " errstr);        \
        return -1;                                                 \
    }

typedef struct
{
  int dummypipe_fd;

  char *server;
  char *device;
  char *app_name;
  char *stream_name;

  struct pw_loop *pw_main_loop;
  struct pw_context *pw_context;
  struct pw_core *pw_core;
  struct pw_stream *pw_stream;
} settings_t;

settings_t *my_params;

bool debug = false;

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
  int dummypipe_fd = -1;

  int dummypipe[2];
  if (pipe (dummypipe) == -1)
    {
      fprintf (stderr, "Pulse I/O: Could not create pipe.\n");
      return -1;
    }

  if (io == BF_IN)
    {
      close (dummypipe[1]);	// close unused write-end
      dummypipe_fd = dummypipe[0];
    }
  else if (io == BF_OUT)
    {
      close (dummypipe[0]);	// Close unused read-end
      dummypipe_fd = dummypipe[1];
    }
  else
    {
      fprintf (stderr, "Pulse I/O: Invalid IO direction.\n");
      return -1;
    }

  return dummypipe_fd;
}

/**
 * Detect appropriate sample-format for Pipewire.
 *
 * @param bf_sample_format The sample-format requested by BruteFIR
 * @return Pipewire sample-format to use, or PA_SAMPLE_INVALID if no sample-format could be found.
 */
static const enum spa_audio_format
detect_pw_sample_format (const int bf_sample_format)
{
  switch (bf_sample_format)
    {
    case BF_SAMPLE_FORMAT_AUTO:
#ifdef LITTLE_ENDIAN
      return SPA_AUDIO_FORMAT_F32_LE;
#else if BIG_ENDIAN
      return SPA_AUDIO_FORMAT_F32_LE;
#endif
    case BF_SAMPLE_FORMAT_S8:
      return SPA_AUDIO_FORMAT_U8;
    case BF_SAMPLE_FORMAT_S16_LE:
      return SPA_AUDIO_FORMAT_S16_LE;
    case BF_SAMPLE_FORMAT_S16_BE:
      return SPA_AUDIO_FORMAT_S16_BE;
    case BF_SAMPLE_FORMAT_S24_LE:
      return SPA_AUDIO_FORMAT_S24_LE;
    case BF_SAMPLE_FORMAT_S24_BE:
      return SPA_AUDIO_FORMAT_S24_BE;
    case BF_SAMPLE_FORMAT_S24_4LE:
      return SPA_AUDIO_FORMAT_S24_32_LE;
    case BF_SAMPLE_FORMAT_S24_4BE:
      return SPA_AUDIO_FORMAT_S24_32_BE;
    case BF_SAMPLE_FORMAT_S32_LE:
      return SPA_AUDIO_FORMAT_S32_LE;
    case BF_SAMPLE_FORMAT_S32_BE:
      return SPA_AUDIO_FORMAT_S32_BE;
    case BF_SAMPLE_FORMAT_FLOAT_LE:
      return SPA_AUDIO_FORMAT_F32_LE;
    case BF_SAMPLE_FORMAT_FLOAT_BE:
      return SPA_AUDIO_FORMAT_F32_BE;
    default:
      // noop
    }

  return SPA_AUDIO_FORMAT_UNKNOWN;
}

/**
 * Read settings from config-file.
 *
 * Returns 0 on success, negative values on failure.
 */
static int
parse_config_options (const int io, settings_t *settings, int
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
	      settings->server = strdup (lexval.string);
	    }
	  else if (strcmp (lexval.field, "device") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
	      settings->device = strdup (lexval.string);
	    }
	  else if (strcmp (lexval.field, "app_name") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
	      settings->app_name = strdup (lexval.string);
	    }
	  else if (strcmp (lexval.field, "stream_name") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
	      settings->stream_name = strdup (lexval.string);
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

  if (settings->app_name == NULL)
    settings->app_name = "BruteFIR";
  if (settings->stream_name == NULL)
    settings->stream_name = "BruteFIR stream";

  return 0;
}

void*
bfio_preinit (int *version_major, int *version_minor, int
(*get_config_token) (union bflexval *lexval),
	      int io, int *sample_format, int sample_rate, int open_channels,
	      int *uses_sample_clock, int *callback_sched_policy,
	      struct sched_param *callback_sched, int _debug)
{
  debug = _debug;

  if (debug)
    fprintf (stderr, "Pipewire I/O::preinit, %d\n", io);

  if (!check_version (version_major, version_minor))
    {
      fprintf (
	  stderr,
	  "Pulse I/O: Mismatching version-numbers. Expected BF_VERSION_MAJOR.BF_VERSION_MINOR, got %d.%d.\n",
	  *version_major, *version_minor);
      return NULL;
    }

  settings_t *settings = malloc (sizeof(settings_t));
  memset (settings, 0, sizeof(settings_t));

  if (parse_config_options (io, settings, get_config_token) < 0)
    {
      fprintf (stderr, "Pulse I/O: Error parsing options.\n");
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
  if (debug)
    fprintf (stderr, "Pipewire I/O::init, %d, %p\n", io, params);

  my_params = params;

  my_params->dummypipe_fd = create_dummypipe(io);
  if(my_params->dummypipe_fd < 0) {
      fprintf (stderr, "Pulse I/O: Error creating dummy-pipe.\n");
      return -1;
  }

  enum spa_audio_format audio_format = detect_pw_sample_format(sample_format);
  if(audio_format == SPA_AUDIO_FORMAT_UNKNOWN) {
      fprintf (stderr, "Pulse I/O: Error detecting audio-format.\n");
      return -1;
  }

  pw_init (NULL, NULL);

  const struct spa_dict *loop_props =
    { 0, };

  my_params->pw_main_loop = pw_loop_new (loop_props);

  struct pw_properties *context_props = NULL;

  size_t user_data_size = 0;

  my_params->pw_context = pw_context_new (my_params->pw_main_loop,
					  context_props, user_data_size);

  struct pw_properties *core_props = NULL;

  my_params->pw_core = pw_context_connect (my_params->pw_context, core_props,
					   user_data_size);
  if (my_params->pw_core == NULL)
    {
      fprintf (stderr, "Pipewire I/O::init can NOT connect context\n");
      return -1;
    }

  struct pw_properties *stream_props = NULL;

  const char *stream_name = "Steam name";

  my_params->pw_stream = pw_stream_new (my_params->pw_core, stream_name,
					stream_props);

  return my_params->dummypipe_fd;
}

int
bfio_start (int io)
{
  if (debug)
    fprintf (stderr, "Pipewire I/O::start, %d, %p\n", io, my_params);

  return 0;
}

void
bfio_stop (int io)
{
  if (debug)
    fprintf (stderr, "Pipewire I/O::stop, %d, %p\n", io, my_params);

}

int
bfio_read (int fd, void *buf, int offset, int count)
{
  if (debug)
    fprintf (stderr, "Pipewire I/O::read, %d\n", fd);

  return 0;

}

int
bfio_write (int fd, const void *buf, int offset, int count)
{
  if (debug)
    fprintf (stderr, "Pipewire I/O::write, %d\n", fd);

  return 0;

}

