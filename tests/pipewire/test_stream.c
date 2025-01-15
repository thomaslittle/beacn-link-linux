#include <stdio.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>

#define TEST_DURATION_SEC 5
#define STREAM_READY_TIMEOUT_SEC 3
#define SAMPLE_RATE 48000
#define CHANNELS 2

static struct pw_main_loop *main_loop = NULL;
static struct pw_stream *stream = NULL;
static struct spa_hook stream_listener = {0};
static enum pw_stream_state stream_state = PW_STREAM_STATE_UNCONNECTED;
static bool test_running = true;
static bool stream_stable = false;
static time_t connecting_start = 0;
static float phase = 0.0f;

static const char* get_state_name(enum pw_stream_state state) {
    switch (state) {
        case PW_STREAM_STATE_ERROR: return "ERROR";
        case PW_STREAM_STATE_UNCONNECTED: return "UNCONNECTED";
        case PW_STREAM_STATE_CONNECTING: return "CONNECTING";
        case PW_STREAM_STATE_PAUSED: return "PAUSED";
        case PW_STREAM_STATE_STREAMING: return "STREAMING";
        default: return "UNKNOWN";
    }
}

static void on_process(void *userdata) {
    struct pw_buffer *b;
    struct spa_buffer *buf;
    float *dst;
    uint32_t n_frames;
    
    if ((b = pw_stream_dequeue_buffer(stream)) == NULL) {
        fprintf(stderr, "out of buffers\n");
        return;
    }

    buf = b->buffer;
    dst = buf->datas[0].data;
    if (dst == NULL)
        return;
    
    n_frames = buf->datas[0].maxsize / sizeof(float) / CHANNELS;
    
    // Generate a simple sine wave
    for (uint32_t i = 0; i < n_frames; i++) {
        float value = 0.3f * sinf(2.0f * M_PI * 440.0f * phase);
        phase += 1.0f / SAMPLE_RATE;
        if (phase >= 1.0f)
            phase -= 1.0f;
            
        // Write to all channels
        for (int c = 0; c < CHANNELS; c++)
            dst[i * CHANNELS + c] = value;
    }
    
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float) * CHANNELS;
    buf->datas[0].chunk->size = n_frames * sizeof(float) * CHANNELS;

    pw_stream_queue_buffer(stream, b);
}

static void on_stream_state_changed(void *data __attribute__((unused)), 
                                  enum pw_stream_state old,
                                  enum pw_stream_state state, 
                                  const char *error) {
    fprintf(stderr, "Stream state changed: %s -> %s (error: %s)\n", 
            get_state_name(old), get_state_name(state), error ? error : "none");
    
    stream_state = state;
    
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "Stream error occurred\n");
        test_running = false;
    } else if (state == PW_STREAM_STATE_STREAMING) {
        fprintf(stderr, "Stream is now streaming\n");
        stream_stable = true;
    } else if (state == PW_STREAM_STATE_CONNECTING) {
        connecting_start = time(NULL);
        fprintf(stderr, "Stream is connecting...\n");
    } else if (state == PW_STREAM_STATE_PAUSED) {
        fprintf(stderr, "Stream is paused, starting it...\n");
        pw_stream_set_active(stream, true);
    }
}

