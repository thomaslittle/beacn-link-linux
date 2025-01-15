#include <stdio.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

int main(int argc, char *argv[]) {
    pw_init(&argc, &argv);

    struct pw_context *context;
    struct pw_core *core;
    struct pw_loop *loop;
    struct pw_properties *props;

    loop = pw_loop_new(NULL);
    if (!loop) {
        fprintf(stderr, "Failed to create loop\n");
        return 1;
    }

    props = pw_properties_new(
        PW_KEY_CONFIG_NAME, "client-rt.conf",
        PW_KEY_APP_NAME, "pw-test",
        PW_KEY_REMOTE_NAME, "pipewire-0",
        NULL);

    context = pw_context_new(loop, props, 0);
    if (!context) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    core = pw_context_connect(context, NULL, 0);
    if (!core) {
        fprintf(stderr, "Failed to connect: %s\n", spa_strerror(errno));
        return 1;
    }

    printf("Successfully connected to PipeWire\n");

    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_loop_destroy(loop);
    pw_deinit();

    return 0;
} 
