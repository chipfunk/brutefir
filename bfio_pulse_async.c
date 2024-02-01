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
#include "bfconf.h"
#include "bit.h"

#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/stream.h>
#include <pulse/sample.h>

#define GET_TOKEN(token, errstr)                                            \
    if (get_config_token(&lexval) != token) {                               \
        fprintf(stderr, "Pulse I/O: Parse error: " errstr);                 \
        return -1;                                                          \
    }

typedef struct bfio_pulse_settings
{
  // BruteFIR value
  int io;

  // PulseAUdio values
  char *server;         // Name of server to connect to, NULL for default
  char *app_name;       // The name of this application as shown in PA
  char *device;		// Device-name to connect stream to, or NULL for default
  char *stream_name;    // The stream-name as shown in PA

  pa_sample_spec sample_spec;
  pa_buffer_attr buffer_attr;
  pa_sample_format_t sample_format;
  pa_channel_map channel_map;
} bfio_pulse_settings_t;

bfio_pulse_settings_t my_params[2]; // Keep per in/out-stream, because ... fork()/threading?

typedef struct
{
  pa_threaded_mainloop *mainloop;
  pa_mainloop_api *api;
  pa_context *context;
  pa_stream *stream;
} pulseaudio_t;

static pulseaudio_t pulseaudio;

typedef int
(*bf_process_callback_t) (void **callback_states[2],
			  int callback_state_count[2], void **buffers[2],
			  int frame_count, int event);

static bf_process_callback_t _bf_process_callback;

bool_t debug = false;

/**
 * Check current module is compiled for BruteFIRs module-API
 */
static int
check_version (const int *version_major, const int *version_minor)
{
  if (*version_major != BF_VERSION_MAJOR)
    {
      fprintf (stderr, "Pulse I/O: major-version mismatch: %d, expected: %d.\n",
	       *version_major, BF_VERSION_MAJOR);
      return false;
    }

  if (*version_minor != BF_VERSION_MINOR)
    {
      fprintf (stderr, "Pulse I/O: minor-version mismatch: %d, expected: %d.\n",
	       *version_minor, BF_VERSION_MINOR);
      return false;
    }

  return true;
}

static int
parse_config_options_buffer_attr (pa_buffer_attr *into_buffer_attr, int
(*get_config_token) (union bflexval *lexval))
{
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
	      into_buffer_attr->maxlength = (int) lexval.real;
	    }
	  else if (strcmp (lexval.field, "tlength") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_REAL, "expected integer from -1 and up.\n");
	      into_buffer_attr->tlength = (int) lexval.real;
	    }
	  else if (strcmp (lexval.field, "prebuf") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_REAL, "expected integer from -1 and up.\n");
	      into_buffer_attr->prebuf = (int) lexval.real;
	    }
	  else if (strcmp (lexval.field, "minreq") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_REAL, "expected integer from -1 and up.\n");
	      into_buffer_attr->minreq = (int) lexval.real;
	    }
	  else if (strcmp (lexval.field, "fragsize") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_REAL, "expected integer from -1 and up.\n");
	      into_buffer_attr->fragsize = (int) lexval.real;
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
 * Import options from config-file
 */
static int
parse_config_options (bfio_pulse_settings_t *into_settings, int
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
	      into_settings->server = strdup (lexval.string);
	    }
	  else if (strcmp (lexval.field, "device") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
	      into_settings->device = strdup (lexval.string);
	    }
	  else if (strcmp (lexval.field, "app_name") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
	      into_settings->app_name = strdup (lexval.string);
	    }
	  else if (strcmp (lexval.field, "stream_name") == 0)
	    {
	      GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
	      into_settings->stream_name = strdup (lexval.string);
	    }
	  else if (strcmp (lexval.field, "buffer_attr") == 0)
	    {
	      parse_config_options_buffer_attr (&into_settings->buffer_attr,
						get_config_token);
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

  if (into_settings->app_name == NULL)
    into_settings->app_name = "BruteFIR";
  if (into_settings->stream_name == NULL)
    into_settings->stream_name = "BruteFIR stream";

  return 0;
}

/**
 * Detect appropriate sample-format for PA.
 *
 * @param bf_sample_format The sample-format requested by BruteFIR
 * @return PA sample-format to use, or PA_SAMPLE_INVALID if no sample-format could be found.
 */
