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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#define IS_BFIO_MODULE
#include "bfmod.h"

#define GET_TOKEN(token, errstr)                                               \
  if (get_config_token(&lexval) != token) {                                    \
    fprintf(stderr, "Pulse I/O: Parse error: " errstr);                        \
    return -1;                                                                 \
  }

struct settings_t {
  // Dummy-pipe value
  int dummypipe_fd; // File-descriptor for dummy-pipe.

  // PulseAUdio values
  char *app_name;    // The name of this application as shown in PA
  char *server;      // Name of server to connect to, NULL for default
  char *stream_name; // The stream-name as shown in PA
  char *device;      // Device-name to connect to, or NULL for default
  struct pa_sample_spec sample_spec;
  struct pa_buffer_attr
      *buffer_attr; // MUST be pointer to allow for optional configuration
  pa_simple *pa_handle;
};

static struct settings_t *my_params[2][BF_MAXFILTERS];

static int debug = true;

unsigned int bf_device_count = 0;
unsigned int bf_device_channels_in = 0;
unsigned int bf_device_out_channels = 0;

struct device_count_t {
  unsigned int in;
  unsigned int out;
};

struct device_count_t device_count = {0, 0};

struct channel_count_t {
  unsigned int in;
  unsigned int out;
} bf_channel_count = {0, 0};

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
 * @return a file-descriptor to the pipes read OR write end. Returns -1 in case
 * of error.
 */
static int create_dummypipe(const int io) {
  int dummypipe[2];
  if (pipe(dummypipe) == -1) {
    fprintf(stderr, "Pulse I/O: Could not create pipe.\n");
    return -1;
  }

  if (io == BF_IN) {
    close(dummypipe[1]); // close unused write-end
    return dummypipe[0];
  } else if (io == BF_OUT) {
    close(dummypipe[0]); // Close unused read-end
    return dummypipe[1];
  } else {
    fprintf(stderr, "Pulse I/O: Invalid IO direction.\n");
    return -1;
  }
}

static int check_version(const int *version_major, const int *version_minor) {
  if (*version_major != BF_VERSION_MAJOR) {
    return false;
  }

  if (*version_minor != BF_VERSION_MINOR) {
    return false;
  }

  return true;
}

