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

#define GET_TOKEN(token, errstr)                                               \
    if (get_config_token(&lexval) != token) {                                  \
        fprintf(stderr, "Pulse I/O: Parse error: " errstr);                     \
        return NULL;                                                           \
    }

struct settings
{
  // BruteFIR value
  int io;
  int sample_rate;
  int open_channels;

  // Dummy-pipe value
  int dummypipe_fd;               // File-descriptor for dummy-pipe.

  // PulseAUdio values
  char *app_name;         // The name of this application as shown in PA
  char *server;                // Name of server to connect to, NULL for default
  char *stream_name;      // The stream-name as shown in PA
  char *device;                // Device-name to connect to, or NULL for default
  pa_sample_format_t sample_format;
  pa_buffer_attr buffer_attr;
  pa_channel_map channel_map;
};

typedef struct settings pulse_settings_t;

static pulse_settings_t *my_params[2]; // Keep per in/out-stream, because ... fork()/threading?

static pa_mainloop *my_pa_mainloop = NULL;	// primary pulse-API access
static pa_context *my_pa_ctx = NULL;
static pa_stream *my_pa_stream;		// keep playback-stream

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
    close (dummypipe[1]);   // close unused write-end
    my_params[io]->dummypipe_fd = dummypipe[0];
  }
  else if (io == BF_OUT)
  {
    close (dummypipe[0]);   // Close unused read-end
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
  my_params[io] = malloc (sizeof(pulse_settings_t));
  memset (my_params[io], 0, sizeof(pulse_settings_t));

  my_params[io]->io = io;
  my_params[io]->sample_rate = sample_rate;
  my_params[io]->open_channels = open_channels;
}

static void*
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
        return NULL;
      }
      GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
    }
    else
    {
      fprintf (stderr, "Pulse I/O: Parse error: expected field.\n");
      return NULL;
    }
  }

  if (my_params[io]->app_name == NULL) my_params[io]->app_name = "BruteFIR";
  if (my_params[io]->stream_name == NULL) my_params[io]->stream_name =
      "BruteFIR stream";

  return my_params[io];
}

/**
 * Detect appropriate sample-format for PA.
 *
 * @param bf_sample_format The sample-format requested by BruteFIR
 * @return PA sample-format to use, or PA_SAMPLE_INVALID if no sample-format could be found.
 */
static const pa_sample_format_t
detect_pa_sample_format (const int *bf_sample_format)
{
  switch (*bf_sample_format)
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
  if (!check_version (version_major, version_minor))
  {
    fprintf (
        stderr,
        "Pulse I/O: Mismatching version-numbers. Expected BF_VERSION_MAJOR.BF_VERSION_MINOR, got %d.%d.\n",
        *version_major, *version_minor);
    return NULL;
  }

  init_settings (io, sample_rate, open_channels);

  if (!parse_config_options (io, get_config_token))
  {
    fprintf (stderr, "Pulse I/O: Error parsing options.\n");
    return NULL;
  }

  pa_sample_format_t pa_sample_format = detect_pa_sample_format (sample_format);
  if (pa_sample_format == PA_SAMPLE_INVALID)
  {
    fprintf (stderr,
             "Pulse I/O: Could not find appropriate sample-format for PA.\n");
    return NULL;
  }

  my_params[io]->sample_format = pa_sample_format;

  *uses_sample_clock = 0;

  return (void*) my_params[io];
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
  pa_stream_state_t state = pa_stream_get_state (my_pa_stream);
  fprintf (stderr, "Pulse I/O stream-event callback, state %d.\n", state);
}

