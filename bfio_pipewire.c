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

#include <spa/pod/builder.h>
#include <spa/param/latency-utils.h>

#include <pipewire/pipewire.h>
#include <pipewire/core.h>
#include <pipewire/filter.h>
#include <pipewire/thread-loop.h>
#include <spa/param/audio/format.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bfconf.h"

#define GET_TOKEN(token, errstr)                                   \
    if (get_config_token(&lexval) != token) {                      \
        fprintf(stderr, "PipeWire I/O: Parse error: " errstr);     \
        return -1;                                                 \
    }

typedef struct
{
  int io;

  int dummypipe_fd;

  char *server;
  char *device;
  char *app_name;
  char *stream_name;

  struct pw_thread_loop *pw_main_loop;
  struct pw_context *pw_context;
  struct pw_core *pw_core;

  struct pw_registry *pw_registry;
  struct spa_hook pw_registry_listener;

  struct pw_filter *pw_filter;
  struct spa_hook pw_filter_listener;

  struct pw_port *pw_port;
  struct spa_hook pw_port_listener;

} settings_t;

settings_t *my_params[2];

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
      fprintf (stderr, "PipeWire I/O: Could not create pipe.\n");
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
      fprintf (stderr, "PipeWire I/O: Invalid IO direction.\n");
      return -1;
    }

  if (debug)
    fprintf (stderr, "PipeWire I/O::create_dummypipe, %d\n", dummypipe_fd);

  return dummypipe_fd;
}

/**
 * Detect appropriate sample-format for PipeWire.
 *
 * @param bf_sample_format The sample-format requested by BruteFIR
 * @return PipeWire sample-format to use, or SPA_AUDIO_FORMAT_UNKNOWN if no sample-format could be found.
 */
static const enum spa_audio_format
detect_pw_sample_format (const int bf_sample_format)
{
  switch (bf_sample_format)
    {
    case BF_SAMPLE_FORMAT_AUTO:
#ifdef LITTLE_ENDIAN
      return SPA_AUDIO_FORMAT_F32_LE;
#elif BIG_ENDIAN
      return SPA_AUDIO_FORMAT_F32_BE;
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

static void
_pw_registry_global_cb (void *data, uint32_t id, uint32_t permissions,
			const char *type, uint32_t version,
			const struct spa_dict *props)
{
  if (debug)
    {
      fprintf (
	  stderr,
	  "PipeWire I/O::_pw_registry_global_cb, object: id:%u type:%s/%d.\n",
	  id, type, version);

      const struct spa_dict_item *item;
      spa_dict_for_each(item, props)
	printf ("\t\t%s: \"%s\"\n", item->key, item->value);
    }
}

static void
_pw_registry_global_remove_cb (void *data, uint32_t id)
{
  if (debug)
    fprintf (stderr,
	     "PipeWire I/O::_pw_registry_global_remove_cb, object: %d.\n", id);
}

static const struct pw_registry_events registry_events =
  { PW_VERSION_REGISTRY_EVENTS, .global = _pw_registry_global_cb,
      .global_remove = _pw_registry_global_remove_cb, };

static void
_pw_filter_command_cb (void *data, const struct spa_command *command)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_filter_command_cb.\n");

}

static void
_pw_filter_drained_cb (void *data)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_filter_drained_cb\n");

}

static void
_pw_filter_destroy_cb (void *data)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_filter_destroy_cb\n");

}

/**
 * Event handler to process filter
 */
static void
_pw_filter_process_cb (void *data, struct spa_io_position *position)
{
  settings_t *settings = data;

  float *out;
  uint32_t i, n_samples = position->clock.duration;

  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_filter_process_cb\n");

  out = pw_filter_get_dsp_buffer (settings->pw_port, n_samples);
  if (out == NULL)
    return;

//  struct pw_buffer *buffer;
//  if ((buffer = pw_filter_dequeue_buffer (settings->pw_filter)) == NULL)
//    {
//      fprintf (stderr,
//	       "PipeWire I/O::on_filter_process can not dequeue buffer\n");
//      return;
//    }
//
//  pw_filter_queue_buffer (settings->pw_filter, buffer);
}

static void
_pw_filter_add_buffer_cb (void *data, void *port_data, struct pw_buffer *buffer)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_filter_add_buffer_cb\n");
}

static void
_pw_filter_remove_buffer_cb (void *data, void *port_data,
			     struct pw_buffer *buffer)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_filter_remove_buffer_cb\n");
}

