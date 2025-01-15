#include <napi.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <optional>
#include <string.h>

// PipeWire includes
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/support/log.h>
#include <spa/support/system.h>
#include <pipewire/thread-loop.h>

#define MAX_STREAMS 5
#define PA_UNLOAD_TIMEOUT_USEC 1000000 // 1 second timeout for unloading modules

static void cleanup_stream(int index);
static void cleanup();

struct stream_state {
    bool is_input;
    bool is_ready;
    std::string name;
    std::string description;
    float volume;
    bool mute;

    stream_state() : is_input(false), is_ready(false), volume(1.0f), mute(false) {}
};

static struct pw_thread_loop *loop = nullptr;
static struct pw_context *context = nullptr;
static struct pw_core *core = nullptr;
static struct pw_stream *streams[MAX_STREAMS] = {nullptr};
static struct spa_hook core_listener = {0};
static struct spa_hook stream_listeners[MAX_STREAMS] = {0};
static struct stream_state stream_states[MAX_STREAMS];
static bool core_ready = false;
static bool core_done = false;
static int sync_seq = 0;
static struct pw_main_loop *main_loop = nullptr;
static struct pw_loop *loop_obj = nullptr;

static void on_core_info(void *data, uint32_t id, int seq) {
    fprintf(stderr, "Core info received: id=%u seq=%d\n", id, seq);
    core_ready = true;
}

static void on_core_done(void *data, uint32_t id, int seq) {
    fprintf(stderr, "Core operation completed: id=%u seq=%d\n", id, seq);
    core_ready = true;
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message) {
    fprintf(stderr, "Core error: id=%u seq=%d res=%d message=%s\n", id, seq, res, message ? message : "unknown");
    if (res == -EPIPE) {
        fprintf(stderr, "PipeWire connection lost\n");
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    nullptr,                 /* ping */
    on_core_info,           /* info */
    on_core_done,           /* done */
    on_core_error,          /* error */
    nullptr,                /* remove_id */
    nullptr,                /* bound_id */
    nullptr,                /* add_mem */
    nullptr,                /* remove_mem */
    nullptr                 /* bound_props */
};

static int find_stream_index(struct pw_stream *stream) {
    if (!stream) {
        return -1;
    }
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i] == stream) {
            return i;
        }
    }
    return -1;
}

// Buffer for building pod parameters
uint8_t buffer[1024];

class AudioError : public std::runtime_error {
public:
  AudioError(const std::string& msg) : std::runtime_error(msg) {}
};

static void cleanup() {
    // Cleanup all streams
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i]) {
            // Remove listener first to prevent callbacks
            spa_hook_remove(&stream_listeners[i]);
            
            // Disconnect stream
            pw_stream_disconnect(streams[i]);
            
            // Wait for stream to be unconnected with timeout
            struct timespec start, now;
            clock_gettime(CLOCK_MONOTONIC, &start);
            
            const char* error = nullptr;
            while (pw_stream_get_state(streams[i], &error) != PW_STREAM_STATE_UNCONNECTED) {
                pw_loop_iterate(loop_obj, 0);
                
                clock_gettime(CLOCK_MONOTONIC, &now);
                if ((now.tv_sec - start.tv_sec) > 1) {  // 1 second timeout
                    fprintf(stderr, "Warning: Timeout waiting for stream %d to disconnect\n", i);
                    break;
                }
            }
            
            // Destroy stream
            pw_stream_destroy(streams[i]);
            streams[i] = nullptr;
            stream_states[i] = stream_state();
        }
    }
    
    // Cleanup core
    if (core) {
        pw_core_disconnect(core);
        core = nullptr;
    }
    
    if (context) {
        pw_context_destroy(context);
        context = nullptr;
    }
    
    if (main_loop) {
        pw_main_loop_destroy(main_loop);
        main_loop = nullptr;
    }
    
    pw_deinit();
}

