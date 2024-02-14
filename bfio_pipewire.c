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

#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>

#include <pipewire/pipewire.h>
#include <pipewire/core.h>
#include <pipewire/filter.h>
#include <pipewire/thread-loop.h>
#include <pipewire/port.h>
#include <pipewire/stream.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bfconf.h"

static bool debug = false;

#define GET_TOKEN(token, errstr)                              \
  if (get_config_token(&lexval) != token)                     \
    {                                                         \
      fprintf(stderr, "PipeWire I/O: Parse error: " errstr);  \
      return -1;                                              \
    }

static int
(*_bf_process_callback) (void **states[2], int state_count[2], void **bufs[2],
			 int frame_count, int event);
typedef struct
{
  struct pw_thread_loop *main_loop;
  struct spa_hook main_loop_listener;

  struct pw_context *context;
  struct spa_hook context_listener;

  struct pw_core *core;
  struct spa_hook core_listener;

  struct pw_registry *registry;
  struct spa_hook registry_listener;

  struct pw_filter *filter;
  struct spa_hook filter_listener;

  struct pw_port *port;
  void *port_data;
  struct spa_hook port_listener;

  int direction;

} pipewire_t;

typedef struct
{
  int device_no;
  int io;

  int open_channels;
  int sample_rate;

  char *server;
  char *device_name;
  char *app_name;
  char *stream_name;

  pipewire_t pipewire;
} params_t;

static params_t device[BF_MAXMODULES];
static uint8_t device_count = 0;

static void **bf_states[2];

typedef struct
{
  void *states[2];
  int state_count[2];
} bf_callback_t;

typedef void *port_data_t;

typedef struct
{
  struct pw_filter *filter;
  bf_callback_t bf_callback;
  port_data_t ports[2][BF_MAXCHANNELS];
  int port_count[2];
} filter_data_t;

static filter_data_t filter_data;

static int
check_version (const int *version_major, const int *version_minor)
{
  if (*version_major != BF_VERSION_MAJOR)
    return false;

  if (*version_minor != BF_VERSION_MINOR)
    return false;

  return true;
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
parse_config_options (params_t *settings, int
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
	      settings->device_name = strdup (lexval.string);
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
  {
  PW_VERSION_REGISTRY_EVENTS, .global = _pw_registry_global_cb, .global_remove =
      _pw_registry_global_remove_cb, };

static void
_pw_port_info_cb (void *data, const struct pw_port_info *info)
{
  data = (port_data_t*) data;

  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_port_info_cb, %s.\n",
	     pw_direction_as_string (info->direction));
}

static void
_pw_port_param_cb (void *data, int seq, uint32_t id, uint32_t index,
		   uint32_t next, const struct spa_pod *param)
{
  data = (port_data_t*) data;

  if (debug)
    fprintf (stderr,
	     "PipeWire I/O::_pw_port_param_cb, seq: %d, id: %d, index: %d.\n",
	     seq, id, index);
}

static const struct pw_port_events port_events =
      {
      PW_VERSION_PORT_EVENTS, .info = _pw_port_info_cb, .param =
	  _pw_port_param_cb, };

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
  struct pw_buffer *pw_buffers[2][BF_MAXCHANNELS];

  void *in_bufs[BF_MAXCHANNELS], *out_bufs[BF_MAXCHANNELS];
  void **bf_buffers[2];

  bf_buffers[IN] = in_bufs;
  bf_buffers[OUT] = out_bufs;

  uint64_t n_samples = position->clock.duration;

  float null_buffer[n_samples];

  if (debug)
    fprintf (
	stderr,
	"PipeWire I/O::_pw_filter_process_cb, data: %p, n_samples: %d, state: %d\n",
	data, n_samples, position->state);

//  FOR_IN_AND_OUT
//    {
//      for (int channel = 0; channel < filter_data.port_count[IO]; channel++)
//	{
//	  bf_buffers[IO][channel] = NULL;
//
//	  pw_buffers[IO][channel] = pw_filter_dequeue_buffer (
//	      filter_data.ports[IO][channel]);
//	  if (pw_buffers[IO][channel] == NULL)
//	    {
//	      bf_buffers[IO][channel] = &null_buffer;
//	    }
//	  else
//	    {
//	      bf_buffers[IO][channel] =
//		  pw_buffers[IO][channel]->buffer->datas->data;
//	    }
//	}
//    }

//  int bf_state_count[2] =
//    { 1, 1 };

//  int xyz = _bf_process_callback (bf_states, bf_state_count, bf_buffers,
//				  n_samples,
//				  BF_CALLBACK_EVENT_NORMAL);

//  FOR_IN_AND_OUT
//    {
//      for (int channel = 0; channel < filter_data.port_count[IO]; channel++)
//	{
//	  if (pw_buffers[IO][channel] != NULL)
//	    {
//	      pw_filter_queue_buffer (filter_data.ports[IO][channel],
//				      pw_buffers[IO][channel]);
//	    }
//	}
//    }
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
    fprintf (stderr,
	     "PipeWire I/O::_pw_filter_io_changed_cb, %d, %p, %p, %p, %d.\n",
	     id, data, port_data, area, size);
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
	     "PipeWire I/O::_pw_filter_state_changed_cb, %p, from: %s to %s.\n",
	     data, pw_filter_state_as_string (old),
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

static struct pw_filter_events filter_events =
  { PW_VERSION_FILTER_EVENTS, .process = _pw_filter_process_cb, .add_buffer =
      _pw_filter_add_buffer_cb, .command = _pw_filter_command_cb, .destroy =
      _pw_filter_destroy_cb, .drained = _pw_filter_drained_cb, .io_changed =
      _pw_filter_io_changed_cb, .param_changed = _pw_filter_param_changed_cb,
      .remove_buffer = _pw_filter_remove_buffer_cb, .state_changed =
	  _pw_filter_state_changed_cb, };

static void
_pw_main_loop_destroy_cb (void *data)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_main_loop_destroy_cb.\n");
}

