#include <stdio.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <math.h>

#define MAX_STREAMS 4
#define TEST_DURATION_SEC 10
#define SAMPLE_RATE 48000
#define CHANNELS 2

static struct pw_main_loop *main_loop = NULL;
static struct pw_context *context = NULL;
static struct pw_stream *streams[MAX_STREAMS] = {NULL};
static struct spa_hook stream_listeners[MAX_STREAMS] = {0};
static float stream_volumes[MAX_STREAMS] = {1.0f};
static bool stream_muted[MAX_STREAMS] = {false};
static int active_streams = 0;
static bool test_running = true;
static float phases[MAX_STREAMS] = {0.0f};
static float base_frequencies[MAX_STREAMS] = {440.0f, 523.25f, 659.25f, 783.99f}; // A4, C5, E5, G5

static void on_process(void *data) {
    int stream_id = (int)(intptr_t)data;
    struct pw_stream *stream = streams[stream_id];
    struct pw_buffer *b;
    struct spa_buffer *buf;
    float *dst;
    uint32_t n_frames;
    
    if ((b = pw_stream_dequeue_buffer(stream)) == NULL) {
        fprintf(stderr, "out of buffers for stream %d\n", stream_id);
        return;
    }

    buf = b->buffer;
    dst = buf->datas[0].data;
    if (dst == NULL)
        return;
    
    n_frames = buf->datas[0].maxsize / sizeof(float) / CHANNELS;
    
    // Generate a simple sine wave at a unique frequency for each stream
    float freq = base_frequencies[stream_id];
    float volume = stream_muted[stream_id] ? 0.0f : stream_volumes[stream_id];
    
    for (uint32_t i = 0; i < n_frames; i++) {
        float value = 0.3f * sinf(2.0f * M_PI * freq * phases[stream_id]);
        phases[stream_id] += 1.0f / SAMPLE_RATE;
        if (phases[stream_id] >= 1.0f)
            phases[stream_id] -= 1.0f;
            
        // Apply volume and mute
        value *= volume;
            
        // Write to all channels
        for (int c = 0; c < CHANNELS; c++)
            dst[i * CHANNELS + c] = value;
    }
    
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float) * CHANNELS;
    buf->datas[0].chunk->size = n_frames * sizeof(float) * CHANNELS;

    pw_stream_queue_buffer(stream, b);
}

static void on_stream_state_changed(void *data, 
                                  enum pw_stream_state old,
                                  enum pw_stream_state state, 
                                  const char *error) {
    int stream_id = (int)(intptr_t)data;
    fprintf(stderr, "Stream %d state changed: %d -> %d (error: %s)\n", 
            stream_id, old, state, error ? error : "none");
    
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "Stream %d error, attempting recovery...\n", stream_id);
        // Attempt recovery by reconnecting the stream
        pw_stream_disconnect(streams[stream_id]);
        // Note: In a real application, you would implement proper recovery here
    } else if (state == PW_STREAM_STATE_PAUSED) {
        fprintf(stderr, "Stream %d paused, starting it...\n", stream_id);
        pw_stream_set_active(streams[stream_id], true);
    }
}

static void on_stream_control_info(void *data,
                                 uint32_t id,
                                 const struct pw_stream_control *control) {
    int stream_id = (int)(intptr_t)data;
    if (id == SPA_PROP_volume) {
        stream_volumes[stream_id] = control->values[0];
        fprintf(stderr, "Stream %d volume changed to: %f\n", stream_id, control->values[0]);
    } else if (id == SPA_PROP_mute) {
        stream_muted[stream_id] = control->values[0] > 0.0f;
        fprintf(stderr, "Stream %d mute changed to: %s\n", stream_id, 
                stream_muted[stream_id] ? "true" : "false");
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .control_info = on_stream_control_info,
    .process = on_process,
};

static void cleanup_streams(void) {
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i]) {
            pw_stream_destroy(streams[i]);
            streams[i] = NULL;
        }
    }
}

static bool create_stream(int index) {
    char name[32], desc[64];
    snprintf(name, sizeof(name), "test_stream_%d", index);
    snprintf(desc, sizeof(desc), "Test Stream %d", index);
    
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_APP_NAME, "test_multi_stream",
        PW_KEY_NODE_NAME, name,
        PW_KEY_NODE_DESCRIPTION, desc,
        NULL
    );
    
    struct pw_core *core = pw_context_connect(context, NULL, 0);
    if (!core) {
        fprintf(stderr, "Failed to connect to PipeWire for stream %d\n", index);
        return false;
    }
    
    streams[index] = pw_stream_new(core, name, props);
    if (!streams[index]) {
        fprintf(stderr, "Failed to create stream %d\n", index);
        pw_core_disconnect(core);
        return false;
    }
    
    pw_stream_add_listener(streams[index], 
                          &stream_listeners[index],
                          &stream_events,
                          (void*)(intptr_t)index);
    
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
    
    int res = pw_stream_connect(streams[index],
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | 
                              PW_STREAM_FLAG_MAP_BUFFERS | 
                              PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    
    if (res < 0) {
        fprintf(stderr, "Failed to connect stream %d: %s\n", 
                index, spa_strerror(res));
        pw_stream_destroy(streams[index]);
        streams[index] = NULL;
        return false;
    }
    
    active_streams++;
    return true;
}