// Initialize PipeWire
static bool init_pipewire() {
    fprintf(stderr, "Initializing PipeWire...\n");
    
    // First cleanup any existing state
    cleanup();
    
    pw_init(nullptr, nullptr);
    
    fprintf(stderr, "Creating main loop...\n");
    main_loop = pw_main_loop_new(nullptr);
    if (!main_loop) {
        fprintf(stderr, "Failed to create main loop: %s\n", strerror(errno));
        pw_deinit();
        return false;
    }
    
    loop_obj = pw_main_loop_get_loop(main_loop);
    if (!loop_obj) {
        fprintf(stderr, "Failed to get loop: %s\n", strerror(errno));
        pw_main_loop_destroy(main_loop);
        pw_deinit();
        return false;
    }
    
    fprintf(stderr, "Creating context...\n");
    struct pw_properties *props = pw_properties_new(
        PW_KEY_CONFIG_NAME, "client-rt.conf",
        PW_KEY_APP_NAME, "beacn-link",
        PW_KEY_APP_PROCESS_BINARY, "beacn",
        PW_KEY_REMOTE_NAME, "pipewire-0",
        nullptr
    );
    
    if (!props) {
        fprintf(stderr, "Failed to create context properties\n");
        pw_main_loop_destroy(main_loop);
        pw_deinit();
        return false;
    }
    
    context = pw_context_new(loop_obj, props, 0);
    props = nullptr;  // context takes ownership
    
    if (!context) {
        fprintf(stderr, "Failed to create context: %s\n", strerror(errno));
        pw_main_loop_destroy(main_loop);
        pw_deinit();
        return false;
    }
    
    fprintf(stderr, "Connecting to PipeWire daemon...\n");
    core = pw_context_connect(context, nullptr, 0);
    if (!core) {
        fprintf(stderr, "Failed to connect to PipeWire daemon: %s\n", strerror(errno));
        pw_context_destroy(context);
        pw_main_loop_destroy(main_loop);
        pw_deinit();
        return false;
    }
    
    // Add core listener
    spa_zero(core_listener);
    pw_core_add_listener(core, &core_listener, &core_events, nullptr);
    
    // Trigger a sync operation to get the done event
    pw_core_sync(core, 0, 0);
    
    // Run the loop for a short time to process events
    fprintf(stderr, "Running event loop...\n");
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    while (!core_ready) {
        pw_loop_iterate(loop_obj, 0);
        
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - start.tv_sec) > 5) {  // 5 second timeout
            fprintf(stderr, "Timeout waiting for core to be ready\n");
            pw_core_disconnect(core);
            pw_context_destroy(context);
            pw_main_loop_destroy(main_loop);
            pw_deinit();
            return false;
        }
    }
    
    fprintf(stderr, "Successfully connected to PipeWire daemon\n");
    return true;
}

static void cleanup_stream(int index) {
    if (index < 0 || index >= MAX_STREAMS || !loop || !streams[index]) {
        return;
    }

    pw_thread_loop_lock(loop);

    try {
        // Remove listener first to prevent callbacks
        spa_hook_remove(&stream_listeners[index]);
        
        // Disconnect stream
        pw_stream_disconnect(streams[index]);
        
        // Wait for stream to be unconnected with timeout
        struct timespec abstime;
        clock_gettime(CLOCK_REALTIME, &abstime);
        abstime.tv_sec += 1; // 1 second timeout
        
        const char* error = nullptr;
        while (pw_stream_get_state(streams[index], &error) != PW_STREAM_STATE_UNCONNECTED) {
            if (pw_thread_loop_timed_wait_full(loop, &abstime) < 0) {
                fprintf(stderr, "Warning: Timeout waiting for stream %d to disconnect\n", index);
                break;
            }
        }
        
        // Destroy stream
        pw_stream_destroy(streams[index]);
        streams[index] = nullptr;
        stream_states[index] = stream_state();
        
        pw_thread_loop_unlock(loop);
    } catch (...) {
        pw_thread_loop_unlock(loop);
        throw;
    }
}

static void unload_cb(void *data) {
    cleanup();
}