static const struct pw_thread_loop_events main_loop_events =
  {
  PW_VERSION_MAIN_LOOP_EVENTS, .destroy = _pw_main_loop_destroy_cb, };

static void
_pw_core_add_mem_cb (void *data, uint32_t id, uint32_t type, int fd,
		     uint32_t flags)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_core_add_mem_cb.\n");
}

static void
_pw_core_bound_id_cb (void *data, uint32_t id, uint32_t global_id)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_core_bound_id_cb.\n");
}

static void
_pw_core_bound_props_cb (void *data, uint32_t id, uint32_t global_id,
			 const struct spa_dict *props)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_core_bound_props_cb.\n");
}

static void
_pw_core_done_cb (void *data, uint32_t id, int seq)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_core_done_cb.\n");
}

static void
_pw_core_error_cb (void *data, uint32_t id, int seq, int res,
		   const char *message)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_core_error_cb.\n");
}

static void
_pw_core_info_cb (void *data, const struct pw_core_info *info)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_core_info_cb.\n");
}

static void
_pw_core_ping_cb (void *data, uint32_t id, int seq)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_core_ping_cb.\n");
}

static void
_pw_core_remove_id_cb (void *data, uint32_t id)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_core_remove_id_cb.\n");
}

static void
_pw_core_remove_mem_cb (void *data, uint32_t id)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_core_remove_mem_cb.\n");
}

static const struct pw_core_events core_events =
      { PW_VERSION_CORE_EVENTS,

      .add_mem = _pw_core_add_mem_cb, .bound_id = _pw_core_bound_id_cb,
	  .bound_props = _pw_core_bound_props_cb, .done = _pw_core_done_cb,
	  .error = _pw_core_error_cb, .info = _pw_core_info_cb, .ping =
	      _pw_core_ping_cb, .remove_id = _pw_core_remove_id_cb,
	  .remove_mem = _pw_core_remove_mem_cb, };

static void
_pw_context_check_access_cb (void *data, struct pw_impl_client *client)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_context_check_access_cb.\n");
}

static void
_pw_context_destroy_cb (void *data)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_context_destroy_cb.\n");
}

static void
_pw_context_free_cb (void *data)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_context_free_cb.\n");
}

static void
_pw_context_driver_added_cb (void *data, struct pw_impl_node *node)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_context_driver_added_cb.\n");
}

