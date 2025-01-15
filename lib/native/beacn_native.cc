#include <napi.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <optional>
#include <string.h>
#include <cstring>

// PipeWire includes
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/support/log.h>
#include <spa/support/system.h>
#include <pipewire/thread-loop.h>
#include <spa/debug/types.h>

#define MAX_STREAMS 5
#define PA_UNLOAD_TIMEOUT_USEC 1000000 // 1 second timeout for unloading modules
#define SAMPLE_RATE 48000
#define CHANNELS 2
#define BUFFER_FRAMES 1024

static void cleanup_stream(int index);
static void cleanup();

struct stream_state
{
  bool is_input;
  bool is_ready;
  std::string name;
  std::string description;
  float volume;
  bool mute;

  stream_state() : is_input(false), is_ready(false), volume(1.0f), mute(false) {}
};

static struct pw_context *context = nullptr;
static struct pw_core *core = nullptr;
static struct pw_stream *streams[MAX_STREAMS] = {nullptr};
static struct spa_hook core_listener = {};
static struct spa_hook stream_listeners[MAX_STREAMS] = {};
static struct stream_state stream_states[MAX_STREAMS];
static bool core_ready = false;
static bool core_done = false;
static int sync_seq = 0;
static struct pw_main_loop *main_loop = nullptr;
static struct pw_loop *loop_obj = nullptr;

static void on_core_info(void *data, uint32_t id, int seq)
{
  fprintf(stderr, "Core info received: id=%u seq=%d\n", id, seq);
  core_ready = true;
}

static void on_core_done(void *data, uint32_t id, int seq)
{
  fprintf(stderr, "Core operation completed: id=%u seq=%d\n", id, seq);
  core_ready = true;
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
  fprintf(stderr, "Core error: id=%u seq=%d res=%d message=%s\n", id, seq, res, message ? message : "unknown");
  if (res == -EPIPE)
  {
    fprintf(stderr, "PipeWire connection lost\n");
  }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    nullptr,       /* ping */
    on_core_info,  /* info */
    on_core_done,  /* done */
    on_core_error, /* error */
    nullptr,       /* remove_id */
    nullptr,       /* bound_id */
    nullptr,       /* add_mem */
    nullptr,       /* remove_mem */
    nullptr        /* bound_props */
};

static int find_stream_index(struct pw_stream *stream)
{
  if (!stream)
  {
    return -1;
  }
  for (int i = 0; i < MAX_STREAMS; i++)
  {
    if (streams[i] == stream)
    {
      return i;
    }
  }
  return -1;
}

// Buffer for building pod parameters
uint8_t buffer[1024];

class AudioError : public std::runtime_error
{
public:
  AudioError(const std::string &msg) : std::runtime_error(msg) {}
};

