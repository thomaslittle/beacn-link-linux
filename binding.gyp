{
  "targets": [{
    "target_name": "nothing",
    "type": "static_library",
    "sources": [],
    "include_dirs": [
      "node_modules/node-addon-api"
    ]
  }, {
    "target_name": "beacn_native",
    "sources": [ "beacn_native.cc" ],
    "include_dirs": [
      "node_modules/node-addon-api",
      "/usr/include/pipewire-0.3",
      "/usr/include/spa-0.2"
    ],
    "defines": [ "NAPI_CPP_EXCEPTIONS" ],
    "libraries": [
      "-lpipewire-0.3"
    ],
    "dependencies": ["nothing"],
    "cflags!": [ "-fno-exceptions" ],
    "cflags_cc!": [ "-fno-exceptions" ]
  }]
}