static void
_pw_context_driver_removed_cb (void *data, struct pw_impl_node *node)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_context_driver_removed_cb.\n");
}

static void
_pw_context_global_added_cb (void *data, struct pw_global *global)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_context_global_added_cb.\n");
}

static void
_pw_context_global_removed_cb (void *data, struct pw_global *global)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::_pw_context_global_removed_cb.\n");
}

static const struct pw_context_events context_events =
  {
  PW_VERSION_CONTEXT_EVENTS, .check_access = _pw_context_check_access_cb,
      .destroy = _pw_context_destroy_cb, .free = _pw_context_free_cb,
      .driver_added = _pw_context_driver_added_cb, .driver_removed =
	  _pw_context_driver_removed_cb, .global_added =
	  _pw_context_global_added_cb, .global_removed =
	  _pw_context_global_removed_cb };

static int
init_pw_registry (struct pw_core *core, struct spa_hook *listener,
		  const struct pw_registry_events *events,
		  size_t user_data_size, void *user_data)
{
  struct pw_registry *registry = pw_core_get_registry (core,
  PW_VERSION_REGISTRY,
						       user_data_size);

//  spa_zero(listener);
  pw_registry_add_listener(registry, listener, events, user_data);

  return 0;
}

static int
init_pipewire_port (const int i)
{
  enum spa_direction direction =
	  (device[i].io == BF_IN) ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;

  for (uint8_t channel = 0; channel < device[i].open_channels; channel++)
	{
	  char channel_name[32];
	  sprintf (channel_name,
		   (direction == SPA_DIRECTION_INPUT) ? "in %d" : "out %d",
		   channel);

	  fprintf (stderr,
		   "PipeWire I/O::init port, device: %d, channel: %d.\n", i,
		   channel);

	  struct pw_properties *port_props = pw_properties_new (
	      PW_KEY_FORMAT_DSP, "32 bit float mono audio",
	      PW_KEY_PORT_NAME,
	      channel_name,
//	      PW_KEY_AUDIO_CHANNELS,
//	      "2",
	      NULL);

	  if (device[i].device_name != NULL)
	    pw_properties_set (port_props, PW_KEY_TARGET_OBJECT,
			       device[i].device_name);

	  device[i].pipewire.port_data = pw_filter_add_port (
	      device[i].pipewire.filter, direction,
	      PW_FILTER_PORT_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_AUTOCONNECT, 0,
	      port_props,
	      NULL,
	      0);

	  if (device[i].pipewire.port_data == NULL)
	    {
	      fprintf (stderr,
		       "PipeWire I/O::init can not setup PipeWire-port.\n");
	      return -1;
	    }

//	  spa_zero(device[i].pipewire.port_listener);
//	  pw_port_add_listener(device[i].pipewire.port_data,
//			       &device[i].pipewire.port_listener, &port_events,
//			       NULL);

//	  filter_data.ports[IO][channel] = port_data;
	}

  return 0;
}

static int
init_pipewire_filter (const int i)
{
  const char *filter_name = "Filter-name";

  struct pw_properties *filter_props = pw_properties_new (
  PW_KEY_MEDIA_TYPE,
							  "Audio",
							  PW_KEY_MEDIA_CATEGORY,
							  "Filter",
							  PW_KEY_MEDIA_ROLE,
							  "DSP",
							  PW_KEY_AUDIO_CHANNELS,
							  "2",
							  NULL);

  device[i].pipewire.filter = pw_filter_new (device[i].pipewire.core,
					     filter_name, filter_props);
  if (device[i].pipewire.filter == NULL)
    {
      fprintf (
      stderr,
	       "PipeWire I/O::bfio_init_pw_filter can not create filter.\n");
      return -1;
    }

  spa_zero(device[i].pipewire.filter_listener);
  pw_filter_add_listener (device[i].pipewire.filter,
			  &device[i].pipewire.filter_listener, &filter_events,
			  NULL);

  /* Make one parameter with the supported formats. The SPA_PARAM_EnumFormat
   * id means that this is a format enumeration (of 1 value).
   * We leave the channels and rate empty to accept the native graph
   * rate and channels. */
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

  const struct spa_pod *params[1];
  params[0] = spa_format_audio_raw_build (&b, SPA_PARAM_EnumFormat,
					  &SPA_AUDIO_INFO_RAW_INIT(.format =
					      SPA_AUDIO_FORMAT_F32));

  if (pw_filter_connect (device[i].pipewire.filter, PW_FILTER_FLAG_ASYNC,
			 params, 0) < 0)
    {
      fprintf (stderr, "PipeWire I/O::bfio_init_pw_filter can not connect.\n");
      return -1;
    }

  return 0;
}