static int
_pa_open_stream (const char *device, const char *stream_name,
                 const pa_stream_direction_t stream_direction,
                 const pa_sample_spec *sample_spec,
                 const pa_channel_map *channel_map,
                 const pa_buffer_attr *buffer_attr)
{
  pa_proplist *my_stream_proplist = pa_proplist_new ();
  pa_stream_flags_t my_stream_flags = PA_STREAM_NOFLAGS;

  my_pa_stream = pa_stream_new_with_proplist (my_pa_ctx, stream_name,
                                              sample_spec, channel_map,
                                              my_stream_proplist);

  if (stream_direction == PA_STREAM_RECORD)
  {
    if (pa_stream_connect_record (my_pa_stream, device, buffer_attr,
                                  my_stream_flags) != 0)
    {
      fprintf (stderr,
               "Pulse I/O error connecting recording-stream, code %d.\n",
               pa_context_errno (my_pa_ctx));
      return -1;
    }
  }
  else if (stream_direction == PA_STREAM_PLAYBACK)
  {
    pa_cvolume *my_pa_volume = NULL;
    pa_stream *my_pa_sync_stream = NULL;

    if (pa_stream_connect_playback (my_pa_stream, device, buffer_attr,
                                    my_stream_flags, my_pa_volume,
                                    my_pa_sync_stream) != 0)
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

  pa_stream_set_event_callback (my_pa_stream, _pa_stream_event_cb, NULL);

  return 0;
}

/**
 * Callback whenever pulseaudio-context/-connection changes state.
 */
static void
_pa_state_cb (pa_context *c, void *userdata)
{
  pulse_settings_t *settings = (pulse_settings_t*) userdata;

  pa_context_state_t state = pa_context_get_state (my_pa_ctx);
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

      const pa_stream_direction_t stream_direction =
          (settings->io == BF_IN) ? PA_STREAM_RECORD : PA_STREAM_PLAYBACK;

      pa_sample_spec sample_spec = { .format = settings->sample_format,
                .rate = settings->sample_rate, .channels = settings->open_channels, };

      _pa_open_stream (settings->device, settings->stream_name,
                       stream_direction, &sample_spec, &settings->channel_map,
                       &settings->buffer_attr);

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
  fprintf (stderr, "Pulse I/O CB context_subscribe.\n");
}

static int
_pa_open_connection (pa_mainloop_api *my_pa_api, const char *pa_server,
                     const char *pa_app_name, void *userdata)
{
  pa_proplist *my_pa_ctx_proplist = pa_proplist_new ();
  my_pa_ctx = pa_context_new_with_proplist (my_pa_api, "my context",
                                            my_pa_ctx_proplist);

  pa_context_set_state_callback (my_pa_ctx, _pa_state_cb, userdata);
  pa_context_set_subscribe_callback (my_pa_ctx, _pa_context_subscribe_cb,
                                     userdata);
  pa_context_set_event_callback (my_pa_ctx, _pa_context_event_cb, userdata);

  pa_context_flags_t my_pa_context_flags = PA_CONTEXT_NOFLAGS;

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

static pa_buffer_attr
init_buffer_attr (const int io, const int period_size)
{
  pa_buffer_attr buffer_attr = { .maxlength = period_size };

  if (io == BF_IN)
  {
    // BF_IN is a PA recording-stream
    buffer_attr.fragsize = period_size;
  }

  if (io == BF_OUT)
  {
    // BF_IN is a PA recording-stream
    buffer_attr.tlength = -1;
    buffer_attr.minreq = period_size;
    buffer_attr.prebuf = -1;
  }

  return buffer_attr;
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
  fprintf(stderr, "Params: %d.\n", params);

  my_params[io]->buffer_attr =  init_buffer_attr(io, period_size);

  my_pa_mainloop = pa_mainloop_new ();
  pa_mainloop_api *my_pa_api = pa_mainloop_get_api (my_pa_mainloop);

  _pa_open_connection (my_pa_api, my_params[io]->server,
                       my_params[io]->app_name, &my_params[io]);

  *device_period_size = 4096;
  *isinterleaved = true;

  return create_dummypipe (io);
}

int
bfio_start (int io)
{
//  pa_stream_direction_t stream_direction;
//  if (io == BF_IN)
//  {
//    stream_direction = PA_STREAM_RECORD;
//  }
//  else if (io == BF_OUT)
//  {
//    stream_direction = PA_STREAM_PLAYBACK;
//  }
//  else
//  {
//    fprintf (stderr,
//             "Pulse I/O module could not determine stream-direction.\n");
//    return -1;
//  }

  return pa_mainloop_run (my_pa_mainloop, 0);
}

void
bfio_stop (int io)
{
  close (my_params[io]->dummypipe_fd);
  my_params[io]->dummypipe_fd = -1;

  if (pa_stream_disconnect (my_pa_stream) != 0)
  {
    fprintf (stderr,
             "Pulse I/O error disconnecting playback-stream, code %d.\n",
             pa_context_errno (my_pa_ctx));
  }

  if (pa_stream_disconnect (my_pa_stream) != 0)
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
  if (pa_stream_peek (my_pa_stream, buf, &nbytes) < 0)
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
  if (pa_stream_write (my_pa_stream, buf, count, NULL, 0, PA_SEEK_RELATIVE)
      != 0)
  {
    fprintf (stderr, "Pulse I/O error connecting playback-stream, code %d.\n",
             pa_context_errno (my_pa_ctx));
    return -1;
  }

  return count;
}