static void cleanup()
{
  // Cleanup all streams
  for (int i = 0; i < MAX_STREAMS; i++)
  {
    if (streams[i])
    {
      // Remove listener first to prevent callbacks
      spa_hook_remove(&stream_listeners[i]);

      // Disconnect stream
      pw_stream_disconnect(streams[i]);

      // Wait for stream to be unconnected with timeout
      struct timespec start, now;
      clock_gettime(CLOCK_MONOTONIC, &start);

      const char *error = nullptr;
      while (pw_stream_get_state(streams[i], &error) != PW_STREAM_STATE_UNCONNECTED)
      {
        pw_loop_iterate(loop_obj, 0);

        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - start.tv_sec) > 1)
        { // 1 second timeout
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
  if (core)
  {
    pw_core_disconnect(core);
    core = nullptr;
  }

  if (context)
  {
    pw_context_destroy(context);
    context = nullptr;
  }

  if (main_loop)
  {
    pw_main_loop_destroy(main_loop);
    main_loop = nullptr;
  }

  pw_deinit();
}

// Initialize PipeWire
static bool init_pipewire()
{
  fprintf(stderr, "Initializing PipeWire...\n");

  // First cleanup any existing state
  cleanup();

  pw_init(nullptr, nullptr);

  fprintf(stderr, "Creating main loop...\n");
  main_loop = pw_main_loop_new(nullptr);
  if (!main_loop)
  {
    fprintf(stderr, "Failed to create main loop: %s\n", strerror(errno));
    pw_deinit();
    return false;
  }

  loop_obj = pw_main_loop_get_loop(main_loop);
  if (!loop_obj)
  {
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
      nullptr);

  if (!props)
  {
    fprintf(stderr, "Failed to create context properties\n");
    pw_main_loop_destroy(main_loop);
    pw_deinit();
    return false;
  }

  context = pw_context_new(loop_obj, props, 0);
  props = nullptr; // context takes ownership

  if (!context)
  {
    fprintf(stderr, "Failed to create context: %s\n", strerror(errno));
    pw_main_loop_destroy(main_loop);
    pw_deinit();
    return false;
  }

  fprintf(stderr, "Connecting to PipeWire daemon...\n");
  core = pw_context_connect(context, nullptr, 0);
  if (!core)
  {
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

  while (!core_ready)
  {
    pw_loop_iterate(loop_obj, 0);

    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - start.tv_sec) > 5)
    { // 5 second timeout
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

static void cleanup_stream(int index)
{
  if (index < 0 || index >= MAX_STREAMS || !streams[index])
  {
    return;
  }

  // Remove listener first to prevent callbacks
  spa_hook_remove(&stream_listeners[index]);

  // Disconnect stream
  pw_stream_disconnect(streams[index]);

  // Wait for stream to be unconnected with timeout
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);

  const char *error = nullptr;
  while (pw_stream_get_state(streams[index], &error) != PW_STREAM_STATE_UNCONNECTED)
  {
    pw_loop_iterate(loop_obj, 0);

    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - start.tv_sec) > 1)
    { // 1 second timeout
      fprintf(stderr, "Warning: Timeout waiting for stream %d to disconnect\n", index);
      break;
    }
  }

  // Destroy stream
  pw_stream_destroy(streams[index]);
  streams[index] = nullptr;
  stream_states[index] = stream_state();
}

static void unload_cb(void *data)
{
  cleanup();
}

// Cleanup PipeWire with timeout
void cleanup_pipewire()
{
  // Cleanup all streams
  for (int i = 0; i < MAX_STREAMS; i++)
  {
    if (streams[i])
    {
      cleanup_stream(i);
    }
  }

  // Cleanup core
  if (core)
  {
    spa_hook_remove(&core_listener);
    pw_core_disconnect(core);
    core = nullptr;
  }

  if (context)
  {
    pw_context_destroy(context);
    context = nullptr;
  }

  if (main_loop)
  {
    pw_main_loop_destroy(main_loop);
    main_loop = nullptr;
  }

  pw_deinit();
}

static void on_stream_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
  int index = find_stream_index((struct pw_stream *)data);
  if (index < 0)
  {
    return;
  }

  fprintf(stderr, "Stream %d state changed: %d -> %d\n", index, old, state);

  switch (state)
  {
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

static void on_stream_destroy(void *data)
{
  struct pw_stream *stream = (struct pw_stream *)data;
  int index = find_stream_index(stream);
  if (index < 0)
  {
    return;
  }

  fprintf(stderr, "Stream %d destroyed\n", index);
  streams[index] = nullptr;
  stream_states[index] = stream_state();
}

static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
  struct pw_stream *stream = (struct pw_stream *)data;
  int index = find_stream_index(stream);
  if (index < 0)
  {
    return;
  }

  if (id != SPA_PARAM_Format || param == nullptr)
  {
    return;
  }

  fprintf(stderr, "Stream %d format changed\n", index);
}

static void on_stream_process(void *data)
{
  struct pw_stream *stream = (struct pw_stream *)data;
  int index = find_stream_index(stream);
  if (index < 0)
  {
    return;
  }

  struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
  if (b == nullptr)
  {
    return;
  }

  struct spa_buffer *buf = b->buffer;
  float *samples = (float *)buf->datas[0].data;

  if (samples)
  {
    // Fill with silence for now
    size_t n_frames = buf->datas[0].maxsize / sizeof(float) / CHANNELS;
    memset(samples, 0, n_frames * CHANNELS * sizeof(float));
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float) * CHANNELS;
    buf->datas[0].chunk->size = n_frames * sizeof(float) * CHANNELS;
  }

  pw_stream_queue_buffer(stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .destroy = on_stream_destroy,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_stream_process};

// Create a virtual device
static void create_virtual_device(const char *name, const char *description, bool is_source)
{
  fprintf(stderr, "\nCreating virtual device: %s\n", name);
  fprintf(stderr, "Description: %s\n", description);
  fprintf(stderr, "Type: %s\n", is_source ? "Source" : "Sink");

  // First check if we already have this device
  int existing_index = -1;
  for (int i = 0; i < MAX_STREAMS; i++)
  {
    if (streams[i] != nullptr && stream_states[i].name == name)
    {
      existing_index = i;
      break;
    }
  }

  if (existing_index >= 0)
  {
    fprintf(stderr, "Device %s already exists at index %d\n", name, existing_index);
    return;
  }

  // Find available slot
  int index = -1;
  for (int i = 0; i < MAX_STREAMS; i++)
  {
    if (streams[i] == nullptr)
    {
      index = i;
      break;
    }
  }

  if (index < 0)
  {
    fprintf(stderr, "No available stream slots\n");
    return;
  }

  fprintf(stderr, "Using stream slot %d\n", index);

  // Create stream properties
  fprintf(stderr, "Creating stream properties...\n");
  struct pw_properties *props = pw_properties_new(
      PW_KEY_MEDIA_CLASS, is_source ? "Audio/Source" : "Audio/Sink",
      PW_KEY_NODE_NAME, name,
      PW_KEY_NODE_DESCRIPTION, description,
      PW_KEY_NODE_VIRTUAL, "1",
      PW_KEY_NODE_NETWORK, "1",
      PW_KEY_MEDIA_TYPE, "audio",
      PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE, "Music",
      PW_KEY_AUDIO_CHANNELS, "2",
      PW_KEY_AUDIO_FORMAT, "F32",
      PW_KEY_AUDIO_RATE, "48000",
      PW_KEY_CLIENT_NAME, "BEACN Link",
      PW_KEY_APP_NAME, "BEACN Link",
      PW_KEY_APP_ID, "com.beacn.link",
      PW_KEY_APP_ICON_NAME, "audio-card",
      PW_KEY_FACTORY_NAME, "support.null-audio-sink",
      PW_KEY_PRIORITY_SESSION, "100",
      PW_KEY_PRIORITY_DRIVER, "100",
      PW_KEY_OBJECT_PATH, name,
      PW_KEY_OBJECT_SERIAL, "1",
      "factory.mode", "merge",
      "audio.position", "FL,FR",
      "audio.channels", "2",
      "audio.format", "F32LE",
      "audio.rate", "48000",
      "node.pause-on-idle", "false",
      "node.always-process", "true",
      "pulse.server.type", "unix",
      "pulse.min.req", "1024/48000",
      "pulse.min.frag", "1024/48000",
      "pulse.min.quantum", "1024/48000",
      nullptr);

  if (!props)
  {
    fprintf(stderr, "Failed to create stream properties\n");
    return;
  }

  // Create stream
  fprintf(stderr, "Creating stream...\n");
  streams[index] = pw_stream_new(core, name, props);
  if (!streams[index])
  {
    fprintf(stderr, "Failed to create stream\n");
    pw_properties_free(props);
    return;
  }

  // Initialize stream state
  stream_states[index].name = name;
  stream_states[index].is_input = !is_source;

  // Add stream listener
  fprintf(stderr, "Adding stream listener...\n");
  spa_zero(stream_listeners[index]);
  pw_stream_add_listener(streams[index], &stream_listeners[index], &stream_events, streams[index]);

  // Create format
  fprintf(stderr, "Creating audio format...\n");
  uint8_t format_buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(format_buffer, sizeof(format_buffer));
  struct spa_audio_info_raw info;
  memset(&info, 0, sizeof(info));
  info.format = SPA_AUDIO_FORMAT_F32_LE;
  info.channels = CHANNELS;
  info.rate = SAMPLE_RATE;
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;
  info.flags = SPA_AUDIO_FLAG_UNPOSITIONED;

  // Create buffer parameters with more typical values
  struct spa_pod_frame f;
  spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
  spa_pod_builder_add(&b,
                      SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(8),
                      SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
                      SPA_PARAM_BUFFERS_size, SPA_POD_Int(BUFFER_FRAMES * sizeof(float) * CHANNELS),
                      SPA_PARAM_BUFFERS_stride, SPA_POD_Int(sizeof(float) * CHANNELS),
                      SPA_PARAM_BUFFERS_align, SPA_POD_Int(16),
                      0);
  spa_pod_builder_pop(&b, &f);

  const struct spa_pod *params[2];
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
  params[1] = spa_pod_builder_frame(&b, &f);

  // Connect stream with proper flags
  fprintf(stderr, "Connecting stream...\n");
  int res = pw_stream_connect(streams[index],
                              is_source ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
                              PW_ID_ANY,
                              (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                                                     PW_STREAM_FLAG_MAP_BUFFERS |
                                                     PW_STREAM_FLAG_RT_PROCESS),
                              params, 2);

  if (res < 0)
  {
    fprintf(stderr, "Failed to connect stream: %s\n", strerror(-res));
    streams[index] = nullptr;
    stream_states[index] = stream_state();
    return;
  }

  // Wait for stream to be ready with more detailed state logging
  fprintf(stderr, "Waiting for stream to be ready...\n");
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);

  const char *error = nullptr;
  enum pw_stream_state last_state = PW_STREAM_STATE_UNCONNECTED;
  bool stream_ready = false;

  while (!stream_ready)
  {
    enum pw_stream_state current_state = pw_stream_get_state(streams[index], &error);

    // Log state changes
    if (current_state != last_state)
    {
      fprintf(stderr, "Stream state changed: %d -> %d\n", last_state, current_state);
      if (error)
      {
        fprintf(stderr, "Stream error: %s\n", error);
      }
      last_state = current_state;
    }

    switch (current_state)
    {
    case PW_STREAM_STATE_ERROR:
      fprintf(stderr, "Stream error: %s\n", error ? error : "unknown");
      pw_stream_disconnect(streams[index]);
      streams[index] = nullptr;
      stream_states[index] = stream_state();
      return;

    case PW_STREAM_STATE_PAUSED:
      // This is actually okay - we'll start streaming when needed
      stream_ready = true;
      break;

    case PW_STREAM_STATE_STREAMING:
      stream_ready = true;
      break;

    default:
      break;
    }

    pw_loop_iterate(loop_obj, 0);

    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - start.tv_sec) > 5)
    { // 5 second timeout
      fprintf(stderr, "Timeout waiting for stream to be ready (stuck in state %d)\n", current_state);
      pw_stream_disconnect(streams[index]);
      streams[index] = nullptr;
      stream_states[index] = stream_state();
      return;
    }
  }

  fprintf(stderr, "Successfully created virtual device: %s\n", name);
}