static int parse_config_options_buffer_attr(
    pa_buffer_attr *buffer_attr,
    int (*get_config_token)(union bflexval *lexval)) {
  buffer_attr->maxlength = -1;
  buffer_attr->tlength = -1;
  buffer_attr->prebuf = -1;
  buffer_attr->minreq = -1;
  buffer_attr->fragsize = -1;

  union bflexval lexval;
  int token;

  if (BF_LEX_LBRACE != get_config_token(&lexval)) {
    fprintf(stderr, "Pulse I/O: Parse error: expected {.\n");
    return -1;
  }

  while ((token = get_config_token(&lexval)) > 0) {
    if (token == BF_LEX_RBRACE) {
      return 0;
    } else if (token == BF_LEXVAL_FIELD) {
      if (strcmp(lexval.field, "maxlength") == 0) {
        GET_TOKEN(BF_LEXVAL_REAL,
                  "\"maxlength\" expects integer from -1 and up.\n");
        buffer_attr->maxlength = (int)lexval.real;
      } else if (strcmp(lexval.field, "tlength") == 0) {
        GET_TOKEN(BF_LEXVAL_REAL, "expected integer from -1 and up.\n");
        buffer_attr->tlength = (int)lexval.real;
      } else if (strcmp(lexval.field, "prebuf") == 0) {
        GET_TOKEN(BF_LEXVAL_REAL, "expected integer from -1 and up.\n");
        buffer_attr->prebuf = (int)lexval.real;
      } else if (strcmp(lexval.field, "minreq") == 0) {
        GET_TOKEN(BF_LEXVAL_REAL, "expected integer from -1 and up.\n");
        buffer_attr->minreq = (int)lexval.real;
      } else if (strcmp(lexval.field, "fragsize") == 0) {
        GET_TOKEN(BF_LEXVAL_REAL, "expected integer from -1 and up.\n");
        buffer_attr->fragsize = (int)lexval.real;
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

  if (debug)
    fprintf(stderr,
            "Pulse I/O: configure buffer attributes, maxlength: %d, "
            "tlength: %d, prebuf: %d, minreq: %d, fragsize: %d\n",
            buffer_attr->maxlength, buffer_attr->tlength, buffer_attr->prebuf,
            buffer_attr->minreq, buffer_attr->fragsize);

  return 0;
}

/**
 * Read settings from config-file.
 *
 * Returns 0 on success, negative values on failure.
 */
static int
parse_config_options(struct settings_t *settings,
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
        settings->device = strdup(lexval.string);
      } else if (strcmp(lexval.field, "buffer_attr") == 0) {
        settings->buffer_attr = malloc(sizeof(pa_buffer_attr));
        memset(settings->buffer_attr, 0, sizeof(pa_buffer_attr));

        parse_config_options_buffer_attr(settings->buffer_attr,
                                         get_config_token);
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

  if (settings->app_name == NULL) {
    fprintf(stderr, "Pulse I/O: Parse error: application-name expected.\n");
    return -1;
  }
  if (settings->stream_name == NULL) {
    fprintf(stderr, "Pulse I/O: Parse error: stream-name expected.\n");
    return -1;
  }

  return 0;
}

/**
 * Detect appropriate sample-format for PA.
 *
 * @param bf_sample_format The sample-format requested by BruteFIR
 * @return PA sample-format to use, or PA_SAMPLE_INVALID if no sample-format
 * could be found.
 */
static const pa_sample_format_t
detect_pa_sample_format(const int bf_sample_format) {
  switch (bf_sample_format) {
  case BF_SAMPLE_FORMAT_AUTO:
#ifdef LITTLE_ENDIAN
    return PA_SAMPLE_S32LE;
#endif
#ifdef BIG_ENDIAN
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

static pa_simple *_pa_simple_open(const char *server, const char *app_name,
                                  const char *device, const char *stream_name,
                                  const pa_stream_direction_t stream_direction,
                                  const pa_sample_spec *sample_spec,
                                  const pa_channel_map *channel_map,
                                  const pa_buffer_attr *buffer_attr) {
  if (debug)
    fprintf(stderr, "Pulse I/O: %s, %s, %s, %s\n", server, app_name, device,
            stream_name);

  if (debug)
    fprintf(stderr,
            "Pulse I/O: buffer attributes, maxlength: %d, tlength: %d, prebuf: "
            "%d, minreq: %d, fragsize: %d\n",
            buffer_attr->maxlength, buffer_attr->tlength, buffer_attr->prebuf,
            buffer_attr->minreq, buffer_attr->fragsize);

  int errno = 0;
  pa_simple *handle =
      pa_simple_new(NULL, app_name, stream_direction, NULL, stream_name,
                    sample_spec, channel_map, buffer_attr, &errno);

  if (handle == NULL) {
    fprintf(stderr,
            "Pulse I/O could not open connection/stream, code %d - %s.\n",
            errno, pa_strerror(errno));
    return NULL;
  }

  return handle;
}

void *bfio_preinit(int *version_major, int *version_minor,
                   int (*get_config_token)(union bflexval *lexval), int io,
                   int *sample_format, int sample_rate, int open_channels,
                   int *uses_sample_clock, int *callback_sched_policy,
                   struct sched_param *callback_sched_param, int _debug) {
  if (!check_version(version_major, version_minor)) {
    fprintf(stderr,
            "Pulse I/O: Mismatching version-numbers. Expected "
            "BF_VERSION_MAJOR.BF_VERSION_MINOR, got %d.%d.\n",
            *version_major, *version_minor);
    return NULL;
  }

  debug = _debug;

  printf("pulse::preinit: %d, %d, %d\n", io, sample_rate, open_channels);

  bf_device_count++;

  unsigned int device = 0;
  if (io == BF_IN) {
    device = device_count.in++;
    bf_channel_count.in += open_channels;
  } else if (io == BF_OUT) {
    device = device_count.out++;
    bf_channel_count.out += open_channels;
  } else {
    fprintf(stderr, "Pulse I/O: Unknown I/O direction, %d.\n", io);
    return NULL;
  }

  struct settings_t *settings = malloc(sizeof(struct settings_t));
  memset((void *)settings, 0, sizeof(struct settings_t));

  if (parse_config_options(settings, get_config_token) < 0) {
    fprintf(stderr, "Pulse I/O: Error parsing options.\n");
    return NULL;
  }

  *uses_sample_clock = 0;

  my_params[io][device] = settings;
  return (void *)settings;
}

int bfio_init(void *params, int io, int sample_format, int sample_rate,
              int open_channels, int used_channels,
              const int channel_selection[], int period_size,
              int *device_period_size, int *isinterleaved, void *callback_state,
              int (*process_callback)(void **callback_states[2],
                                      int callback_state_count[2],
                                      void **buffers[2], int frame_count,
                                      int event)) {
  printf("pulse::init: %d, %d, %d, %d, %d, %d\n", io, sample_rate,
         open_channels, period_size, bf_device_channels_in,
         bf_device_out_channels);

  struct settings_t *settings = params;

  settings->dummypipe_fd = create_dummypipe(io);

  *device_period_size = period_size;
  *isinterleaved = true;

  pa_sample_format_t pa_sample_format = detect_pa_sample_format(sample_format);
  if (pa_sample_format == PA_SAMPLE_INVALID) {
    fprintf(stderr,
            "Pulse I/O: Could not find appropriate sample-format for PA.\n");
    return -1;
  }

  settings->sample_spec.format = pa_sample_format;
  settings->sample_spec.rate = sample_rate;
  settings->sample_spec.channels = open_channels;

  // Set low-latency buffer-attribs if none configured
  if (settings->buffer_attr == NULL) {
    settings->buffer_attr = malloc(sizeof(pa_buffer_attr));
    memset(settings->buffer_attr, 0, sizeof(pa_buffer_attr));

    settings->buffer_attr->maxlength = -1;
    settings->buffer_attr->tlength = -1;
    settings->buffer_attr->prebuf = -1;
    settings->buffer_attr->minreq = -1;
    settings->buffer_attr->fragsize = -1;

    uint32_t nbytes = period_size * pa_sample_size(&settings->sample_spec);
    if (io == BF_IN) {
      settings->buffer_attr->fragsize = nbytes;
    } else if (io == BF_OUT) {
      settings->buffer_attr->tlength = nbytes;
    } else {
      fprintf(stderr, "Pulse I/O: Cannot determine stream-direction.\n");
      return -1;
    }
  }

  return settings->dummypipe_fd;
}

/**
 * Initializing PA-connection here to avoid fork()-ing after bfio_init().
 */
int bfio_start(const int io) {
  printf("pulse:bfio_start %d\n", io);

  pa_stream_direction_t stream_direction;
  if (io == BF_IN) {
    stream_direction = PA_STREAM_RECORD;
  } else if (io == BF_OUT) {
    stream_direction = PA_STREAM_PLAYBACK;
  } else {
    fprintf(stderr, "Pulse I/O module could not determine stream-direction.\n");
    return -1;
  }

  unsigned int devices = 0;
  if (io == BF_IN) {
    devices = device_count.in;
  } else if (io == BF_OUT) {
    devices = device_count.out;
  } else {
    fprintf(stderr, "Pulse I/O module could not determine stream-direction.\n");
    return -1;
  }

  for (unsigned int device = 0; device < devices; device++) {
    struct settings_t *settings = my_params[io][device];

    pa_simple *pa_handle =
        _pa_simple_open(settings->server, settings->app_name, settings->device,
                        settings->stream_name, stream_direction,
                        &(settings->sample_spec), NULL, settings->buffer_attr);

    if (pa_handle == NULL) {
      return -1;
    }

    settings->pa_handle = pa_handle;
    fprintf(stderr, "Pulse I/O opened handle %d\n", pa_handle);
  }

  return 0;
}

void bfio_stop(const int io) {

  unsigned int devices = 0;
  if (io == BF_IN) {
    devices = device_count.in;
  } else if (io == BF_OUT) {
    devices = device_count.out;
  } else {
    fprintf(stderr, "Pulse I/O module could not determine stream-direction.\n");
  }

  for (unsigned int device = 0; device < devices; device++) {
    struct settings_t *settings = my_params[io][device];

    close(settings->dummypipe_fd);
    settings->dummypipe_fd = -1;

    pa_simple_free(settings->pa_handle);
    settings->pa_handle = NULL;

    free(settings);
  }
}

int bfio_read(const int fd, void *buf, const int offset, const int count) {
  if (debug)
    printf("pulse::bfio_read %d, %d, %d\n", fd, offset, count);

  for (unsigned int device = 0; device < device_count.in; device++) {
    struct settings_t *settings = my_params[BF_IN][device];

    // skip different fd
    if (settings->dummypipe_fd != fd)
      continue;

    // skip non-initialzed input
    if (settings->pa_handle == NULL)
      return 0;

    int errno = 0;
    if ((pa_simple_read(settings->pa_handle, buf, count, &errno)) < 0) {
      fprintf(stderr,
              "Pulse I/O module failed to read, message: %d, %d - %s.\n",
              device, errno, pa_strerror(errno));
      return 0;
    }
  }

  return count;
}

int bfio_write(const int fd, const void *buf, const int offset,
               const int count) {
  if (debug)
    printf("pulse::bfio_write %d, %d, %d\n", fd, offset, count);

  for (unsigned int device = 0; device < device_count.out; device++) {
    struct settings_t *settings = my_params[BF_OUT][device];

    // skip different fd
    if (settings->dummypipe_fd != fd)
      continue;

    // skip non-initialzed output
    if (settings->pa_handle == NULL)
      return 0;

    int errno = 0;
    if ((pa_simple_write(settings->pa_handle, buf, count, &errno)) < 0) {
      fprintf(stderr,
              "Pulse I/O module failed to write, message: %d, %d - %s.\n",
              device, errno, pa_strerror(errno));
      return 0;
    }
  }

  return count;
}
