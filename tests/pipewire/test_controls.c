#include <stdio.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <unistd.h>
#include <math.h>

#define SAMPLE_RATE 48000
#define CHANNELS 2

static struct pw_main_loop *main_loop = NULL;
static struct pw_stream *stream = NULL;
static struct spa_hook stream_listener = {0};
static float current_volume = 1.0f;
static bool is_muted = false;
static float phase = 0.0f;

static void on_process(void *userdata __attribute__((unused))) {
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
            
        // Apply volume and mute
        if (is_muted)
            value = 0.0f;
        else
            value *= current_volume;
            
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
    fprintf(stderr, "Stream state changed: %d -> %d (error: %s)\n", old, state, error ? error : "none");
    
    if (state == PW_STREAM_STATE_ERROR) {
        pw_main_loop_quit(main_loop);
    }
    
    if (state == PW_STREAM_STATE_STREAMING) {
        fprintf(stderr, "Stream is now streaming\n");
    }
}

static void on_stream_param_changed(void *data __attribute__((unused)), 
                                  uint32_t id,
                                  const struct spa_pod *param __attribute__((unused))) {
    fprintf(stderr, "Stream param changed: %u\n", id);
}

static void on_stream_control_info(void *data __attribute__((unused)),
                                 uint32_t id,
                                 const struct pw_stream_control *control) {
    if (id == SPA_PROP_volume) {
        fprintf(stderr, "Volume changed to: %f\n", control->values[0]);
        current_volume = control->values[0];
    } else if (id == SPA_PROP_mute) {
        bool mute = control->values[0] > 0.0f;
        fprintf(stderr, "Mute changed to: %s\n", mute ? "true" : "false");
        is_muted = mute;
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .control_info = on_stream_control_info,
    .process = on_process,
};

static bool set_volume(float volume) {
    if (volume < 0.0f || volume > 1.0f) {
        fprintf(stderr, "Volume must be between 0.0 and 1.0\n");
        return false;
    }

    float values[1] = {volume};
    int res = pw_stream_set_control(stream, SPA_PROP_volume, 1, values);
    if (res < 0) {
        fprintf(stderr, "Failed to set volume: %s\n", spa_strerror(res));
        return false;
    }
    return true;
}

static bool set_mute(bool mute) {
    float values[1] = {mute ? 1.0f : 0.0f};
    int res = pw_stream_set_control(stream, SPA_PROP_mute, 1, values);
    if (res < 0) {
        fprintf(stderr, "Failed to set mute: %s\n", spa_strerror(res));
        return false;
    }
    return true;
}

int main(int argc, char *argv[]) {
    pw_init(&argc, &argv);
    
    // Create main loop
    main_loop = pw_main_loop_new(NULL);
    if (!main_loop) {
        fprintf(stderr, "Failed to create main loop\n");
        return 1;
    }
    
    struct pw_loop *loop = pw_main_loop_get_loop(main_loop);
    struct pw_context *context = pw_context_new(loop, NULL, 0);
    if (!context) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    struct pw_core *core = pw_context_connect(context, NULL, 0);
    if (!core) {
        fprintf(stderr, "Failed to connect to PipeWire\n");
        return 1;
    }
    
    // Create stream properties
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_APP_NAME, "test_controls",
        PW_KEY_NODE_NAME, "test_control_source",
        PW_KEY_NODE_DESCRIPTION, "Test Control Source",
        NULL
    );
    
    // Create stream
    stream = pw_stream_new(core, "test_stream", props);
    if (!stream) {
        fprintf(stderr, "Failed to create stream\n");
        return 1;
    }
    
    // Add stream listener
    pw_stream_add_listener(stream, &stream_listener, &stream_events, NULL);
    
    // Create format
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
    int res = pw_stream_connect(stream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    
    if (res < 0) {
        fprintf(stderr, "Failed to connect stream: %s\n", spa_strerror(res));
        return 1;
    }
    
    printf("Running control tests...\n");
    
    // Wait for stream to be ready
    while (pw_stream_get_state(stream, NULL) != PW_STREAM_STATE_STREAMING) {
        pw_loop_iterate(loop, 0);
    }
    
    // Test volume control
    printf("\nTesting volume control...\n");
    float test_volumes[] = {0.0f, 0.5f, 1.0f};
    for (size_t i = 0; i < sizeof(test_volumes)/sizeof(test_volumes[0]); i++) {
        printf("Setting volume to %f\n", test_volumes[i]);
        if (!set_volume(test_volumes[i])) {
            fprintf(stderr, "Failed to set volume\n");
            return 1;
        }
        
        // Keep processing audio while waiting for change
        for (int j = 0; j < 50; j++) {
            pw_loop_iterate(loop, 0);
            usleep(10000); // 10ms sleep
        }
    }
    
    // Test mute control
    printf("\nTesting mute control...\n");
    printf("Muting stream\n");
    if (!set_mute(true)) {
        fprintf(stderr, "Failed to mute stream\n");
        return 1;
    }
    
    // Keep processing audio while waiting for change
    for (int j = 0; j < 50; j++) {
        pw_loop_iterate(loop, 0);
        usleep(10000); // 10ms sleep
    }
    
    printf("Unmuting stream\n");
    if (!set_mute(false)) {
        fprintf(stderr, "Failed to unmute stream\n");
        return 1;
    }
    
    // Keep processing audio while waiting for change
    for (int j = 0; j < 50; j++) {
        pw_loop_iterate(loop, 0);
        usleep(10000); // 10ms sleep
    }
    
    // Final state check
    printf("\nFinal state:\n");
    printf("Volume: %f\n", current_volume);
    printf("Muted: %s\n", is_muted ? "yes" : "no");
    
    // Cleanup
    if (stream) {
        pw_stream_destroy(stream);
    }
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(main_loop);
    pw_deinit();
    
    return 0;
} 