// Create all virtual devices
void create_virtual_devices()
{
  fprintf(stderr, "Creating virtual devices...\n");
  try
  {
    fprintf(stderr, "Step 1: Initializing PipeWire...\n");
    if (!init_pipewire())
    {
      throw AudioError("Failed to initialize PipeWire");
    }
    fprintf(stderr, "PipeWire initialized successfully\n");

    // Create or connect to virtual devices with timeouts
    const int DEVICE_TIMEOUT_SEC = 10;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    fprintf(stderr, "Step 2: Creating virtual devices with %d second timeout...\n", DEVICE_TIMEOUT_SEC);

    // Create devices in sequence with timeout check
    const char *devices[][2] = {
        {"beacn_link_out", "Link Out"},
        {"beacn_link_2_out", "Link 2 Out"},
        {"beacn_link_3_out", "Link 3 Out"},
        {"beacn_link_4_out", "Link 4 Out"},
        {"beacn_virtual_input", "BEACN Virtual Input"}};

    for (const auto &device : devices)
    {
      clock_gettime(CLOCK_MONOTONIC, &now);
      if ((now.tv_sec - start.tv_sec) > DEVICE_TIMEOUT_SEC)
      {
        fprintf(stderr, "Timeout reached while creating devices\n");
        throw AudioError("Device creation timeout");
      }

      fprintf(stderr, "Creating device: %s (%s)...\n", device[0], device[1]);
      create_virtual_device(device[0], device[1],
                            strcmp(device[0], "beacn_virtual_input") == 0);

      // Give a short delay between device creations
      usleep(100000); // 100ms delay
    }

    fprintf(stderr, "All virtual devices created successfully\n");
  }
  catch (const AudioError &e)
  {
    fprintf(stderr, "Error creating virtual devices: %s\n", e.what());
    cleanup();
    throw;
  }
}

