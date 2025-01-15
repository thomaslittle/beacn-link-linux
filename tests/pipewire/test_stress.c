#include <stdio.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>

#define MAX_STREAMS 8
#define TEST_DURATION_SEC 30
#define OPERATION_INTERVAL_USEC 1000 // 1ms between operations
#define STATS_INTERVAL_SEC 5

static struct pw_main_loop *main_loop = NULL;
static struct pw_context *context = NULL;
static struct pw_stream *streams[MAX_STREAMS] = {NULL};
static struct spa_hook stream_listeners[MAX_STREAMS] = {0};
static float stream_volumes[MAX_STREAMS] = {1.0f};
static bool stream_muted[MAX_STREAMS] = {false};
static int active_streams = 0;
static bool test_running = true;

// Performance metrics
static struct {
    unsigned long total_operations;
    unsigned long successful_operations;
    unsigned long failed_operations;
    unsigned long state_changes;
    unsigned long errors;
    struct timeval start_time;
    struct timeval last_stats_time;
} metrics = {0};

static void print_stats(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    
    double elapsed = (now.tv_sec - metrics.start_time.tv_sec) + 
                    (now.tv_usec - metrics.start_time.tv_usec) / 1000000.0;
    
    printf("\nPerformance Stats (%.2f seconds):\n", elapsed);
    printf("Total operations: %lu\n", metrics.total_operations);
    printf("Successful operations: %lu\n", metrics.successful_operations);
    printf("Failed operations: %lu\n", metrics.failed_operations);
    printf("State changes: %lu\n", metrics.state_changes);
    printf("Errors: %lu\n", metrics.errors);
    printf("Operations per second: %.2f\n", 
           metrics.total_operations / elapsed);
    printf("Success rate: %.2f%%\n", 
           (metrics.successful_operations * 100.0) / metrics.total_operations);
    
    metrics.last_stats_time = now;
}

static void on_stream_state_changed(void *data, 
                                  enum pw_stream_state old,
                                  enum pw_stream_state state, 
                                  const char *error) {
    int stream_id = (int)(intptr_t)data;
    fprintf(stderr, "Stream %d state changed: %d -> %d (error: %s)\n", 
            stream_id, old, state, error ? error : "none");
    
    metrics.state_changes++;
    
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "Stream %d error, attempting recovery...\n", stream_id);
        metrics.errors++;
        
        // Attempt recovery by reconnecting the stream
        pw_stream_disconnect(streams[stream_id]);
        
        uint8_t buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const struct spa_pod *params[1];
        params[0] = spa_format_audio_raw_build(&b,
            SPA_PARAM_EnumFormat,
            &SPA_AUDIO_INFO_RAW_INIT(
                .format = SPA_AUDIO_FORMAT_F32,
                .channels = 2,
                .rate = 48000
            ));
        
        int res = pw_stream_connect(streams[stream_id],
            PW_DIRECTION_OUTPUT,
            PW_ID_ANY,
            (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | 
                                 PW_STREAM_FLAG_MAP_BUFFERS | 
                                 PW_STREAM_FLAG_RT_PROCESS),
            params, 1);
        
        if (res < 0) {
            fprintf(stderr, "Failed to reconnect stream %d: %s\n", 
                    stream_id, spa_strerror(res));
        }
    }
}

static void on_stream_control_info(void *data,
                                 uint32_t id,
                                 const struct pw_stream_control *control) {
    int stream_id = (int)(intptr_t)data;
    if (id == SPA_PROP_volume) {
        stream_volumes[stream_id] = control->values[0];
    } else if (id == SPA_PROP_mute) {
        stream_muted[stream_id] = control->values[0] > 0.0f;
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .control_info = on_stream_control_info,
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
        PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_NAME, name,
        PW_KEY_NODE_DESCRIPTION, desc,
        PW_KEY_NODE_VIRTUAL, "1",
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
            .channels = 2,
            .rate = 48000
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

static bool set_stream_volume(int index, float volume) {
    if (!streams[index]) return false;
    
    metrics.total_operations++;
    
    float values[1] = {volume};
    int res = pw_stream_set_control(streams[index], 
                                  SPA_PROP_volume, 1, values);
    if (res < 0) {
        fprintf(stderr, "Failed to set volume for stream %d: %s\n", 
                index, spa_strerror(res));
        metrics.failed_operations++;
        return false;
    }
    
    metrics.successful_operations++;
    return true;
}

static bool set_stream_mute(int index, bool mute) {
    if (!streams[index]) return false;
    
    metrics.total_operations++;
    
    float values[1] = {mute ? 1.0f : 0.0f};
    int res = pw_stream_set_control(streams[index], 
                                  SPA_PROP_mute, 1, values);
    if (res < 0) {
        fprintf(stderr, "Failed to set mute for stream %d: %s\n", 
                index, spa_strerror(res));
        metrics.failed_operations++;
        return false;
    }
    
    metrics.successful_operations++;
    return true;
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
    
    printf("Creating %d streams for stress test...\n", MAX_STREAMS);
    
    // Initialize metrics
    gettimeofday(&metrics.start_time, NULL);
    metrics.last_stats_time = metrics.start_time;
    
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
    
    printf("\nStarting stress test for %d seconds...\n", TEST_DURATION_SEC);
    printf("Press Ctrl+C to stop the test\n");
    
    struct timeval start, now;
    gettimeofday(&start, NULL);
    
    // Run stress test
    while (test_running) {
        gettimeofday(&now, NULL);
        double elapsed = (now.tv_sec - start.tv_sec) + 
                        (now.tv_usec - start.tv_usec) / 1000000.0;
        
        if (elapsed >= TEST_DURATION_SEC) {
            break;
        }
        
        // Perform random operations
        int stream = rand() % MAX_STREAMS;
        float volume = (float)(rand() % 100) / 100.0f;
        bool mute = rand() % 2;
        
        if (rand() % 2) {
            set_stream_volume(stream, volume);
        } else {
            set_stream_mute(stream, mute);
        }
        
        // Print stats every STATS_INTERVAL_SEC seconds
        if (now.tv_sec - metrics.last_stats_time.tv_sec >= STATS_INTERVAL_SEC) {
            print_stats();
        }
        
        pw_loop_iterate(loop, 0);
        usleep(OPERATION_INTERVAL_USEC);
    }
    
    // Print final stats
    print_stats();
    
    cleanup_streams();
    pw_context_destroy(context);
    pw_main_loop_destroy(main_loop);
    pw_deinit();
    
    return 0;
} 
