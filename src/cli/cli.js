#!/usr/bin/env node

const beacnNative = require('../../build/Release/beacn_native');
const chalk = require('chalk');
const readline = require('readline');

// Color scheme
const colors = {
  title: chalk.bold.cyan,
  success: chalk.green,
  error: chalk.red,
  warning: chalk.yellow,
  info: chalk.blue,
  device: chalk.magenta,
  state: chalk.cyan,
  prompt: chalk.bold.green,
  header: chalk.bold.white,
  detail: chalk.gray,
  command: chalk.yellow,
};

// Logging utilities
function logTitle(text) {
  console.log('\n' + colors.title('=== ' + text + ' ==='));
}

function logSuccess(text) {
  console.log(colors.success('✓ ' + text));
}

function logError(text) {
  console.log(colors.error('✗ ' + text));
}

function logInfo(text) {
  console.log(colors.info('ℹ ' + text));
}

function logWarning(text) {
  console.log(colors.warning('⚠ ' + text));
}

function logDevice(name, description) {
  console.log(colors.device(`${name}`) + colors.detail(` (${description})`));
}

function logState(text, state) {
  console.log(colors.detail('  └─ ') + colors.state(text) + ': ' + state);
}

function logStep(number, text) {
  console.log(colors.header(`\nStep ${number}: `) + text);
}

// Main CLI interface
console.log(colors.title('\nBEACN Link CLI'));
console.log(colors.info('Type \'help\' for available commands\n'));

logTitle('Initializing Virtual Devices');

// Device creation process
logStep(1, 'Creating Virtual Devices');
logInfo('Initializing PipeWire...');

try {
  beacnNative.createVirtualDevice();
  logSuccess('Successfully created virtual devices');
} catch (error) {
  logError('Failed to create virtual devices: ' + error.message);
  process.exit(1);
}

const devices = [
  { name: 'beacn_link_out', description: 'Link Out', type: 'Sink' },
  { name: 'beacn_link_2_out', description: 'Link 2 Out', type: 'Sink' },
  { name: 'beacn_link_3_out', description: 'Link 3 Out', type: 'Sink' },
  { name: 'beacn_link_4_out', description: 'Link 4 Out', type: 'Sink' },
  { name: 'beacn_virtual_input', description: 'BEACN Virtual Input', type: 'Source' }
];

logTitle('Device Status');

for (const device of devices) {
  logDevice(device.name, device.description);
  try {
    const status = beacnNative.getDeviceStatus(device.name);
    logState('Type', device.type);
    logState('Volume', Math.round(status.volume * 100) + '%');
    logState('Mute', status.mute ? 'Yes' : 'No');
    logState('State', 'PAUSED');
  } catch (error) {
    logError(`Failed to get status: ${error.message}`);
  }
}

logTitle('Ready');
logSuccess('All virtual devices are ready');

// CLI prompt
const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  prompt: colors.prompt('beacn> ')
});

rl.prompt();

rl.on('line', (line) => {
  const command = line.trim().toLowerCase();
  const args = command.split(' ');

  switch (args[0]) {
    case 'help':
      logTitle('Available Commands');
      console.log(colors.command('help') + colors.detail('                     - Show this help message'));
      console.log(colors.command('status [device]') + colors.detail('         - Show device status'));
      console.log(colors.command('list') + colors.detail('                     - List all devices'));
      console.log(colors.command('volume <device> <0-100>') + colors.detail(' - Set device volume'));
      console.log(colors.command('mute <device> <on|off>') + colors.detail('  - Set device mute'));
      console.log(colors.command('quit') + colors.detail('                     - Exit the application'));
      break;

    case 'status':
      logTitle('Device Status');
      if (args[1]) {
        const device = devices.find(d => d.name === args[1]);
        if (!device) {
          logError(`Device not found: ${args[1]}`);
          break;
        }
        logDevice(device.name, device.description);
        try {
          const status = beacnNative.getDeviceStatus(device.name);
          logState('Type', device.type);
          logState('Volume', Math.round(status.volume * 100) + '%');
          logState('Mute', status.mute ? 'Yes' : 'No');
          logState('State', 'PAUSED');
        } catch (error) {
          logError(`Failed to get status: ${error.message}`);
        }
      } else {
        for (const device of devices) {
          logDevice(device.name, device.description);
          try {
            const status = beacnNative.getDeviceStatus(device.name);
            logState('Type', device.type);
            logState('Volume', Math.round(status.volume * 100) + '%');
            logState('Mute', status.mute ? 'Yes' : 'No');
            logState('State', 'PAUSED');
          } catch (error) {
            logError(`Failed to get status: ${error.message}`);
          }
        }
      }
      break;

    case 'volume':
      if (args.length !== 3) {
        logError('Usage: volume <device> <0-100>');
        break;
      }
      const volume = parseInt(args[2]);
      if (isNaN(volume) || volume < 0 || volume > 100) {
        logError('Volume must be between 0 and 100');
        break;
      }
      try {
        beacnNative.setVolume(args[1], volume / 100);
        logSuccess(`Set ${args[1]} volume to ${volume}%`);
      } catch (error) {
        logError(`Failed to set volume: ${error.message}`);
      }
      break;

    case 'mute':
      if (args.length !== 3 || !['on', 'off'].includes(args[2])) {
        logError('Usage: mute <device> <on|off>');
        break;
      }
      try {
        beacnNative.setMute(args[1], args[2] === 'on');
        logSuccess(`Set ${args[1]} mute to ${args[2]}`);
      } catch (error) {
        logError(`Failed to set mute: ${error.message}`);
      }
      break;

    case 'list':
      logTitle('Available Devices');
      for (const device of devices) {
        logDevice(device.name, device.description);
      }
      break;

    case 'quit':
      logInfo('Cleaning up...');
      try {
        beacnNative.cleanup();
        logSuccess('Cleanup complete');
      } catch (error) {
        logError(`Cleanup failed: ${error.message}`);
      }
      rl.close();
      process.exit(0);
      break;

    default:
      if (command !== '') {
        logError('Unknown command. Type \'help\' for available commands.');
      }
      break;
  }

  rl.prompt();
}).on('close', () => {
  console.log(colors.info('\nGoodbye!'));
  try {
    beacnNative.cleanup();
  } catch (error) {
    // Ignore cleanup errors on exit
  }
  process.exit(0);
}); 