static pa_sample_format_t
detect_pa_sample_format (const int bf_sample_format)
{
  switch (bf_sample_format)
    {
    case BF_SAMPLE_FORMAT_AUTO:
#ifdef LITTLE_ENDIAN
      return PA_SAMPLE_S32LE;
#elif BIG_ENDIAN
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

  *callback_sched_policy = SCHED_FIFO;
  callback_sched_param->sched_priority = 0;

  if (!check_version (version_major, version_minor))
    {
      fprintf (
	  stderr,
	  "Pulse I/O: Mismatching version-numbers. Expected BF_VERSION_MAJOR.BF_VERSION_MINOR, got %d.%d.\n",
	  *version_major, *version_minor);
      return NULL;
    }

  my_params[io].buffer_attr.maxlength = -1;
  my_params[io].buffer_attr.tlength = -1;
  my_params[io].buffer_attr.prebuf = -1;
  my_params[io].buffer_attr.minreq = -1;
  my_params[io].buffer_attr.fragsize = -1;

  my_params[io].io = io;

  if (parse_config_options (&my_params[io], get_config_token) < 0)
    {
      fprintf (stderr, "Pulse I/O: Error parsing options.\n");
      return NULL;
    }

  *uses_sample_clock = 0;

  return (void*) &my_params[io];
}

/**
 * Callback
 */
static void
_pa_stream_event_cb (pa_stream *p, const char *name, pa_proplist *pl,
		     void *userdata)
{
  if (debug)
    fprintf (stderr, "Pulse I/O: stream-event callback, proplist %s.\n",
	     pa_proplist_to_string (pl));

//  bfio_pulse_settings_t *settings = (bfio_pulse_settings_t*) userdata;

}

/**
 * Called when the state of a stream changes.
 */
static void
_pa_stream_state_cb (pa_stream *p, void *userdata)
{
  bfio_pulse_settings_t *settings = (bfio_pulse_settings_t*) userdata;

  pa_stream_state_t state = pa_stream_get_state (p);
  switch (state)
    {
    case PA_STREAM_UNCONNECTED: /**< The stream is not yet connected to any sink or source */
      if (debug)
	fprintf (stderr, "Pulse I/O::stream unconnected, state %d.\n", state);
      break;
    case PA_STREAM_CREATING: /**< The stream is being created */
      if (debug)
	fprintf (stderr, "Pulse I/O::stream creating, state %d.\n", state);
      break;
    case PA_STREAM_READY: /**< The stream is established, you may pass audio data to it now */
      if (debug)
	fprintf (stderr, "Pulse I/O::stream ready, state %d.\n", state);
      break;
    case PA_STREAM_FAILED: /**< An error occurred that made the stream invalid */
      if (debug)
	fprintf (stderr, "Pulse I/O::stream failed, state %d.\n", state);
      break;
    case PA_STREAM_TERMINATED: /**< The stream has been terminated cleanly */
      if (debug)
	fprintf (stderr, "Pulse I/O::stream terminated, state %d.\n", state);
      break;
    default:
      // should not happen, but who knows
      fprintf (stderr, "Pulse I/O::stream ERROR, unkown state %d.\n", state);
      break;
    }
}

static void
_pa_free_cb (void *p)
{
  if (debug)
    fprintf (stderr, "Pulse I/O::stream free.\n");

}

/**
 * Called when the stream can be written to.
 */
static void
_pa_stream_write_cb (pa_stream *p, size_t nbytes, void *userdata)
{
  bfio_pulse_settings_t *settings = (bfio_pulse_settings_t*) userdata;

  if (debug)
    fprintf (stderr, "Pulse I/O::stream write, nbytes: %d.\n", (int) nbytes);

  if (settings->io == BF_IN)
    {
      fprintf (stderr, "Pulse I/O::stream can NOT write on record-stream.\n");
      return;
    }

  void *buf = malloc (nbytes);
  if (pa_stream_begin_write (p, buf, &nbytes) < 0)
    {
      fprintf (stderr, "Pulse I/O: error writing stream, code %d.\n",
	       pa_context_errno (pa_stream_get_context (p)));
      return;
    }

  int64_t offset = 0;
  int err = pa_stream_write (p, buf, nbytes, _pa_free_cb, offset,
			     PA_SEEK_RELATIVE);
  if (err < 0)
    {
      fprintf (stderr, "Pulse I/O: error reading input-stream, code %d.\n",
	       pa_context_errno (pa_stream_get_context (p)));
      return;
    }

  pa_stream_drop (p);
}

/**
 * Called when data are available in the stream.
 */
static void
_pa_stream_read_cb (pa_stream *p, size_t nbytes, void *userdata)
{
  bfio_pulse_settings_t *settings = (bfio_pulse_settings_t*) userdata;

  if (debug)
    fprintf (stderr, "Pulse I/O::stream read, nbytes: %d.\n", (int) nbytes);

  if (settings->io == BF_OUT)
    {
      fprintf (stderr, "Pulse I/O::stream can NOT read on playback-stream.\n");
      return;
    }

  void *buf = malloc (nbytes);
  int err = pa_stream_peek (p, buf, &nbytes);
  if (err < 0)
    {
      fprintf (stderr, "Pulse I/O: error reading input-stream, code %d.\n",
	       pa_context_errno (pa_stream_get_context (p)));
      return;
    }

  pa_stream_drop (p);
}

/**
 * An overflow occured.
 */
static void
_pa_stream_overflow_cb (pa_stream *p, void *userdata)
{
  fprintf (stderr, "Pulse I/O: overflow CHECK.\n");

}

/**
 * An underflow occured.
 */
static void
_pa_stream_underflow_cb (pa_stream *p, void *userdata)
{
  fprintf (stderr, "Pulse I/O: underflow CHECK.\n");

}

/**
 * PA server updated latency of stream.
 */
static void
_pa_stream_latency_update_cb (pa_stream *p, void *userdata)
{
  fprintf (stderr, "Pulse I/O: latency CHECK.\n");

}

/**
 * PA server moved stream to another sink/source.
 */
static void
_pa_stream_moved_cb (pa_stream *p, void *userdata)
{
  fprintf (stderr, "Pulse I/O: moved CHECK.\n");

}

static void
_pa_stream_suspended_cb (pa_stream *p, void *userdata)
{
  fprintf (stderr, "Pulse I/O: suspended.\n");

}

/**
 * The buffer-attributes changed.
 */
static void
_pa_stream_buffer_attr_cb (pa_stream *p, void *userdata)
{
  if (debug)
    fprintf (stderr, "Pulse I/O: buffer-attr.\n");

}

/**
 * Open a stream for given context.
 */
static int
_pa_stream_open (bfio_pulse_settings_t *settings)
{
  const pa_stream_direction_t stream_direction =
      (settings->io == BF_OUT) ? PA_STREAM_PLAYBACK : PA_STREAM_RECORD;

  pa_proplist *my_stream_proplist = pa_proplist_new ();

  pulseaudio.stream = pa_stream_new_with_proplist (pulseaudio.context,
						   settings->stream_name,
						   &settings->sample_spec,
						   NULL,
						   my_stream_proplist);

  pa_stream_set_event_callback (pulseaudio.stream, _pa_stream_event_cb,
				settings);
  pa_stream_set_state_callback (pulseaudio.stream, _pa_stream_state_cb,
				settings);
  pa_stream_set_write_callback (pulseaudio.stream, _pa_stream_write_cb,
				settings);
  pa_stream_set_read_callback (pulseaudio.stream, _pa_stream_read_cb, settings);
  pa_stream_set_overflow_callback (pulseaudio.stream, _pa_stream_overflow_cb,
				   settings);
  pa_stream_set_underflow_callback (pulseaudio.stream, _pa_stream_underflow_cb,
				    settings);
  pa_stream_set_latency_update_callback (pulseaudio.stream,
					 _pa_stream_latency_update_cb,
					 settings);
  pa_stream_set_moved_callback (pulseaudio.stream, _pa_stream_moved_cb,
				settings);
  pa_stream_set_suspended_callback (pulseaudio.stream, _pa_stream_suspended_cb,
				    settings);
  pa_stream_set_buffer_attr_callback (pulseaudio.stream,
				      _pa_stream_buffer_attr_cb, settings);

  if (debug)
    fprintf (
	stderr,
	"Pulse I/O::buffer_attr: maxlength: %d, tlength: %d, prebuf: %d, minreq: %d, fragsize: %d\n",
	settings->buffer_attr.maxlength, settings->buffer_attr.tlength,
	settings->buffer_attr.prebuf, settings->buffer_attr.minreq,
	settings->buffer_attr.fragsize);

  pa_stream_flags_t my_stream_flags = PA_STREAM_START_UNMUTED;
  my_stream_flags |= PA_STREAM_ADJUST_LATENCY;

  if (stream_direction == PA_STREAM_RECORD)
    {
      if (pa_stream_connect_record (pulseaudio.stream, settings->device,
				    &settings->buffer_attr, my_stream_flags)
	  != 0)
	{
	  fprintf (stderr,
		   "Pulse I/O: error connecting recording-stream, code %d.\n",
		   pa_context_errno (pulseaudio.context));
	  return -1;
	}
    }
  else if (stream_direction == PA_STREAM_PLAYBACK)
    {
      pa_cvolume *volume = NULL;
      pa_stream *sync_stream = NULL;

      if (pa_stream_connect_playback (pulseaudio.stream, settings->device,
				      &settings->buffer_attr, my_stream_flags,
				      volume, sync_stream) != 0)
	{
	  fprintf (stderr,
		   "Pulse I/O: error connecting playback-stream, code %d.\n",
		   pa_context_errno (pulseaudio.context));
	  return -1;
	}
    }
  else
    {
      fprintf ( stderr,
	       "Pulse I/O: module could not determine stream-direction.\n");
      return -1;
    }

  return 0;
}

/**
 * Callback whenever pulseaudio-context/-connection changes state.
 */
static void
_pa_context_state_cb (pa_context *c, void *userdata)
{
  bfio_pulse_settings_t *settings = userdata;

  pa_context_state_t state = pa_context_get_state (c);
  switch (state)
    {
    case PA_CONTEXT_UNCONNECTED: /**< The context hasn't been connected yet */
      if (debug)
	fprintf (stderr, "Pulse I/O::context not connected, state %d.\n",
		 state);
      break;
    case PA_CONTEXT_CONNECTING: /**< A connection is being established */
      if (debug)
	fprintf (stderr, "Pulse I/O::context connecting, state %d.\n", state);
      break;
    case PA_CONTEXT_AUTHORIZING: /**< The client is authorizing itself to the daemon */
      if (debug)
	fprintf (stderr, "Pulse I/O::context authorizing, state %d.\n", state);
      break;
    case PA_CONTEXT_SETTING_NAME: /**< The client is passing its application name to the daemon */
      if (debug)
	fprintf (stderr, "Pulse I/O::context setting name, state %d.\n", state);
      break;
    case PA_CONTEXT_READY: /**< The connection is established, the context is ready to execute operations */
      if (debug)
	fprintf (stderr, "Pulse I/O::context ready, state %d.\n", state);
      _pa_stream_open (settings);
      break;
    case PA_CONTEXT_FAILED: /**< The connection failed or was disconnected */
      if (debug)
	fprintf (stderr, "Pulse I/O::context failed, state %d.\n", state);
      break;
    case PA_CONTEXT_TERMINATED: /**< The connection was terminated cleanly */
      if (debug)
	fprintf (stderr, "Pulse I/O::context terminated, state %d.\n", state);
      break;
    default:
      fprintf (stderr, "Pulse I/O::context UNKOWN state %d.\n", state);
      break;
    }
}

static void
_pa_context_event_cb (pa_context *c, const char *name, pa_proplist *p,
		      void *userdata)
{
  if (debug)
    fprintf (stderr, "Pulse I/O::context event callback, name: %s, state %s.\n",
	     name, pa_proplist_to_string (p));

//  bfio_pulse_settings_t *settings = (bfio_pulse_settings_t*) userdata;
}

/**
 * ???
 */
static void
_pa_context_subscribe_cb (pa_context *c, pa_subscription_event_type_t t,
			  uint32_t idx, void *userdata)
{
  if (debug)
    fprintf (stderr, "Pulse I/O::context subscribe, type: %d.\n", t);

//  bfio_pulse_settings_t *settings = (bfio_pulse_settings_t*) userdata;

  switch (t)
    {
    case PA_SUBSCRIPTION_EVENT_SINK:
      break;
    case PA_SUBSCRIPTION_EVENT_SOURCE:
      break;
    case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
      break;
    case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
      break;
    case PA_SUBSCRIPTION_EVENT_MODULE:
      break;
    case PA_SUBSCRIPTION_EVENT_CLIENT:
      break;
    case PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE:
      break;
    case PA_SUBSCRIPTION_EVENT_SERVER:
      break;
    case PA_SUBSCRIPTION_EVENT_CARD:
      break;
    case PA_SUBSCRIPTION_EVENT_FACILITY_MASK:
      break;
    case PA_SUBSCRIPTION_EVENT_AUTOLOAD:
      // obsolete
      break;
    case PA_SUBSCRIPTION_EVENT_CHANGE:
      break;
    case PA_SUBSCRIPTION_EVENT_REMOVE:
      break;
    case PA_SUBSCRIPTION_EVENT_TYPE_MASK:
      break;
    default:
      break;
    }
}

/**
 * Connect to PA-server.
 */
static int
_pa_context_connect (pa_mainloop_api *my_pa_api, char *server_name,
		     bfio_pulse_settings_t *settings)
{
  pa_proplist *my_pa_ctx_proplist = pa_proplist_new ();
  pulseaudio.context = pa_context_new_with_proplist (my_pa_api, "my context",
						     my_pa_ctx_proplist);

  pa_context_set_state_callback (pulseaudio.context, _pa_context_state_cb,
				 settings);
  pa_context_set_subscribe_callback (pulseaudio.context,
				     _pa_context_subscribe_cb, settings);
  pa_context_set_event_callback (pulseaudio.context, _pa_context_event_cb,
				 settings);

  pa_context_flags_t my_pa_context_flags = PA_CONTEXT_NOFLAGS;

  pa_spawn_api *my_ctx_spawn_api =
    { };

  if (pa_context_connect (pulseaudio.context, server_name, my_pa_context_flags,
			  my_ctx_spawn_api) < 0)
    {
      fprintf (stderr, "Pulse I/O: connection error, code %d.\n",
	       pa_context_errno (pulseaudio.context));
      return -1;
    }

  return 0;
}

int
bfio_iscallback (void)
{
  return true;
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
  _bf_process_callback = process_callback;

  *device_period_size = 4096;
  *isinterleaved = true;

  const pa_sample_format_t pa_sample_format = detect_pa_sample_format (sample_format);
  if (my_params[io].sample_format == PA_SAMPLE_INVALID)
    {
      fprintf (stderr,
	       "Pulse I/O: Could not find appropriate sample-format for PA.\n");
      return -1;
    }

  my_params[io].sample_spec.format = pa_sample_format;
  my_params[io].sample_spec.rate = sample_rate;
  my_params[io].sample_spec.channels = open_channels;

  /*
   * Set low-latency buffer-attribs if none configured
   *
   * https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/Developer/Clients/LatencyControl/
   * https://www.freedesktop.org/software/pulseaudio/doxygen/streams.html
   */
  uint32_t nbytes = period_size * pa_sample_size (&my_params[io].sample_spec);
  if (io == BF_IN)
    {
      my_params[io].buffer_attr.fragsize = nbytes;
    }
  else if (io == BF_OUT)
    {
      my_params[io].buffer_attr.tlength = nbytes;
    }
  else
    {
      fprintf (stderr, "Pulse I/O: Cannot determine stream-direction.\n");
      return -1;
    }

  return 0;
}

int
bfio_synch_start ()
{
  if (debug)
    fprintf (stderr, "Pulse I/O: start.\n");

  pulseaudio.mainloop = pa_threaded_mainloop_new ();
  pulseaudio.api = pa_threaded_mainloop_get_api (pulseaudio.mainloop);

  if(_pa_context_connect (pulseaudio.api, NULL, my_params) < 0)
    {
      fprintf(stderr, "Pulse I/O::could not connect context.");
      return -1;
    }

  pa_threaded_mainloop_start(pulseaudio.mainloop);

  return 0;
}

void
bfio_synch_stop ()
{
//  bfio_pulse_settings_t *settings = my_params[io];

  if (pa_stream_disconnect (pulseaudio.stream) != 0)
    {
      fprintf (stderr,
	       "Pulse I/O: error disconnecting playback-stream, code %d.\n",
	       pa_context_errno (pulseaudio.context));
    }

  if (pa_stream_disconnect (pulseaudio.stream) != 0)
    {
      fprintf (stderr,
	       "Pulse I/O: error disconnecting recording-stream, code %d.\n",
	       pa_context_errno (pulseaudio.context));
    }

  pa_threaded_mainloop_stop(pulseaudio.mainloop);

  pa_threaded_mainloop_free (pulseaudio.mainloop);
}
