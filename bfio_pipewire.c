/*
 * (c) Copyright 2024 -- chipfunk
 *
 * PipeWire I/O module.
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pipewire-0.3/pipewire/pipewire.h>
#include <pipewire-0.3/pipewire/stream.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bfconf.h"

#define GET_TOKEN(token, errstr)                                   \
    if (get_config_token(&lexval) != token) {                      \
        fprintf(stderr, "Pulse I/O: Parse error: " errstr);        \
        return -1;                                                 \
    }

typedef struct settings {
  struct pw_main_loop *pw_main_loop;
  struct pw_context *pw_context;
  struct pw_core *pw_core;
} settings_t;

void*
bfio_preinit (int *version_major, int *version_minor, int
(*get_config_token) (union bflexval *lexval),
	      int io, int *sample_format, int sample_rate, int open_channels,
	      int *uses_sample_clock, int *callback_sched_policy,
	      struct sched_param *callback_sched, int _debug)
{

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
  const struct spa_dict *props = NULL;

  struct pw_loop *pw_main_loop = pw_loop_new(props);

  struct pw_context *pw_context = pw_context_new(struct pw_loop *main_loop, struct pw_properties *props, size_t user_data_size);

  struct pw_properties *props = pw_properties_new();

  struct pw_core *pw_core = pw_context_connect();

  struct pw_stream*
  pw_stream_new (struct pw_core *core, const char *name,
		 struct pw_properties *props);
}

int
bfio_start (int io)
{
  return 0;
}

void
bfio_stop (int io)
{
}

int
bfio_read (int fd, void *buf, int offset, int count)
{
  return 0;

}

int
bfio_write (int fd, const void *buf, int offset, int count)
{
  return 0;

}

