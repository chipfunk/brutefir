/*
 * bfio_pulse.c
 *
 */
#include <string.h>

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bit.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

static bool_t debug = false;

struct settings {
    char *device;
};

static pa_simple *pa_input = NULL;
static pa_simple *pa_output = NULL;

int bfio_iscallback(void)
{

	return 0;
}

void *
bfio_preinit(int *version_major,
             int *version_minor,
             int (*get_config_token)(union bflexval *lexval),
             int io,
             int *sample_format,
             int sample_rate,
             int open_channels,
             int *uses_sample_clock,
             int *callback_sched_policy,
             struct sched_param *callback_sched_param,
             int _debug)
{
    struct settings *settings;
    union bflexval lexval;
    int n, token, ver;

    ver = *version_major;
    *version_major = BF_VERSION_MAJOR;
    *version_minor = BF_VERSION_MINOR;
    if (ver != BF_VERSION_MAJOR) {
        return NULL;
    }
    debug = !!_debug;

    settings = malloc(sizeof(struct settings));
    memset(settings, 0, sizeof(struct settings));

    return settings;
}

int bfio_init(void *params, int io, int sample_format, int sample_rate,
		int open_channels, int used_channels, const int channel_selection[],
		int period_size, int *device_period_size, int *isinterleaved,
		void *callback_state,
		int (*process_callback)(void **callback_states[2],
				int callback_state_count[2], void **buffers[2], int frame_count,
				int event))
{
	const char *pa_server = NULL;
	const char *pa_app_name = "BruteFIR";

	const char *pa_device = NULL;
	const char *pa_stream_name = "Input for BruteFIR";
	const pa_sample_spec pa_sample_spec =
	{
	PA_SAMPLE_S16LE, sample_rate, 2 };
	const pa_channel_map *pa_channel_map = NULL;
	const pa_buffer_attr *pa_buffer_attr = NULL;
	int pa_error = 0;

	if (io == BF_IN)
	{
		pa_input = pa_simple_new(pa_server, pa_app_name,
		PA_STREAM_PLAYBACK, pa_device, pa_stream_name, &pa_sample_spec,
				pa_channel_map, pa_buffer_attr, &pa_error);

	}
	else if (io == BF_OUT)
	{
		pa_output = pa_simple_new(pa_server, pa_app_name,
		PA_STREAM_RECORD, pa_device, pa_stream_name, &pa_sample_spec,
				pa_channel_map, pa_buffer_attr, &pa_error);

	}

	return 0;
}

int bfio_read(int fd, void *buf, int offset, int count)
{
	count = pa_simple_read(pa_input, buf, count, NULL);
	return count;
}

int bfio_write(int fd, const void *buf, int offset, int count)
{
	count = pa_simple_write(pa_output, buf, count, NULL);
	return count;
}

int bfio_synch_start(void)
{
	return 0;
}

void bfio_synch_stop(void)
{

}

int bfio_start(int io)
{

	return 0;
}

void bfio_stop(int io)
{
	if (io == BF_IN)
	{
		pa_simple_free(pa_input);
	}
	else if (io == BF_OUT)
	{
		pa_simple_free(pa_output);
	}
}

const char*
bfio_message(void)
{
	return "A message.";
}
