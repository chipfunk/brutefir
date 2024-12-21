/**
 * Example to show how to implement a BFIO-module using asynchronous
 * callbacks.
 *
 */

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define IS_BFIO_MODULE
#include "bfmod.h"

struct params_t {
    int fd;
};

static int debug = 0;

static int (*bf_process_callback)(void **_states[2], int state_count[2],
                                  void **bufs[2], int count, int event);

void *bf_callback_state;

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

static void *audio_api_get_buffer(const int io, const int channel) {
  return NULL;
}

void my_async_callback() {
  // void *inbuf[BF_MAXCHANNELS], *outbuf[BF_MAXCHANNELS];

  // void **bufs[2];
  // bufs[BF_IN] = inbuf;
  // bufs[BF_OUT] = outbuf;

  // for (int channel = 0; channel < open_channels; channel++) {
  //   bufs[BF_IN][channel] = audio_api_get_buffer(BF_IN, channel);
  //   bufs[BF_OUT][channel] = audio_api_get_buffer(BF_OUT, channel);
  // }

  // void *in_state[BF_MAXCHANNELS], *out_state[BF_MAXCHANNELS];

  // void **states[2];
  // states[BF_IN] = in_state;
  // states[BF_OUT] = out_state;

  // states[BF_IN][0] = bf_callback_state;
  // states[BF_OUT][0] = bf_callback_state;

  // int state_count[2] = {1, 1};

  // frame_count = n_bytes / sizeof(sample_format);

  // int frames = bf_process_callback(states, state_count, bufs, frame_count,
  //                                  BF_CALLBACK_EVENT_NORMAL);
}

int bfio_iscallback(void) { return true; }

void *bfio_preinit(int *version_major, int *version_minor,
                   int (*get_config_token)(union bflexval *lexval), int io,
                   int *sample_format, int sample_rate, int open_channels,
                   int *uses_sample_clock, int *callback_sched_policy,
                   struct sched_param *callback_sched, int _debug) {
  // Assuming debug should be a local variable and not used outside
  int debug = _debug;

  // allocate memory for params
  struct params_t *params = malloc(sizeof(struct params_t));
  if (!params) {
    return NULL; // Handle allocation failure
  }

  memset(params, 0, sizeof(struct params_t));

  // @todo parse config-params

  return params;
}

int bfio_init(void *params, int io, int sample_format, int sample_rate,
              int open_channels, int used_channels,
              const int channel_selection[], int period_size,
              int *device_period_size, int *isinterleaved, void *callback_state,
              int (*process_callback)(void **callback_states[2],
                                      int callback_state_count[2],
                                      void **buffers[2], int frame_count,
                                      int event)) {
  struct params_t *settings = params;

  *device_period_size = period_size;

  bf_process_callback = process_callback;

  bf_callback_state = callback_state;

  // Create pipe to "trap" BruteFIR into availability of data
  int fd = create_dummypipe(io);
  settings->fd = fd;

  return fd;
}

static int audio_api_start() { return 0; }

int bfio_synch_start() {
  // @todo start async resources/threads
  audio_api_start();
  return 0;
}

static int audio_api_stop() { return 0; }

void bfio_synch_stop() {
  // @todo stop async resources/threads
  audio_api_stop();
}
