/*
 * bfio_callback.c
 *
 *  Created on: Jan 29, 2024
 *      Author: d00d3
 */

#include <stdbool.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bfconf.h"

typedef struct {} params_t;

static int (*_bf_process_callback)(void **_states[2],
                         int state_count[2],
                         void **bufs[2],
                         int count,
                         int event);

void my_async_callback()
{
  _bf_process_callback(states, state_count, &bufs, count, BF_CALLBACK_EVENT_NORMAL);
}

int bfio_iscallback(void) {
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

  // allocate memory for params
  params_t *params = malloc (sizeof(params_t));
  memset (params, 0, sizeof(params_t));

  // @todo parse config-params

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
    (*process_callback) (void **callback_states[2], int callback_state_count[2],
			 void **buffers[2], int frame_count, int event))
{
  _bf_process_callback = process_callback;

}

int bfio_synch_start() {
  // @todo start async resources/threads
  return 0;
}

void bfio_synch_stop() {
  // @todo stop async resources/threads
}
