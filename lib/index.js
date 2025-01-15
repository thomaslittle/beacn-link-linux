const beacnNative = require('../build/Release/beacn_native');

// Export the native module functions with better names
module.exports = {
  createVirtualDevices: beacnNative.createVirtualDevice,
  cleanup: beacnNative.cleanup,
  getDeviceStatus: beacnNative.getDeviceStatus,
  setVolume: beacnNative.setVolume,
  setMute: beacnNative.setMute
};