void set_volume(const char *name, float volume)
{
  if (!core)
  {
    throw AudioError("PipeWire not initialized");
  }

  // Find the stream index
  int index = -1;
  for (int i = 0; i < MAX_STREAMS; i++)
  {
    if (streams[i] && stream_states[i].name == name)
    {
      index = i;
      break;
    }
  }

  if (index == -1)
  {
    throw AudioError("Stream not found");
  }

  // Store old volume to detect change
  float old_volume = stream_states[index].volume;

  // Update stream state
  stream_states[index].volume = volume;

  // Set the volume control
  float values[1] = {volume};
  int res = pw_stream_set_control(streams[index], SPA_PROP_volume, 1, values);
  if (res < 0)
  {
    throw AudioError(std::string("Failed to set volume: ") + strerror(-res));
  }

  // Wait for volume change to be confirmed
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);

  while (stream_states[index].volume == old_volume)
  {
    pw_loop_iterate(loop_obj, 0);

    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - start.tv_sec) > 2)
    { // 2 second timeout
      fprintf(stderr, "Warning: Timeout waiting for volume change confirmation\n");
      break;
    }
  }

  fprintf(stderr, "Set volume for stream %d to %f\n", index, volume);
}

void set_mute(const char *name, bool mute)
{
  if (!core)
  {
    throw AudioError("PipeWire not initialized");
  }

  // Find the stream index
  int index = -1;
  for (int i = 0; i < MAX_STREAMS; i++)
  {
    if (streams[i] && stream_states[i].name == name)
    {
      index = i;
      break;
    }
  }

  if (index == -1)
  {
    throw AudioError("Stream not found");
  }

  // Store old mute state to detect change
  bool old_mute = stream_states[index].mute;

  // Update stream state
  stream_states[index].mute = mute;

  // Set the mute control
  float values[1] = {mute ? 1.0f : 0.0f};
  int res = pw_stream_set_control(streams[index], SPA_PROP_mute, 1, values);
  if (res < 0)
  {
    throw AudioError(std::string("Failed to set mute: ") + strerror(-res));
  }

  // Wait for mute change to be confirmed
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);

  while (stream_states[index].mute == old_mute)
  {
    pw_loop_iterate(loop_obj, 0);

    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - start.tv_sec) > 2)
    { // 2 second timeout
      fprintf(stderr, "Warning: Timeout waiting for mute change confirmation\n");
      break;
    }
  }

  fprintf(stderr, "Set mute for stream %d to %d\n", index, mute);
}

