/**
 * Example to show how to implement a BFIO-module using synchronous
 * read()/write() calls.
 *
 */

#include <stdbool.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bfconf.h"

typedef struct
{
} params_t;

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
  return fd;
}

int
bfio_read (int fd, void *into_buf, int offset, int count)
{
//  memcpy (into_buf, in, count);

  return count;
}

int
bfio_write (int fd, const void *from_buf, int offset, int count)
{
//  memcpy (out, from_buf, count);

  return count;
}

/**
 * Will be called synchronously by BruteFIR
 */
int
bfio_start (int io)
{
  if (debug)
    fprintf (stderr, "BFIO::start, %d, %p\n", io, my_params[io]);

  return 0;
}

/**
 * Will be called synchronously by BruteFIR
 */
void
bfio_stop (int io)
{
  if (debug)
    fprintf (stderr, "BFIO::stop, %d, %p\n", io, my_params[io]);
}