static int
init_pipewire_context (const int i)
{
  struct spa_dict *main_loop_props =
    { 0, };

  device[i].pipewire.main_loop = pw_thread_loop_new ("loop-name",
						     main_loop_props);
  if (device[i].pipewire.main_loop == NULL)
    {
      fprintf (stderr,
	       "PipeWire I/O::init can not setup PipeWire-main-loop.\n");
      return -1;
    }

  spa_zero(device[i].pipewire.main_loop_listener);
  pw_thread_loop_add_listener (device[i].pipewire.main_loop,
			       &device[i].pipewire.main_loop_listener,
			       &main_loop_events,
			       NULL);

  struct pw_properties *context_props = NULL;

  device[i].pipewire.context = pw_context_new (
      pw_thread_loop_get_loop (device[i].pipewire.main_loop), context_props, 0);

  if (device[i].pipewire.context == NULL)
    {
      fprintf (stderr, "PipeWire I/O::init can not setup PipeWire-context.\n");
      return -1;
    }

  spa_zero(device[i].pipewire.context_listener);
  pw_context_add_listener (device[i].pipewire.context,
			   &device[i].pipewire.context_listener,
			   &context_events, NULL);

  struct pw_properties *core_props = NULL;

  device[i].pipewire.core = pw_context_connect (device[i].pipewire.context,
						core_props, 0);
  if (device[i].pipewire.core == NULL)
    {
      fprintf (stderr, "PipeWire I/O::init can not connect context\n");
      return -1;
    }

  spa_zero(device[i].pipewire.core_listener);
  pw_core_add_listener(device[i].pipewire.core,
		       &device[i].pipewire.core_listener, &core_events, NULL);

  device[i].pipewire.registry = pw_core_get_registry (device[i].pipewire.core,
						      PW_VERSION_REGISTRY, 0);

  spa_zero(device[i].pipewire.registry_listener);
  pw_registry_add_listener(device[i].pipewire.registry,
			   &device[i].pipewire.registry_listener,
			   &registry_events, NULL);

  return 0;
}

static int
init_pipewire ();
static int
init_pipewire ()
{
  pw_init (NULL, NULL);

  for (int i = 0; i < device_count; i++)
    {
      if (debug)
	fprintf (stderr, "PipeWire I/O::synch_start, device: %d.\n", i);

      if (init_pipewire_context (i) < 0)
	{
	  fprintf (
	      stderr,
	      "PipeWire I/O::synch_start, can not init pipewire, device: %d.\n",
	      i);
	  return -1;
	}

      if (init_pipewire_filter (i) < 0)
	{
	  fprintf (
	      stderr,
	      "PipeWire I/O::synch_start, can not init filter, device: %d.\n",
	      i);
	  return -1;
	}

      if (init_pipewire_port (i) < 0)
	{
	  fprintf (
	      stderr,
	      "PipeWire I/O::synch_start, can not init ports, device: %d.\n",
	      i);
	  return -1;
	}
    }

  return 0;
}

static void
print_params (params_t *params)
{
  fprintf (stderr, "PipeWire I/O:device-no: %d\n", params->device_no);
  fprintf (stderr, "PipeWire I/O:io: %d\n", params->io);
  fprintf (stderr, "PipeWire I/O:server: %s\n", params->server);
  fprintf (stderr, "PipeWire I/O:app-name: %s\n", params->app_name);
  fprintf (stderr, "PipeWire I/O:stream-name: %s\n", params->stream_name);
  fprintf (stderr, "PipeWire I/O:server: channels: %d, sample-rate: %d\n",
	   params->open_channels, params->sample_rate);
  fprintf (stderr, "PipeWire I/O:device-name: %s\n", params->device_name);
}