// Get device status
Napi::Value get_device_status(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsString())
  {
    Napi::TypeError::New(env, "String expected").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::string device_name = info[0].As<Napi::String>().Utf8Value();

  // Find the stream index
  int stream_index = -1;
  for (int i = 0; i < MAX_STREAMS; i++)
  {
    if (stream_states[i].name == device_name)
    {
      stream_index = i;
      break;
    }
  }

  if (stream_index == -1)
  {
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

Napi::Value CleanupDevices(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();
  cleanup();
  return env.Undefined();
}

Napi::Value CreateVirtualDevice(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  try
  {
    create_virtual_devices();
    return Napi::Boolean::New(env, true);
  }
  catch (const AudioError &e)
  {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return env.Null();
  }
}

Napi::Value GetDeviceStatus(const Napi::CallbackInfo &info)
{
  return get_device_status(info);
}

Napi::Value SetVolume(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsNumber())
  {
    Napi::TypeError::New(env, "Expected device name and volume level").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string device_name = info[0].As<Napi::String>();
  double volume = info[1].As<Napi::Number>().DoubleValue();

  if (volume < 0.0 || volume > 1.0)
  {
    Napi::RangeError::New(env, "Volume must be between 0.0 and 1.0").ThrowAsJavaScriptException();
    return env.Null();
  }

  try
  {
    set_volume(device_name.c_str(), volume);
    return Napi::Boolean::New(env, true);
  }
  catch (const AudioError &e)
  {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return env.Null();
  }
}

Napi::Value SetMute(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsBoolean())
  {
    Napi::TypeError::New(env, "Expected device name and mute state").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string device_name = info[0].As<Napi::String>();
  bool mute = info[1].As<Napi::Boolean>();

  try
  {
    set_mute(device_name.c_str(), mute);
    return Napi::Boolean::New(env, true);
  }
  catch (const AudioError &e)
  {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return env.Null();
  }
}

Napi::Object Init(Napi::Env env, Napi::Object exports)
{
  exports.Set("createVirtualDevice", Napi::Function::New(env, CreateVirtualDevice));
  exports.Set("cleanup", Napi::Function::New(env, CleanupDevices));
  exports.Set("getDeviceStatus", Napi::Function::New(env, GetDeviceStatus));
  exports.Set("setVolume", Napi::Function::New(env, SetVolume));
  exports.Set("setMute", Napi::Function::New(env, SetMute));
  return exports;
}

NODE_API_MODULE(beacn_native, Init)