static void
_pw_filter_io_changed_cb (void *data, void *port_data, uint32_t id, void *area,
			  uint32_t size)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_filter_io_changed_cb, %d, %p, %d.\n",
	     id, area, size);
}

static void
_pw_filter_param_changed_cb (void *data, void *port_data, uint32_t id,
			     const struct spa_pod *param)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_filter_param_changed_cb, %d.\n", id);
}

static void
_pw_filter_state_changed_cb (void *data, enum pw_filter_state old,
			     enum pw_filter_state state, const char *error)
{
  if (debug)
    fprintf (stderr,
	     "PipeWire I/O::_pw_filter_state_changed_cb, from: %s to %s.\n",
	     pw_filter_state_as_string (old),
	     pw_filter_state_as_string (state));

  switch (state)
    {
    case PW_FILTER_STATE_ERROR:
      break;
    case PW_FILTER_STATE_UNCONNECTED:
      break;
    case PW_FILTER_STATE_CONNECTING:
      break;
    case PW_FILTER_STATE_PAUSED:
      break;
    case PW_FILTER_STATE_STREAMING:
      break;
    }
}

static const struct pw_filter_events filter_events =
  { PW_VERSION_FILTER_EVENTS, .process = _pw_filter_process_cb, .add_buffer =
      _pw_filter_add_buffer_cb, .command = _pw_filter_command_cb, .destroy =
      _pw_filter_destroy_cb, .drained = _pw_filter_drained_cb, .io_changed =
      _pw_filter_io_changed_cb, .param_changed = _pw_filter_param_changed_cb,
      .remove_buffer = _pw_filter_remove_buffer_cb, .state_changed =
	  _pw_filter_state_changed_cb, };

static void
_pw_port_info_cb (void *data, const struct pw_port_info *info)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_port_info_cb.\n");

}

static void
_pw_port_param_cb (void *data, int seq, uint32_t id, uint32_t index,
		   uint32_t next, const struct spa_pod *param)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_port_param_cb, id: %d\n", id);

}

static const struct pw_port_events port_events =
      {
      PW_VERSION_PORT_EVENTS, .info = _pw_port_info_cb, .param =
	  _pw_port_param_cb, };

void*
bfio_preinit (int *version_major, int *version_minor, int
(*get_config_token) (union bflexval *lexval),
	      int io, int *sample_format, int sample_rate, int open_channels,
	      int *uses_sample_clock, int *callback_sched_policy,
	      struct sched_param *callback_sched, int _debug)
{
  debug = _debug;

  if (debug)
    fprintf (stderr, "PipeWire I/O::preinit, %d\n", io);

  if (!check_version (version_major, version_minor))
    {
      fprintf (
	  stderr,
	  "PipeWire I/O: Mismatching version-numbers. Expected BF_VERSION_MAJOR.BF_VERSION_MINOR, got %d.%d.\n",
	  *version_major, *version_minor);
      return NULL;
    }

  settings_t *settings = malloc (sizeof(settings_t));
  memset (settings, 0, sizeof(settings_t));

  if (parse_config_options (io, settings, get_config_token) < 0)
    {
      fprintf (stderr, "PipeWire I/O: Error parsing options.\n");
      return NULL;
    }

  *uses_sample_clock = 0;

  return settings;
}

static int
setup_pw_context (int io)
{
  struct pw_properties *context_props = NULL;

  size_t user_data_size = 0;

  my_params[io]->pw_context = pw_context_new (
      pw_thread_loop_get_loop (my_params[io]->pw_main_loop), context_props,
      user_data_size);

  struct pw_properties *core_props = NULL;

  my_params[io]->pw_core = pw_context_connect (my_params[io]->pw_context,
					       core_props, user_data_size);
  if (my_params[io]->pw_core == NULL)
    {
      fprintf (stderr, "PipeWire I/O::init can not connect context\n");
      return -1;
    }

  my_params[io]->pw_registry = pw_core_get_registry (my_params[io]->pw_core,
  PW_VERSION_REGISTRY,
						     user_data_size);

  spa_zero(my_params[io]->pw_registry_listener);
  pw_registry_add_listener(my_params[io]->pw_registry,
			   &my_params[io]->pw_registry_listener,
			   &registry_events, my_params[io]);

  return 0;
}

static int
setup_pw_filter (int io)
{
  struct pw_properties *filter_props = NULL;
  my_params[io]->pw_filter = pw_filter_new (my_params[io]->pw_core,
					    "Filter name", filter_props);

  spa_zero(my_params[io]->pw_filter_listener);
  pw_filter_add_listener (my_params[io]->pw_filter,
			  &my_params[io]->pw_filter_listener, &filter_events,
			  my_params[io]);

  return 0;
}

