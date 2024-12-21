/*
 * (c) Copyright 2024 -- chipfunk
 *
 * pipewire:: module.
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/format.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>

#include <pipewire/core.h>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <pipewire/port.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>

#define IS_BFIO_MODULE
#include "bfmod.h"

int debug = 0;

#define GET_TOKEN(token, errstr)                                               \
  if (get_config_token(&lexval) != token) {                                    \
    fprintf(stderr, "pipewire::: Parse error: " errstr);                       \
    return -1;                                                                 \
  }

static int (*_bf_process_callback)(void **states[2], int state_count[2],
                                   void **bufs[2], int frame_count, int event);

struct pipewire_t {
  struct pw_thread_loop *loop;
  struct spa_hook *thread_loop_listener;

  struct pw_context *context;
  struct spa_hook *context_listener;

  struct pw_core *core;
  struct spa_hook *core_listener;

  struct pw_registry *registry;
  struct spa_hook *registry_listener;

  struct pw_filter *filter;
  struct spa_hook *filter_listener;

  struct port *ports[BF_MAXCHANNELS];
  void *port_data[BF_MAXCHANNELS];
  struct spa_hook *port_listener[BF_MAXCHANNELS];
};

typedef void *port_data_t;

struct params_t {
  int io;
  int dummypipe_fd;

  int open_channels;
  int sample_rate;
  int sample_format;

  char *server;
  char *device_name;
  char *app_name;
  char *stream_name;

  struct pipewire_t pipewire;
};

static struct params_t *devices[2 * BF_MAXFILTERS] = {};

static int device_count = 0;

/**
 * Create a pipe to trap BruteFIR into thinking there is data-available.
 *
 * Code shamelessly copied from `bfio_file.c`.
 *
 * Assumption: For PulseAudio there is always data available or ready
 * to write. If not, the blocking characteristics of pipewire-roundtrip takes
 * care of that.
 *
 * @param io determines the direction of created file-descriptor.
 * @return a file-descriptor to the pipes read OR write end. Returns -1 in case
 * of error.
 */
static int create_dummypipe(const int io) {
  int dummypipe[2];
  if (pipe(dummypipe) == -1) {
    fprintf(stderr, "pipewire::create_dummypipe: Could not create pipe.\n");
    return -1;
  }

  if (io == BF_IN) {
    close(dummypipe[1]); // close unused write-end
    return dummypipe[0];
  } else if (io == BF_OUT) {
    close(dummypipe[0]); // Close unused read-end
    return dummypipe[1];
  }

  fprintf(stderr, "pipewire::create_dummypipe: Invalid IO direction.\n");
  return -1;
}

