/**
 * Example to show how to implement a BFIO-module using asynchronous
 * callbacks.
 *
 */

#include <stdbool.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bfconf.h"

typedef struct
{
} params_t;

static int
(*bf_process_callback) (void **_states[2], int state_count[2], void **bufs[2],
			int count, int event);

void *bf_callback_state;

void
my_async_callback ()
{
  void *inbuf[BF_MAXCHANNELS], *outbuf[BF_MAXCHANNELS];

  void **bufs[2];
  bufs[BF_IN] = inbuf;
  bufs[BF_OUT] = outbuf;

  for (int channel = 0; channel < open_channels; channel++)
    {
      bufs[BF_IN][channel] = audio_api_get_buffer (INPUT, channel);
      bufs[BF_OUT][channel] = audio_api_get_buffer (OUTPUT, channel);
    }

  void *in_state[BF_MAXCHANNELS], *out_state[BF_MAXCHANNELS];

  void **states[2];
  states[BF_IN] = in_state;
  states[BF_OUT] = out_state;

  states[BF_IN][0] = bf_callback_state;
  states[BF_OUT][0] = bf_callback_state;

  int state_count[2] =
    { 1, 1 };

  frame_count = n_bytes / sizeof(sample_format);

  int frames = bf_process_callback (states, state_count, bufs, frame_count,
				    BF_CALLBACK_EVENT_NORMAL);
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

  // allocate memory for params
  params_t *params = malloc (sizeof(params_t));
  memset (params, 0, sizeof(params_t));

  // @todo parse config-params

  return &params;
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
  params_t *settings = params;

  bf_process_callback = process_callback;

  bf_callback_state = callback_state;
}

int
bfio_synch_start ()
{
  // @todo start async resources/threads
  audio_api_start();
  return 0;
}

void
bfio_synch_stop ()
{
  // @todo stop async resources/threads
  audio_api_stop();
  audio_api_free();
}
