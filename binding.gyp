{
  "targets": [{
    "target_name": "beacn_native",
    "sources": [ "lib/native/beacn_native.cc" ],
    "include_dirs": [
      "<!@(node -p \"require('node-addon-api').include\")",
      "/usr/include/pipewire-0.3",
      "/usr/include/spa-0.2"
    ],
    'defines': [ 'NAPI_DISABLE_CPP_EXCEPTIONS' ],
    "libraries": [
      "-lpipewire-0.3"
    ],
    'cflags!': [ '-fno-exceptions' ],
    'cflags_cc!': [ '-fno-exceptions' ],
    'conditions': [
      ['OS=="linux"', {
        'cflags+': [
          '-std=c++17'
        ],
        'cflags_cc+': [
          '-std=c++17'
        ]
      }]
    ]
  }]
}