static int check_version(const int *version_major, const int *version_minor) {
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
 * @return PipeWire sample-format to use, or SPA_AUDIO_FORMAT_UNKNOWN if no
 * sample-format could be found.
 */
static enum spa_audio_format
detect_pw_sample_format(const int bf_sample_format) {
  switch (bf_sample_format) {
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
parse_config_options(struct params_t *settings,
                     int (*get_config_token)(union bflexval *lexval)) {
  union bflexval lexval;
  int token;
  while ((token = get_config_token(&lexval)) > 0) {
    if (token == BF_LEXVAL_FIELD) {
      if (strcmp(lexval.field, "server") == 0) {
        GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
        settings->server = strdup(lexval.string);
      } else if (strcmp(lexval.field, "device") == 0) {
        GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
        settings->device_name = strdup(lexval.string);
      } else if (strcmp(lexval.field, "app_name") == 0) {
        GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
        settings->app_name = strdup(lexval.string);
      } else if (strcmp(lexval.field, "stream_name") == 0) {
        GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
        settings->stream_name = strdup(lexval.string);
      } else {
        fprintf(stderr, "Pulse I/O: Parse error: unknown field.\n");
        return -1;
      }
      GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
    } else {
      fprintf(stderr, "Pulse I/O: Parse error: expected field.\n");
      return -1;
    }
  }

  if (settings->app_name == NULL)
    settings->app_name = "BruteFIR";
  if (settings->stream_name == NULL)
    settings->stream_name = "BruteFIR stream";

  return 0;
}

int bfio_iscallback(void) {
  if (debug)
    fprintf(stderr, "pipewire::bfio_iscallback\n");

  return true;
}

void *bfio_preinit(int *version_major, int *version_minor,
                   int (*get_config_token)(union bflexval *lexval), int io,
                   int *sample_format, int sample_rate, int open_channels,
                   int *uses_sample_clock, int *callback_sched_policy,
                   struct sched_param *callback_sched, int _debug) {
  debug = 1;

  if (debug)
    fprintf(stderr, "pipewire::bfio_preinit\n");

  if (!check_version(version_major, version_minor)) {
    fprintf(stderr,
            "pipewire::bfio_preinit: Mismatching version-numbers. Expected "
            "BF_VERSION_MAJOR.BF_VERSION_MINOR, got %d.%d.\n",
            *version_major, *version_minor);
    return NULL;
  }

  //  memset (callback_sched, 0, sizeof(*callback_sched));
  // *callback_sched_policy = SCHED_FIFO;
  // callback_sched->sched_priority = 3;

  *sample_format = BF_SAMPLE_FORMAT_FLOAT_LE;

  if (debug)
    fprintf(stderr, "pipewire::preinit, device: %d, io: %d\n", device_count,
            io);

  struct params_t *settings = malloc(sizeof(struct params_t));
  if (settings == NULL) {
    fprintf(stderr, "pipewire::preinit, could not alloc memory for params");
    return NULL;
  }

  memset(settings, 0, sizeof(struct params_t));

  if (parse_config_options(settings, get_config_token) < 0) {
    fprintf(stderr, "pipewire::: Error parsing options.\n");
    return NULL;
  }

  *uses_sample_clock = 0;

  if (*sample_format == BF_SAMPLE_FORMAT_AUTO) {
#ifdef __LITTLE_ENDIAN
    *sample_format = BF_SAMPLE_FORMAT_FLOAT_LE;
#elif __BIG_ENDIAN
    *sample_format = BF_SAMPLE_FORMAT_FLOAT_BE;
#endif
  }

  settings->io = io;

  devices[device_count] = settings;
  device_count++;

  return (void *)settings;
}

int bfio_init(void *params, int io, int sample_format, int sample_rate,
              int open_channels, int used_channels,
              const int channel_selection[], int period_size,
              int *device_period_size, int *isinterleaved, void *callback_state,
              int (*process_callback)(void **states[2], int state_count[2],
                                      void **buffers[2], int frame_count,
                                      int event)) {
  if (debug)
    fprintf(stderr, "pipewire::bfio_init\n");

  struct params_t *settings = params;

  if (debug)
    fprintf(stderr, "pipewire::bfio_init, io: %d, params: %p, callback: %p\n",
            io, params, callback_state);

  if (used_channels != open_channels) {
    fprintf(stderr, "pipewire::bfio_init: Open channels must be equal to used "
                    "channels for this I/O module.\n");
    return -1;
  }

  if (sample_format != BF_SAMPLE_FORMAT_FLOAT_LE) {
    fprintf(stderr, "pipewire::bfio_init: sample format must be "
                    "BF_SAMPLE_FORMAT_FLOAT_LE.\n");
    return -1;
  }

  *device_period_size = period_size;
  *isinterleaved = false;

  settings->io = io;
  settings->open_channels = open_channels;
  settings->sample_rate = sample_rate;
  // sample_format;

  // settings->sample_format = detect_pw_sample_format(sample_format);
  // if (settings->sample_format == SPA_AUDIO_FORMAT_UNKNOWN) {
  //   fprintf(stderr, "pipewire::bfio_init: Error detecting audio-format.\n");
  //   return -1;
  // }

  settings->dummypipe_fd = create_dummypipe(io);
  if (settings->dummypipe_fd < 0) {
    fprintf(stderr, "pipewire::bfio_init: Error creating dummy-pipe\n");
    return -1;
  }

  return settings->dummypipe_fd;
}

static void _pw_filter_on_process(void *userdata,
                                  struct spa_io_position *position) {
  if (debug)
    fprintf(stderr, "pipewire::_pw_filter_on_process.\n");

  struct params_t *settings = userdata;

  float *in, *out;
  uint32_t n_samples = position->clock.duration;

  pw_log_trace("do process %d", n_samples);

  in = pw_filter_get_dsp_buffer(settings->pipewire.ports[0], n_samples);
  // out = pw_filter_get_dsp_buffer(data->out_port, n_samples);

  // if (in == NULL || out == NULL)
  //   return;

  // memcpy(out, in, n_samples * sizeof(float));
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = _pw_filter_on_process,
};

int bfio_synch_start() {
  if (debug)
    fprintf(stderr, "pipewire::bfio_synch_start.\n");

  pw_init(NULL, NULL);

  for (unsigned int i = 0; i < device_count; i++) {
    fprintf(stderr, "pipewire::bfio_synch_start %d.\n", i);
    struct params_t *settings = devices[i];

    settings->pipewire.loop = pw_thread_loop_new("loop", NULL);
    if (settings->pipewire.loop == NULL) {
      fprintf(stderr, "pipewire::bfio_start: can NOT init PipeWire.\n");
      return -1;
    }

    settings->pipewire.filter = pw_filter_new_simple(
        pw_thread_loop_get_loop(settings->pipewire.loop), "audio-filter",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                          "Filter", PW_KEY_MEDIA_ROLE, "DSP", NULL),
        &filter_events, settings);

    for (int channel = 0; channel < settings->open_channels; channel++) {
      /* make an audio DSP port */
      settings->pipewire.ports[channel] = pw_filter_add_port(
          settings->pipewire.filter,
          settings->io == BF_IN ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
          PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(struct params_t),
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                            PW_KEY_PORT_NAME, settings->io == BF_IN ? "input" : "output", NULL),
          NULL, 0);
    }

    if (pw_filter_connect(settings->pipewire.filter, PW_FILTER_FLAG_RT_PROCESS,
                          NULL, 0) < 0) {
      fprintf(stderr, "can't connect\n");
      return -1;
    }
  }

  for (unsigned int i = 0; i < device_count; i++) {
    struct params_t *settings = devices[i];
    pw_thread_loop_start(settings->pipewire.loop);
  }

  return 0;
}

void bfio_synch_stop() {
  if (debug)
    fprintf(stderr, "pipewire::bfio_synch_stop.\n");

  for (unsigned int i = 0; i < device_count; i++) {
    struct params_t *settings = devices[i];
    pw_thread_loop_stop(settings->pipewire.loop);
    pw_thread_loop_destroy(settings->pipewire.loop);
  }

  pw_deinit();
}
