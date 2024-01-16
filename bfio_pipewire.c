/*
 * (c) Copyright 2024 -- chipfunk
 *
 * PipeWire I/O module.
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/support/plugin.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bfconf.h"

#define GET_TOKEN(token, errstr)                                   \
    if (get_config_token(&lexval) != token) {                      \
        fprintf(stderr, "Pulse I/O: Parse error: " errstr);        \
        return -1;                                                 \
    }

typedef struct
{
  struct pw_loop *pw_main_loop;
  struct pw_context *pw_context;
  struct pw_core *pw_core;
  struct pw_stream *pw_stream;
} settings_t;

settings_t *my_params;

bool debug = false;

void*
bfio_preinit (int *version_major, int *version_minor, int
(*get_config_token) (union bflexval *lexval),
	      int io, int *sample_format, int sample_rate, int open_channels,
	      int *uses_sample_clock, int *callback_sched_policy,
	      struct sched_param *callback_sched, int _debug)
{
  debug = _debug;

  if (debug)
    fprintf (stderr, "Pipewire I/O::preinit, %d\n", io);

  pw_init (NULL, NULL);

  settings_t *settings = malloc (sizeof(settings_t));
  memset (settings, 0, sizeof(settings_t));

  return settings;
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
  if (debug)
    fprintf (stderr, "Pipewire I/O::init, %d, %p\n", io, params);

  my_params = params;

  const struct spa_dict *loop_props =
    { 0, };

  my_params->pw_main_loop = pw_loop_new (loop_props);

  struct pw_properties *context_props = NULL;

  size_t user_data_size = 0;

  my_params->pw_context = pw_context_new (my_params->pw_main_loop, context_props,
						  user_data_size);

  struct pw_properties *core_props = NULL;

  my_params->pw_core = pw_context_connect (my_params->pw_context, core_props,
					     user_data_size);
  if (my_params->pw_core == NULL)
    {
      fprintf (stderr, "Pipewire I/O::init can NOT connect context\n");
      return -1;
    }

  struct pw_properties *stream_props = NULL;

  const char *stream_name = "Steam name";

  my_params->pw_stream = pw_stream_new (my_params->pw_core, stream_name, stream_props);

  return 0;
}

int
bfio_start (int io)
{
  if (debug)
    fprintf (stderr, "Pipewire I/O::start, %d, %p\n", io, my_params);

  return 0;
}

void
bfio_stop (int io)
{
  if (debug)
    fprintf (stderr, "Pipewire I/O::stop, %d, %p\n", io, my_params);

}

int
bfio_read (int fd, void *buf, int offset, int count)
{
  if (debug)
    fprintf (stderr, "Pipewire I/O::read, %d\n", fd);

  return 0;

}

int
bfio_write (int fd, const void *buf, int offset, int count)
{
  if (debug)
    fprintf (stderr, "Pipewire I/O::write, %d\n", fd);

  return 0;

}

