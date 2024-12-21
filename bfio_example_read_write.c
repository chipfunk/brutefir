/**
 * Example to show how to implement a BFIO-module using synchronous
 * read()/write() calls.
 *
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IS_BFIO_MODULE
#include "bfmod.h"

struct params_t {
  int fd;
};

static int debug = 0;

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

/*
 * Prepare device initialization
 */
void *bfio_preinit(int *version_major, int *version_minor,
                   int (*get_config_token)(union bflexval *lexval), int io,
                   int *sample_format, int sample_rate, int open_channels,
                   int *uses_sample_clock, int *callback_sched_policy,
                   struct sched_param *callback_sched, int _debug) {
  debug = _debug;

  // allocate memory for params
  struct params_t *settings = malloc(sizeof(struct params_t));
  if (settings == NULL) {
    fprintf(stderr, "Could not alocate memory for params.");
    return NULL;
  }
  memset(settings, 0, sizeof(struct params_t));

  // @todo parse config-params

  return settings;
}

/*
 * Device-initialization
 */
int bfio_init(void *params, int io, int sample_format, int sample_rate,
              int open_channels, int used_channels,
              const int channel_selection[], int period_size,
              int *device_period_size, int *isinterleaved, void *callback_state,
              int (*process_callback)(void **callback_states[2],
                                      int callback_state_count[2],
                                      void **buffers[2], int frame_count,
                                      int event)) {
  struct params_t *settings = params;

  // Create pipe to "trap" BruteFIR into availability of data
  int fd = create_dummypipe(io);
  settings->fd = fd;

  return fd;
}

/*
 * Start devices
 */
int bfio_start(int io) {
  if (debug)
    fprintf(stderr, "BFIO::start, %d\n", io);

  return 0;
}

/*
 * Stop devices
 */
void bfio_stop(int io) {
  if (debug)
    fprintf(stderr, "BFIO::stop, %d\n", io);
}

/*
 * Read audio-data from device
 */
int bfio_read(int fd, void *into_buf, int offset, int count) {
  //  memcpy (into_buf, in, count);

  return count;
}

/*
 * Write filtered audio-data
 */
int bfio_write(int fd, const void *from_buf, int offset, int count) {
  //  memcpy (out, from_buf, count);

  return count;
}
