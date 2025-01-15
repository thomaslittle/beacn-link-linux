# Debug Log

This document provides detailed debugging information for the BEACN Desktop Linux application.

## Virtual Device States

### State Transitions
```
UNCONNECTED (0) -> CONNECTING (1) -> PAUSED (2) -> STREAMING (3)
```

Each state change is logged with the following format:
```
Stream <id> state changed: <old_state> -> <new_state>
Stream <id> <state_name>
```

### State Descriptions

1. **UNCONNECTED (0)**
   - Initial state when stream is created
   - No connection to PipeWire daemon
   - Properties and format not yet set

2. **CONNECTING (1)**
   - Attempting to connect to PipeWire
   - Stream properties being set
   - Audio format being negotiated
   - If stuck in this state:
     - Check PipeWire daemon status
     - Verify audio format compatibility
     - Check for resource conflicts

3. **PAUSED (2)**
   - Successfully connected to PipeWire
   - Stream ready for audio data
   - Default state after connection
   - Normal state when no audio is flowing
   - Properties:
     - Format: 32-bit float
     - Rate: 48000 Hz
     - Channels: 2 (stereo)
     - Buffer size: 1024 frames

4. **STREAMING (3)**
   - Actively processing audio data
   - Buffer callbacks being processed
   - Audio data flowing through device
   - Only enters this state when:
     - Audio data is being sent
     - Device is actively used by an application

## Debug Output Format

### Device Creation
```
Creating virtual device: <name>
Description: <description>
Type: <Sink|Source>
Using stream slot <id>
Creating stream properties...
Creating stream...
Adding stream listener...
Creating audio format...
Connecting stream...
```

### Core Operations
```
Core info received: id=<id> seq=<seq>
Core operation completed: id=<id> seq=<seq>
```

### Stream Events
```
Stream <id> format changed
Stream state changed: <old> -> <new>
Stream <id> <state_name>
```

## Common Issues and Debug Steps

### 1. Device Creation Issues
- Check PipeWire daemon status:
  ```bash
  systemctl --user status pipewire
  ```
- Verify PipeWire version:
  ```bash
  pw-cli info all | grep version
  ```
- Check for existing nodes:
  ```bash
  pw-cli ls Node
  ```

### 2. State Transition Issues
- Monitor state changes in real-time:
  ```bash
  PIPEWIRE_DEBUG=3 node cli.js
  ```
- Check for format negotiation issues:
  ```bash
  pw-top
  ```
- Verify no conflicting devices:
  ```bash
  pactl list short sinks
  pactl list short sources
  ```

### 3. Audio Flow Issues
- Check buffer processing:
  ```bash
  pw-profiler
  ```
- Monitor audio levels:
  ```bash
  pw-mon
  ```
- Check for xruns:
  ```bash
  pw-metadata -n settings 0 clock.force-rate
  ```

## Debug Environment Variables

- `PIPEWIRE_DEBUG=3`: Enable detailed PipeWire debug output
- `PIPEWIRE_LOG_SYSTEMD=true`: Send debug output to systemd journal
- `PIPEWIRE_LATENCY`: Override default latency settings
- `PIPEWIRE_NODE`: Override node settings

## Testing Commands

1. Test device visibility:
```bash
pw-cli ls Node | grep beacn
```

2. Test device properties:
```bash
pw-cli enum Device | grep -A 20 beacn
```

3. Test audio routing:
```bash
pw-link -o beacn_link_out:input_FL another_device:output_FL
pw-link -o beacn_link_out:input_FR another_device:output_FR
```

4. Monitor device states:
```bash
watch -n 1 'pw-cli ls Node | grep beacn'
```

## Performance Monitoring

Monitor system performance during device operation:

1. CPU Usage:
```bash
top -p $(pgrep -f beacn)
```

2. Memory Usage:
```bash
ps -o pid,ppid,%mem,rss,cmd -p $(pgrep -f beacn)
```

3. Audio Buffer Stats:
```bash
pw-top | grep beacn
``` 