int
bfio_iscallback (void)
{
  return true;
}

void*
bfio_preinit (int *version_major, int *version_minor, int
(*get_config_token) (union bflexval *lexval),
	      int io, int *sample_format, int sample_rate, int open_channels,
	      int *uses_sample_clock, int *callback_sched_policy,
	      struct sched_param *callback_sched, int _debug)
{
  debug = _debug;

  memset (callback_sched, 0, sizeof(*callback_sched));
  *callback_sched_policy = SCHED_FIFO;

  if (debug)
    fprintf (stderr, "PipeWire I/O::preinit, device: %d, io: %d\n",
	     device_count, io);

  if (!check_version (version_major, version_minor))
    {
      fprintf (
	  stderr,
	  "PipeWire I/O: Mismatching version-numbers. Expected BF_VERSION_MAJOR.BF_VERSION_MINOR, got %d.%d.\n",
	  *version_major, *version_minor);
      return NULL;
    }

  params_t *params = malloc (sizeof(params_t));
  memset (params, 0, sizeof(params_t));

  if (debug)
    fprintf (stderr, "PipeWire I/O::preinit, params: %p\n", params);

  if (parse_config_options (params, get_config_token) < 0)
    {
      fprintf (stderr, "PipeWire I/O: Error parsing options.\n");
      return NULL;
    }

  params->io = io;
  params->device_no = device_count;
  params->sample_rate = sample_rate;
  params->open_channels = open_channels;

  if (debug)
    print_params (params);

  *uses_sample_clock = 0;

  if (*sample_format == BF_SAMPLE_FORMAT_AUTO)
    {
#ifdef __LITTLE_ENDIAN
      *sample_format = BF_SAMPLE_FORMAT_FLOAT_LE;
#elif __BIG_ENDIAN
      *sample_format = BF_SAMPLE_FORMAT_FLOAT_BE;
#endif
    }

  device[device_count] = *params;

  device_count++;

  return params;
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
    (*process_callback) (void **states[2], int state_count[2],
			 void **buffers[2], int frame_count, int event))
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::init, io: %d, params: %p, callback: %p\n",
	     io, params, callback_state);

  if (used_channels != open_channels)
    {
      fprintf (stderr, "JACK I/O: Open channels must be equal to used "
	       "channels for this I/O module.\n");
      return -1;
    }

  _bf_process_callback = process_callback;

  static void *_states[2][BF_MAXCHANNELS];

  for (int i = 0; i < used_channels; i++)
    {
      _states[io][i] = callback_state;
    }

  bf_states[io] = _states[io];

  filter_data.bf_callback.states[io] = callback_state;
  filter_data.bf_callback.state_count[io] = 1;

  *device_period_size = period_size;
  *isinterleaved = false;

  filter_data.port_count[io] = used_channels;
//  device[i].pipewire.n_channels[io] = used_channels;

  enum spa_audio_format audio_format = detect_pw_sample_format (sample_format);
  if (audio_format == SPA_AUDIO_FORMAT_UNKNOWN)
    {
      fprintf (stderr, "PipeWire I/O: Error detecting audio-format.\n");
      return -1;
    }

  return 0;
}

int
bfio_cb_init (void *params)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::bfio_cb_init.\n");

  return -1;
}

const char*
bfio_message (void)
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::message.\n");

  return "AHA";
}

int
bfio_synch_start ()
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::synch_start.\n");

  if (init_pipewire () < 0)
    {
      fprintf (stderr, "PipeWire I/O: can NOT init PipeWire.\n");
      return -1;
    }

  for (int i = 0; i < device_count; i++)
    {
      if (pw_thread_loop_start (device[i].pipewire.main_loop) < 0)
	{
	  fprintf (stderr, "PipeWire I/O: can run main-loop for device %d.\n",
		   i);
	  return -1;
	}
    }

  return 0;
}

void
bfio_synch_stop ()
{
  if (debug)
    fprintf (stderr, "PipeWire I/O::synch_stop.\n");

  for (int i = 0; i < device_count; i++)
    {
      pw_thread_loop_stop (device[i].pipewire.main_loop);
      pw_thread_loop_destroy (device[i].pipewire.main_loop);
    }
}