static void on_stream_param_changed(void *data __attribute__((unused)), 
                                  uint32_t id,
                                  const struct spa_pod *param) {
    const char* param_name;
    switch (id) {
        case SPA_PARAM_Format: param_name = "Format"; break;
        case SPA_PARAM_Props: param_name = "Props"; break;
        case SPA_PARAM_EnumFormat: param_name = "EnumFormat"; break;
        default: param_name = "Unknown"; break;
    }
    fprintf(stderr, "Stream param changed: %s (id: %u)\n", param_name, id);
    
    if (id == SPA_PARAM_Format && param) {
        struct spa_audio_info_raw info;
        if (spa_format_audio_raw_parse(param, &info) >= 0) {
            fprintf(stderr, "Got audio format:\n");
            fprintf(stderr, "  format: %d (expected: %d)\n", info.format, SPA_AUDIO_FORMAT_F32);
            fprintf(stderr, "  rate: %d (expected: %d)\n", info.rate, SAMPLE_RATE);
            fprintf(stderr, "  channels: %d (expected: %d)\n", info.channels, CHANNELS);
        }
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_process,
};

int main(int argc, char *argv[]) {
    pw_init(&argc, &argv);
    
    fprintf(stderr, "Creating main loop...\n");
    main_loop = pw_main_loop_new(NULL);
    if (!main_loop) {
        fprintf(stderr, "Failed to create main loop\n");
        return 1;
    }
    
    struct pw_loop *loop = pw_main_loop_get_loop(main_loop);
    fprintf(stderr, "Creating context...\n");
    struct pw_context *context = pw_context_new(loop, NULL, 0);
    if (!context) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    fprintf(stderr, "Connecting to PipeWire...\n");
    struct pw_core *core = pw_context_connect(context, NULL, 0);
    if (!core) {
        fprintf(stderr, "Failed to connect to PipeWire\n");
        return 1;
    }
    fprintf(stderr, "Successfully connected to PipeWire\n");
    
    // Create stream properties
    fprintf(stderr, "Creating stream properties...\n");
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_APP_NAME, "test_stream",
        PW_KEY_NODE_NAME, "test_virtual_source",
        PW_KEY_NODE_DESCRIPTION, "Test Virtual Source",
        NULL
    );
    
    // Create stream
    fprintf(stderr, "Creating stream...\n");
    stream = pw_stream_new(core, "test_stream", props);
    if (!stream) {
        fprintf(stderr, "Failed to create stream\n");
        return 1;
    }
    
    // Add stream listener
    fprintf(stderr, "Adding stream listener...\n");
    pw_stream_add_listener(stream, &stream_listener, &stream_events, NULL);
    
    // Create format
    fprintf(stderr, "Creating stream format...\n");
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b,
        SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32,
            .channels = CHANNELS,
            .rate = SAMPLE_RATE
        ));
    
    // Connect stream
    fprintf(stderr, "Connecting stream...\n");
    int res = pw_stream_connect(stream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    
    if (res < 0) {
        fprintf(stderr, "Failed to connect stream: %s\n", spa_strerror(res));
        return 1;
    }
    
    fprintf(stderr, "Running main loop...\n");
    
    // Run until stream is ready or timeout
    time_t start = time(NULL);
    while (test_running && !stream_stable && (time(NULL) - start) < TEST_DURATION_SEC) {
        pw_loop_iterate(loop, 0);
        usleep(10000); // 10ms sleep
        
        time_t now = time(NULL);
        
        // Print progress every second
        if (now > start && (now - start) % 1 == 0) {
            fprintf(stderr, "Waiting for stream... %ld seconds elapsed (current state: %s)\n", 
                    now - start, get_state_name(stream_state));
        }
        
        // Check if we've been in connecting state too long
        if (stream_state == PW_STREAM_STATE_CONNECTING && 
            connecting_start > 0 && 
            (now - connecting_start) >= STREAM_READY_TIMEOUT_SEC) {
            fprintf(stderr, "Stream has been in connecting state for too long (%d seconds)\n", 
                    STREAM_READY_TIMEOUT_SEC);
            test_running = false;
            break;
        }
    }
    
    if (!stream_stable) {
        fprintf(stderr, "Stream failed to become ready within %d seconds (final state: %s)\n", 
                TEST_DURATION_SEC, get_state_name(stream_state));
        return 1;
    }
    
    fprintf(stderr, "Stream is ready and stable\n");
    printf("Stream test completed successfully\n");
    
    // Cleanup
    fprintf(stderr, "Cleaning up...\n");
    if (stream) {
        pw_stream_destroy(stream);
    }
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(main_loop);
    pw_deinit();
    
    return 0;
} 