// Cleanup PipeWire with timeout
void cleanup_pipewire() {
    if (!loop) {
        return;
    }
    
    // First stop the thread loop to prevent new callbacks
    pw_thread_loop_stop(loop);
    
    pw_thread_loop_lock(loop);
    
    // Cleanup all streams with timeout
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i]) {
            // Remove listener first to prevent callbacks
            spa_hook_remove(&stream_listeners[i]);
            
            // Disconnect stream
            pw_stream_disconnect(streams[i]);
            
            // Wait for stream to be unconnected with timeout
            struct timespec abstime;
            clock_gettime(CLOCK_REALTIME, &abstime);
            abstime.tv_sec += 1; // 1 second timeout
            
            const char* error = nullptr;
            while (pw_stream_get_state(streams[i], &error) != PW_STREAM_STATE_UNCONNECTED) {
                if (pw_thread_loop_timed_wait_full(loop, &abstime) < 0) {
                    fprintf(stderr, "Warning: Timeout waiting for stream %d to disconnect\n", i);
                    break;
                }
            }
            
            // Destroy stream
            pw_stream_destroy(streams[i]);
            streams[i] = nullptr;
            stream_states[i] = stream_state();
        }
    }
    
    // Then cleanup core
    if (core) {
        spa_hook_remove(&core_listener);
        pw_core_disconnect(core);
        core = nullptr;
    }
    
    pw_thread_loop_unlock(loop);
    
    // Cleanup context and loop after unlocking
    if (context) {
        pw_context_destroy(context);
        context = nullptr;
    }
    
    pw_thread_loop_destroy(loop);
    loop = nullptr;
    
    pw_deinit();
}

static void on_stream_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    int index = find_stream_index((struct pw_stream *)data);
    if (index < 0) {
        return;
    }

    fprintf(stderr, "Stream %d state changed: %d -> %d\n", index, old, state);

    switch (state) {
        case PW_STREAM_STATE_ERROR:
            fprintf(stderr, "Stream %d error: %s\n", index, error ? error : "unknown");
            break;
        case PW_STREAM_STATE_UNCONNECTED:
            fprintf(stderr, "Stream %d unconnected\n", index);
            break;
        case PW_STREAM_STATE_CONNECTING:
            fprintf(stderr, "Stream %d connecting\n", index);
            break;
        case PW_STREAM_STATE_PAUSED:
            fprintf(stderr, "Stream %d paused\n", index);
            break;
        case PW_STREAM_STATE_STREAMING:
            fprintf(stderr, "Stream %d streaming\n", index);
            break;
    }
}

static void on_stream_destroy(void *data) {
    struct pw_stream *stream = (struct pw_stream *)data;
    int index = find_stream_index(stream);
    if (index < 0) {
        return;
    }

    fprintf(stderr, "Stream %d destroyed\n", index);
    streams[index] = nullptr;
    stream_states[index] = stream_state();
}

static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    struct pw_stream *stream = (struct pw_stream *)data;
    int index = find_stream_index(stream);
    if (index < 0) {
        return;
    }

    if (id != SPA_PARAM_Format || param == nullptr) {
        return;
    }

    fprintf(stderr, "Stream %d format changed\n", index);
}

static void on_stream_process(void *data) {
    struct pw_stream *stream = (struct pw_stream *)data;
    int index = find_stream_index(stream);
    if (index < 0) {
        return;
    }

    struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
    if (b == nullptr) {
        return;
    }

    // Process audio data here
    // ...

    pw_stream_queue_buffer(stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .process = on_stream_process,
    .param_changed = on_stream_param_changed,
    .destroy = on_stream_destroy
};

// Create a virtual device
static void create_virtual_device(const char *name, const char *description, bool is_source) {
    // First check if we already have this device
    int existing_index = -1;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i] != nullptr && stream_states[i].name == name) {
            existing_index = i;
            break;
        }
    }

    if (existing_index >= 0) {
        fprintf(stderr, "Device %s already exists at index %d\n", name, existing_index);
        return;
    }

    int index = -1;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i] == nullptr) {
            index = i;
            break;
        }
    }

    if (index < 0) {
        fprintf(stderr, "No available stream slots\n");
        return;
    }

    // Create stream properties
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_CLASS, is_source ? "Audio/Source" : "Audio/Sink",
        PW_KEY_NODE_NAME, name,
        PW_KEY_NODE_DESCRIPTION, description,
        PW_KEY_NODE_VIRTUAL, "1",
        PW_KEY_NODE_NETWORK, "1",
        nullptr
    );

    if (!props) {
        fprintf(stderr, "Failed to create stream properties\n");
        return;
    }

    // Create stream
    streams[index] = pw_stream_new(core, name, props);
    if (!streams[index]) {
        fprintf(stderr, "Failed to create stream\n");
        pw_properties_free(props);
        return;
    }

    // Initialize stream state
    stream_states[index].name = name;
    stream_states[index].is_input = !is_source;

    // Add stream listener
    spa_zero(stream_listeners[index]);
    pw_stream_add_listener(streams[index], &stream_listeners[index], &stream_events, streams[index]);

    // Create format
    uint8_t format_buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(format_buffer, sizeof(format_buffer));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b,
        SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32,
            .channels = 2,
            .rate = 48000
        ));

    // Connect stream
    int res = pw_stream_connect(streams[index],
        is_source ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
        PW_ID_ANY,
        (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params, 1);

    if (res < 0) {
        fprintf(stderr, "Failed to connect stream: %s\n", spa_strerror(res));
        streams[index] = nullptr;
        stream_states[index] = stream_state();
        return;
    }

    // Wait for stream to be ready
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (pw_stream_get_state(streams[index], nullptr) != PW_STREAM_STATE_STREAMING) {
        pw_loop_iterate(loop_obj, 0);

        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - start.tv_sec) > 5) {  // 5 second timeout
            fprintf(stderr, "Timeout waiting for stream to be ready\n");
            pw_stream_disconnect(streams[index]);
            streams[index] = nullptr;
            stream_states[index] = stream_state();
            return;
        }
    }

    fprintf(stderr, "Successfully created virtual device: %s\n", name);
}

