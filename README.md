# BEACN Desktop Linux

A Linux-compatible version of the BEACN Link app that creates virtual audio devices using PipeWire.

## Prerequisites

- Linux system with PipeWire audio system
- Node.js (v16 or higher)
- npm (Node Package Manager)
- Build tools (gcc, g++, make)
- PipeWire development files

### Installing Dependencies

For Fedora:
```bash
sudo dnf install gcc g++ make pipewire-devel nodejs npm
```

For Ubuntu/Debian:
```bash
sudo apt update
sudo apt install build-essential pipewire-dev nodejs npm
```

For Arch Linux:
```bash
sudo pacman -S gcc make pipewire nodejs npm
```

## Installation

You can install BEACN Desktop Linux globally using npm:

```bash
sudo npm install -g beacn-desktop-linux
```

## Usage

### Command Line Interface

1. Start the interactive CLI:
```bash
beacn
```

2. Or run individual commands:
```bash
# Show device status
beacn status

# Set device volume (0-100)
beacn volume beacn_link_out 80

# Toggle mute
beacn mute beacn_link_out on

# List available devices
beacn list

# Show help
beacn help
```

### Available Commands

- `status [device]` - Show status of all devices or specific device
- `volume <device> <0-100>` - Set volume for device (0-100%)
- `mute <device> <on|off>` - Set mute state for device
- `list` - List all available devices
- `help` - Show help message
- `quit` - Exit the application

### Available Devices

- `beacn_link_out` - Main Output
- `beacn_link_2_out` - Secondary Output
- `beacn_link_3_out` - Third Output
- `beacn_link_4_out` - Fourth Output
- `beacn_virtual_input` - Virtual Input

### Programmatic Usage

You can also use BEACN Desktop Linux in your Node.js applications:

```javascript
const beacnLink = require('beacn-desktop-linux');

// Initialize
beacnLink.init();

// Control devices
const mainOutput = beacnLink.devices.mainOutput;
mainOutput.setVolume(0.8);  // 80%
mainOutput.setMute(false);

// Get device status
const status = mainOutput.getStatus();
console.log(status);

// Cleanup when done
beacnLink.cleanup();
```

## Troubleshooting

1. If the virtual devices don't appear:
   - Make sure PipeWire is running: `systemctl --user status pipewire`
   - Check PipeWire logs: `journalctl --user -u pipewire`

2. If you get permission errors:
   - Make sure your user is in the audio group: `sudo usermod -aG audio $USER`
   - Log out and log back in for the group changes to take effect

3. If the installation fails:
   - Make sure all dependencies are installed
   - Try installing with verbose output: `sudo npm install -g beacn-desktop-linux --verbose`

## Development

To develop or modify BEACN Desktop Linux:

1. Clone the repository:
```bash
git clone https://github.com/yourusername/beacn-desktop-linux.git
cd beacn-desktop-linux
```

2. Install dependencies:
```bash
npm install
```

3. Build the native module:
```bash
npm run build
```

4. Run the CLI in development mode:
```bash
npm start
```

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Contributing

1. Fork the repository
2. Create a feature branch
3. Commit your changes
4. Push to the branch
5. Create a Pull Request

## Acknowledgments

- PipeWire team for the excellent audio system
- BEACN for the original BEACN Link concept