static int
setup_pw_port (int io)
{
  if (io == BF_IN)
    {
      my_params[io]->pw_port = pw_filter_add_port (
	  my_params[io]->pw_filter,
	  PW_DIRECTION_INPUT,
	  PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(settings_t),
	  pw_properties_new (
	  PW_KEY_FORMAT_DSP,
			     "32 bit float mono audio",
			     PW_KEY_PORT_NAME,
			     "input",
			     NULL),
	  NULL, 0);
    }
  else if (io == BF_OUT)
    {
      my_params[io]->pw_port = pw_filter_add_port (
	  my_params[io]->pw_filter,
	  PW_DIRECTION_OUTPUT,
	  PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(settings_t),
	  pw_properties_new (
	  PW_KEY_FORMAT_DSP,
			     "32 bit float mono audio",
			     PW_KEY_PORT_NAME,
			     "output",
			     NULL),
	  NULL, 0);
    }
  else
    {
      fprintf (stderr,
	       "PipeWire I/O::init can not determine stream direction.\n");
      return -1;
    }

  if (my_params[io]->pw_port == NULL)
    {
      fprintf (stderr, "PipeWire I/O::init can not create %s-port.",
	       io == BF_IN ? "input" : "output");
      return -1;
    }

  spa_zero(my_params[io]->pw_port_listener);
  pw_port_add_listener(my_params[io]->pw_port, &my_params[io]->pw_port_listener,
		       &port_events, NULL);

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
  if (debug)
    fprintf (stderr, "PipeWire I/O::init, %d, %p\n", io, params);

  my_params[io] = params;

  my_params[io]->dummypipe_fd = create_dummypipe (io);
  if (my_params[io]->dummypipe_fd < 0)
    {
      fprintf (stderr, "PipeWire I/O: Error creating dummy-pipe.\n");
      return -1;
    }

  enum spa_audio_format audio_format = detect_pw_sample_format (sample_format);
  if (audio_format == SPA_AUDIO_FORMAT_UNKNOWN)
    {
      fprintf (stderr, "PipeWire I/O: Error detecting audio-format.\n");
      return -1;
    }

  pw_init (NULL, NULL);

  const struct spa_dict *loop_props =
    { 0, };

  my_params[io]->pw_main_loop = pw_thread_loop_new ("thread-loop-name",
						    loop_props);

  if (setup_pw_context (io) < 0)
    {
      fprintf (stderr, "PipeWire I/O::init can not setup PipeWire-context.\n");
      return -1;
    }

  if (setup_pw_filter (io) < 0)
    {
      fprintf (stderr, "PipeWire I/O::init can not setup PipeWire-filter.\n");
      return -1;
    }

  if (setup_pw_port (io) < 0)
    {
      fprintf (stderr, "PipeWire I/O::init can not setup PipeWire-port.\n");
      return -1;
    }

  const struct spa_pod *pw_params[1];
  if (pw_filter_connect (my_params[io]->pw_filter, PW_FILTER_FLAG_RT_PROCESS,
			 pw_params, 0) < 0)
    {
      fprintf (stderr, "PipeWire I/O::pw_filter_connect can't connect\n");
      return -1;
    }

  return my_params[io]->dummypipe_fd;
}

int
bfio_start (int io)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::start, %d, %p\n", io, my_params[io]);

  pw_thread_loop_start (my_params[io]->pw_main_loop);

  return 0;
}

void
bfio_stop (int io)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::stop, %d, %p\n", io, my_params[io]);

  pw_thread_loop_stop (my_params[io]->pw_main_loop);

  pw_filter_disconnect (my_params[io]->pw_filter);
  pw_filter_destroy (my_params[io]->pw_filter);

  pw_core_disconnect (my_params[io]->pw_core);
  pw_context_destroy (my_params[io]->pw_context);
  pw_thread_loop_destroy (my_params[io]->pw_main_loop);

  pw_deinit ();

  free (my_params[io]);

  close (my_params[io]->dummypipe_fd);
}

int
bfio_read (int fd, void *buf, int offset, int count)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::read, %d, %p, %d, %d\n", fd, buf, offset,
	     count);

  return count;

}

int
bfio_write (int fd, const void *buf, int offset, int count)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::write, %d, %p, %d, %d\n", fd, buf, offset,
	     count);

  return count;

}

