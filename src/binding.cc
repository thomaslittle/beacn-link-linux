#include <napi.h>
 Napi::Value CreateVirtualDevice(const Napi::CallbackInfo& info) { Napi::Env env = info.Env()
 return Napi::Boolean::New(env, true)
 } Napi::Object Init(Napi::Env env, Napi::Object exports) { exports.Set("createVirtualDevice", Napi::Function::New(env, CreateVirtualDevice))
 return exports
 } NODE_API_MODULE(beacn_native, Init)
