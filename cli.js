#!/usr/bin/env node

const beacnLink = require('./index');
const readline = require('readline');

// ANSI color codes
const colors = {
  reset: '\x1b[0m',
  bright: '\x1b[1m',
  green: '\x1b[32m',
  yellow: '\x1b[33m',
  magenta: '\x1b[35m',
  cyan: '\x1b[36m'
};

// Command handlers
const commands = {
  status: (args) => {
    if (args[1]) {
      const device = Object.values(beacnLink.devices).find(d => d.name === args[1]);
      if (!device) {
        return console.log(`${colors.yellow}Device not found: ${args[1]}${colors.reset}`);
      }
      const status = device.getStatus();
      console.log(`\n${colors.bright}${status.description}:${colors.reset}`);
      console.log(JSON.stringify(status, null, 2));
    } else {
      const status = beacnLink.getAllDeviceStatus();
      console.log(`\n${colors.bright}All Device Status:${colors.reset}`);
      console.log(JSON.stringify(status, null, 2));
    }
  },

  volume: (args) => {
    if (args.length !== 3) {
      return console.log(`${colors.yellow}Usage: volume <device> <0-100>${colors.reset}`);
    }
    const device = Object.values(beacnLink.devices).find(d => d.name === args[1]);
    if (!device) {
      return console.log(`${colors.yellow}Device not found: ${args[1]}${colors.reset}`);
    }
    const volume = parseInt(args[2]) / 100;
    if (isNaN(volume) || volume < 0 || volume > 1) {
      return console.log(`${colors.yellow}Volume must be between 0 and 100${colors.reset}`);
    }
    device.setVolume(volume);
    console.log(`${colors.green}Set ${device.description} volume to ${args[2]}%${colors.reset}`);
  },

  mute: (args) => {
    if (args.length !== 3 || !['on', 'off'].includes(args[2].toLowerCase())) {
      return console.log(`${colors.yellow}Usage: mute <device> <on|off>${colors.reset}`);
    }
    const device = Object.values(beacnLink.devices).find(d => d.name === args[1]);
    if (!device) {
      return console.log(`${colors.yellow}Device not found: ${args[1]}${colors.reset}`);
    }
    const mute = args[2].toLowerCase() === 'on';
    device.setMute(mute);
    console.log(`${colors.green}Set ${device.description} mute to ${mute ? 'on' : 'off'}${colors.reset}`);
  },

  list: () => {
    console.log(`\n${colors.bright}Available Devices:${colors.reset}`);
    Object.values(beacnLink.devices).forEach(device => {
      console.log(`${colors.cyan}${device.name}${colors.reset} (${device.description})`);
    });
  },

  help: () => {
    console.log(`
${colors.bright}BEACN Link CLI Commands:${colors.reset}

${colors.cyan}status [device]${colors.reset}        Show status of all devices or specific device
${colors.cyan}volume <device> <0-100>${colors.reset}  Set volume for device (0-100%)
${colors.cyan}mute <device> <on|off>${colors.reset}   Set mute state for device
${colors.cyan}list${colors.reset}                   List all available devices
${colors.cyan}help${colors.reset}                   Show this help message
${colors.cyan}quit${colors.reset}                   Exit the application

${colors.bright}Available Devices:${colors.reset}
- beacn_link_out       (Main Output)
- beacn_link_2_out     (Secondary Output)
- beacn_link_3_out     (Third Output)
- beacn_link_4_out     (Fourth Output)
- beacn_virtual_input  (Virtual Input)
`);
  },

  quit: () => {
    console.log(`\n${colors.green}Cleaning up virtual devices...${colors.reset}`);
    cleanup();
    process.exit(0);
  },

  exit: () => commands.quit()
};

function cleanup() {
  if (beacnLink.initialized) {
    beacnLink.cleanup();
  }
}

function handleCommand(cmd) {
  const args = cmd.trim().split(/\s+/);
  const command = args[0].toLowerCase();

  try {
    if (command in commands) {
      commands[command](args);
    } else {
      console.log(`${colors.yellow}Unknown command. Type 'help' for available commands.${colors.reset}`);
    }
  } catch (error) {
    console.error(`${colors.yellow}Error: ${error.message}${colors.reset}`);
  }
}

// Main CLI loop
async function main() {
  process.on('SIGINT', commands.quit);
  process.on('SIGTERM', commands.quit);
  process.on('exit', cleanup);

  try {
    // Check if running with arguments
    if (process.argv.length > 2) {
      beacnLink.init();
      handleCommand(process.argv.slice(2).join(' '));
      cleanup();
      return;
    }

    console.log(`${colors.bright}BEACN Link CLI${colors.reset}`);
    console.log(`Type 'help' for available commands\n`);

    beacnLink.init();

    const rl = readline.createInterface({
      input: process.stdin,
      output: process.stdout,
      prompt: `${colors.magenta}beacn>${colors.reset} `,
      removeHistoryDuplicates: true
    });

    rl.prompt();

    rl.on('line', (line) => {
      if (line.trim()) {
        handleCommand(line);
      }
      rl.prompt();
    }).on('close', commands.quit);

  } catch (error) {
    console.error(`${colors.yellow}Fatal error: ${error.message}${colors.reset}`);
    process.exit(1);
  }
}

main(); 