// Create all virtual devices
void create_virtual_devices() {
    fprintf(stderr, "Creating virtual devices...\n");
    try {
        if (!init_pipewire()) {
            throw AudioError("Failed to initialize PipeWire");
        }
        
        // Create or connect to virtual devices
        fprintf(stderr, "Creating Link Out...\n");
        create_virtual_device("beacn_link_out", "Link Out", false);
        
        fprintf(stderr, "Creating Link 2 Out...\n");
        create_virtual_device("beacn_link_2_out", "Link 2 Out", false);
        
        fprintf(stderr, "Creating Link 3 Out...\n");
        create_virtual_device("beacn_link_3_out", "Link 3 Out", false);
        
        fprintf(stderr, "Creating Link 4 Out...\n");
        create_virtual_device("beacn_link_4_out", "Link 4 Out", false);
        
        fprintf(stderr, "Creating Virtual Input...\n");
        create_virtual_device("beacn_virtual_input", "BEACN Virtual Input", true);
        
        fprintf(stderr, "All virtual devices created successfully\n");
    } catch (const AudioError& e) {
        fprintf(stderr, "Error creating virtual devices: %s\n", e.what());
        cleanup();
        throw;
    }
}

void set_volume(const char* name, float volume) {
    if (!loop || !core) {
        throw AudioError("PipeWire not initialized");
    }

    // Find the stream index first without locking
    int index = -1;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i] && stream_states[i].name == name) {
            index = i;
            break;
        }
    }
    
    if (index == -1) {
        throw AudioError("Stream not found");
    }

    pw_thread_loop_lock(loop);

    try {
        // Store old volume to detect change
        float old_volume = stream_states[index].volume;
        
        // Update stream state
        stream_states[index].volume = volume;
        
        // Set the volume control
        float values[1] = {volume};
        int res = pw_stream_set_control(streams[index], SPA_PROP_volume, 1, values);
        if (res < 0) {
            pw_thread_loop_unlock(loop);
            throw AudioError(std::string("Failed to set volume: ") + strerror(-res));
        }
        
        // Wait for volume change to be confirmed via param_changed callback
        struct timespec abstime;
        clock_gettime(CLOCK_REALTIME, &abstime);
        abstime.tv_sec += 2; // 2 second timeout
        
        while (stream_states[index].volume == old_volume) {
            if (pw_thread_loop_timed_wait_full(loop, &abstime) < 0) {
                fprintf(stderr, "Warning: Timeout waiting for volume change confirmation\n");
                break;
            }
        }
        
        fprintf(stderr, "Set volume for stream %d to %f\n", index, volume);
        
        pw_thread_loop_unlock(loop);
    } catch (...) {
        pw_thread_loop_unlock(loop);
        throw;
    }
}