static void set_stream_volume(int index, float volume) {
    if (!streams[index]) return;
    
    if (volume < 0.0f || volume > 1.0f) {
        fprintf(stderr, "Invalid volume %f for stream %d (must be between 0.0 and 1.0)\n",
                volume, index);
        return;
    }
    
    float values[1] = {volume};
    int res = pw_stream_set_control(streams[index], 
                                  SPA_PROP_volume, 1, values);
    if (res < 0) {
        fprintf(stderr, "Failed to set volume for stream %d: %s\n", 
                index, spa_strerror(res));
    }
}

static void set_stream_mute(int index, bool mute) {
    if (!streams[index]) return;
    
    float values[1] = {mute ? 1.0f : 0.0f};
    int res = pw_stream_set_control(streams[index], 
                                  SPA_PROP_mute, 1, values);
    if (res < 0) {
        fprintf(stderr, "Failed to set mute for stream %d: %s\n", 
                index, spa_strerror(res));
    }
}

static void signal_handler(int signo) {
    if (signo == SIGINT) {
        fprintf(stderr, "\nReceived SIGINT, cleaning up...\n");
        test_running = false;
        if (main_loop) {
            pw_main_loop_quit(main_loop);
        }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    
    pw_init(&argc, &argv);
    
    main_loop = pw_main_loop_new(NULL);
    if (!main_loop) {
        fprintf(stderr, "Failed to create main loop\n");
        return 1;
    }
    
    struct pw_loop *loop = pw_main_loop_get_loop(main_loop);
    context = pw_context_new(loop, NULL, 0);
    if (!context) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    printf("Creating multiple streams...\n");
    
    // Create streams
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!create_stream(i)) {
            fprintf(stderr, "Failed to create stream %d\n", i);
            cleanup_streams();
            pw_context_destroy(context);
            pw_main_loop_destroy(main_loop);
            pw_deinit();
            return 1;
        }
        printf("Stream %d created successfully\n", i);
    }
    
    printf("\nTesting volume controls on all streams...\n");
    float test_volumes[] = {0.0f, 0.5f, 1.0f};
    for (size_t v = 0; v < sizeof(test_volumes)/sizeof(test_volumes[0]); v++) {
        printf("Setting all streams to volume %f\n", test_volumes[v]);
        for (int i = 0; i < MAX_STREAMS; i++) {
            set_stream_volume(i, test_volumes[v]);
        }
        
        // Keep processing audio while waiting for change
        for (int j = 0; j < 50; j++) {
            pw_loop_iterate(loop, 0);
            usleep(10000); // 10ms sleep
        }
    }
    
    printf("\nTesting mute controls on all streams...\n");
    printf("Muting all streams\n");
    for (int i = 0; i < MAX_STREAMS; i++) {
        set_stream_mute(i, true);
    }
    
    // Keep processing audio while waiting for change
    for (int j = 0; j < 50; j++) {
        pw_loop_iterate(loop, 0);
        usleep(10000); // 10ms sleep
    }
    
    printf("Unmuting all streams\n");
    for (int i = 0; i < MAX_STREAMS; i++) {
        set_stream_mute(i, false);
    }
    
    // Keep processing audio while waiting for change
    for (int j = 0; j < 50; j++) {
        pw_loop_iterate(loop, 0);
        usleep(10000); // 10ms sleep
    }
    
    printf("\nSimulating error conditions...\n");
    // Test invalid volume (should be rejected)
    printf("Testing invalid volume...\n");
    set_stream_volume(0, 2.0f);
    
    // Test rapid volume changes
    printf("Testing rapid volume changes...\n");
    for (int i = 0; i < 10; i++) {
        set_stream_volume(1, (float)i / 10.0f);
        pw_loop_iterate(loop, 0);
        usleep(10000);
    }
    
    printf("\nRunning main loop for %d seconds...\n", TEST_DURATION_SEC);
    printf("Press Ctrl+C to stop the test\n");
    
    // Run the main loop
    time_t start = time(NULL);
    while (test_running && (time(NULL) - start) < TEST_DURATION_SEC) {
        pw_loop_iterate(loop, 0);
        usleep(10000); // 10ms sleep to prevent busy loop
    }
    
    printf("\nFinal state:\n");
    for (int i = 0; i < MAX_STREAMS; i++) {
        printf("Stream %d: Volume=%f, Muted=%s\n",
               i, stream_volumes[i], stream_muted[i] ? "yes" : "no");
    }
    
    cleanup_streams();
    pw_context_destroy(context);
    pw_main_loop_destroy(main_loop);
    pw_deinit();
    
    return 0;
} 
