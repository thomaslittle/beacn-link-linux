# Changelog

All notable changes to the BEACN Desktop Linux project will be documented in this file.

## [Unreleased]

### Added
- Initial project setup with PipeWire integration
- Native C++ module for virtual device creation
- Node.js CLI interface
- Virtual device creation:
  - Link Out (Main Output)
  - Link 2 Out (Secondary Output)
  - Link 3 Out (Third Output)
  - Link 4 Out (Fourth Output)
  - BEACN Virtual Input
- Device state management and monitoring
- Basic audio stream handling
- Detailed logging system
- Debug tools and utilities

### Changed
- Migrated from thread loop to main loop architecture
- Improved stream state handling
- Enhanced error handling and recovery
- Updated device creation process for better reliability
- Optimized audio buffer management

### Fixed
- Stream state transition issues
- Device visibility in PipeWire
- Audio format negotiation
- Resource cleanup on exit
- Stream connection timeouts

## [0.1.0] - Initial Development

### Added
- Basic project structure
- PipeWire integration
  - Stream creation
  - Audio format configuration
  - Device properties
  - State management
- Core functionality:
  - Virtual device creation
  - Audio stream handling
  - State transitions
  - Event processing
- Documentation:
  - README.md with usage instructions
  - DEBUG_LOG.md with debugging information
  - Code comments and documentation

### Technical Details

#### Virtual Device Implementation
- Audio format: 32-bit float
- Sample rate: 48kHz
- Channels: 2 (stereo)
- Buffer size: 1024 frames
- State management:
  - UNCONNECTED -> CONNECTING -> PAUSED -> STREAMING
  - Proper state transition handling
  - State change logging

#### PipeWire Integration
- Direct PipeWire API usage
- Stream property configuration
- Audio format negotiation
- Buffer processing
- Event handling

#### Architecture Changes
- Implemented main loop based architecture
- Removed thread loop dependencies
- Improved synchronization
- Enhanced error handling
- Added timeout management

#### Testing & Debugging
- Added basic PipeWire connectivity test
- Implemented stream creation tests
- Added state transition verification
- Included buffer processing tests
- Created debug logging system

### Infrastructure

#### Build System
- Node.js native module setup
- GYP build configuration
- Dependency management
- Installation scripts

#### Development Tools
- Debug logging system
- Performance monitoring
- State tracking
- Error reporting

### Documentation

#### User Documentation
- Installation instructions
- Usage guidelines
- Command reference
- Troubleshooting guide

#### Developer Documentation
- Architecture overview
- Build instructions
- Debug information
- Testing procedures

### Known Issues
- Stream state occasionally stuck in CONNECTING
- Audio format negotiation sometimes requires retry
- Resource cleanup needs improvement
- Error recovery could be more robust

### Future Plans
- Implement volume control
- Add mute functionality
- Improve error recovery
- Enhance performance monitoring
- Add stress testing
- Implement proper error recovery strategies
- Add resource monitoring for multiple streams 