void set_mute(const char* name, bool mute) {
    if (!loop || !core) {
        throw AudioError("PipeWire not initialized");
    }

    // Find the stream index first without locking
    int index = -1;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i] && stream_states[i].name == name) {
            index = i;
            break;
        }
    }
    
    if (index == -1) {
        throw AudioError("Stream not found");
    }

    pw_thread_loop_lock(loop);

    try {
        // Store old mute state to detect change
        bool old_mute = stream_states[index].mute;
        
        // Update stream state
        stream_states[index].mute = mute;
        
        // Set the mute control
        float values[1] = {mute ? 1.0f : 0.0f};
        int res = pw_stream_set_control(streams[index], SPA_PROP_mute, 1, values);
        if (res < 0) {
            pw_thread_loop_unlock(loop);
            throw AudioError(std::string("Failed to set mute: ") + strerror(-res));
        }
        
        // Wait for mute change to be confirmed via param_changed callback
        struct timespec abstime;
        clock_gettime(CLOCK_REALTIME, &abstime);
        abstime.tv_sec += 2; // 2 second timeout
        
        while (stream_states[index].mute == old_mute) {
            if (pw_thread_loop_timed_wait_full(loop, &abstime) < 0) {
                fprintf(stderr, "Warning: Timeout waiting for mute change confirmation\n");
                break;
            }
        }
        
        fprintf(stderr, "Set mute for stream %d to %d\n", index, mute);
        
        pw_thread_loop_unlock(loop);
    } catch (...) {
        pw_thread_loop_unlock(loop);
        throw;
    }
}

// Get device status
Napi::Value get_device_status(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "String expected").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    std::string device_name = info[0].As<Napi::String>().Utf8Value();
    
    // Find the stream index
    int stream_index = -1;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (stream_states[i].name == device_name) {
            stream_index = i;
            break;
        }
    }
    
    if (stream_index == -1) {
        Napi::Error::New(env, "Stream not found").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    Napi::Object status = Napi::Object::New(env);
    status.Set("name", stream_states[stream_index].name);
    status.Set("description", stream_states[stream_index].description);
    status.Set("volume", stream_states[stream_index].volume);
    status.Set("mute", stream_states[stream_index].mute);
    
    return status;
}

Napi::Value CleanupDevices(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    cleanup();
    return env.Undefined();
}

Napi::Value CreateVirtualDevice(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    try {
        create_virtual_devices();
        return Napi::Boolean::New(env, true);
    } catch (const AudioError& e) {
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
        return env.Null();
    }
}

Napi::Value GetDeviceStatus(const Napi::CallbackInfo& info) {
    return get_device_status(info);
}

Napi::Value SetVolume(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsNumber()) {
        Napi::TypeError::New(env, "Expected device name and volume level").ThrowAsJavaScriptException();
        return env.Null();
    }
    
    std::string device_name = info[0].As<Napi::String>();
    double volume = info[1].As<Napi::Number>().DoubleValue();
    
    if (volume < 0.0 || volume > 1.0) {
        Napi::RangeError::New(env, "Volume must be between 0.0 and 1.0").ThrowAsJavaScriptException();
        return env.Null();
    }
    
    try {
        set_volume(device_name.c_str(), volume);
        return Napi::Boolean::New(env, true);
    } catch (const AudioError& e) {
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
        return env.Null();
    }
}

Napi::Value SetMute(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsBoolean()) {
        Napi::TypeError::New(env, "Expected device name and mute state").ThrowAsJavaScriptException();
        return env.Null();
    }
    
    std::string device_name = info[0].As<Napi::String>();
    bool mute = info[1].As<Napi::Boolean>();
    
    try {
        set_mute(device_name.c_str(), mute);
        return Napi::Boolean::New(env, true);
    } catch (const AudioError& e) {
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
        return env.Null();
    }
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("createVirtualDevice", Napi::Function::New(env, CreateVirtualDevice));
    exports.Set("cleanup", Napi::Function::New(env, CleanupDevices));
    exports.Set("getDeviceStatus", Napi::Function::New(env, GetDeviceStatus));
    exports.Set("setVolume", Napi::Function::New(env, SetVolume));
    exports.Set("setMute", Napi::Function::New(env, SetMute));
    return exports;
}

NODE_API_MODULE(beacn_native, Init)
