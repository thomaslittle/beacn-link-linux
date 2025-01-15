const beacn = require('./build/Release/beacn_native');

class BeacnDevice {
  constructor(name, description) {
    this.name = name;
    this.description = description;
  }

  getStatus() {
    return beacn.getDeviceStatus(this.name);
  }

  setVolume(volume) {
    if (typeof volume !== 'number' || volume < 0 || volume > 1) {
      throw new Error('Volume must be a number between 0 and 1');
    }
    return beacn.setVolume(this.name, volume);
  }

  setMute(mute) {
    if (typeof mute !== 'boolean') {
      throw new Error('Mute must be a boolean');
    }
    return beacn.setMute(this.name, mute);
  }
}

class BeacnLink {
  constructor() {
    this.devices = {
      mainOutput: new BeacnDevice('beacn_link_out', 'Link Out'),
      output2: new BeacnDevice('beacn_link_2_out', 'Link 2 Out'),
      output3: new BeacnDevice('beacn_link_3_out', 'Link 3 Out'),
      output4: new BeacnDevice('beacn_link_4_out', 'Link 4 Out'),
      virtualInput: new BeacnDevice('beacn_virtual_input', 'BEACN Virtual Input')
    };
    this.initialized = false;
  }

  init() {
    if (this.initialized) {
      return;
    }
    beacn.createVirtualDevice();
    this.initialized = true;

    // Ensure cleanup on process exit
    process.on('exit', () => {
      this.cleanup();
    });

    // Handle Ctrl+C
    process.on('SIGINT', () => {
      console.log('\nCleaning up virtual devices...');
      this.cleanup();
      process.exit(0);
    });
  }

  cleanup() {
    if (this.initialized) {
      beacn.cleanup();
      this.initialized = false;
    }
  }

  getAllDeviceStatus() {
    const status = {};
    for (const [key, device] of Object.entries(this.devices)) {
      try {
        status[key] = device.getStatus();
      } catch (error) {
        console.error(`Error getting status for ${key}:`, error);
        status[key] = null;
      }
    }
    return status;
  }
}

// Create and export a singleton instance
const beacnLink = new BeacnLink();

// Auto-initialize if this is the main module
if (require.main === module) {
  beacnLink.init();
  console.log('BEACN Link initialized. Virtual devices created:');
  const status = beacnLink.getAllDeviceStatus();
  console.log(JSON.stringify(status, null, 2));
}

module.exports = beacnLink;
