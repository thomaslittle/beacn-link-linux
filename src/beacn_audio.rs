use napi_derive::napi;
use std::sync::{Arc, Mutex};
use libpulse_binding as pulse;
use pulse::context::Context;
use pulse::mainloop::standard::Mainloop;

#[napi]
pub struct AudioDevice {
    pub name: String,
    pub id: String,
    pub description: String,
    pub is_output: bool,
}

#[napi]
pub struct BeacnLink {
    pulse_context: Arc<Mutex<Option<Context>>>,
}

#[napi]
impl BeacnLink {
    #[napi(constructor)]
    pub fn new() -> Self {
        BeacnLink {
            pulse_context: Arc::new(Mutex::new(None)),
        }
    }

    #[napi]
    pub fn initialize(&mut self) -> bool {
        // Check if PulseAudio is running
        if !std::process::Command::new("pulseaudio")
            .args(["--check"])
            .status()
            .map_or(false, |status| status.success()) {
            eprintln!("PulseAudio is not running");
            return false;
        }

        let mainloop = match Mainloop::new() {
            Some(m) => m,
            None => return false,
        };

        let context = match Context::new(&mainloop, "BEACN Link") {
            Some(c) => c,
            None => return false,
        };

        *self.pulse_context.lock().unwrap() = Some(context);
        true
    }

    #[napi]
    pub fn get_audio_devices(&self) -> Vec<AudioDevice> {
        let mut devices = Vec::new();

        // Use pulseaudio command line to list sinks
        if let Ok(output) = std::process::Command::new("pactl")
            .args(["list", "short", "sinks"])
            .output() {
                if let Ok(output_str) = String::from_utf8(output.stdout) {
                    for line in output_str.lines() {
                        let parts: Vec<&str> = line.split('\t').collect();
                        if parts.len() >= 2 {
                            devices.push(AudioDevice {
                                name: parts[1].to_string(),
                                id: parts[0].to_string(),
                                description: parts.get(2).unwrap_or(&"").to_string(),
                                is_output: true,
                            });
                        }
                    }
                }
        }

        // Also list sources
        if let Ok(output) = std::process::Command::new("pactl")
            .args(["list", "short", "sources"])
            .output() {
                if let Ok(output_str) = String::from_utf8(output.stdout) {
                    for line in output_str.lines() {
                        let parts: Vec<&str> = line.split('\t').collect();
                        if parts.len() >= 2 {
                            devices.push(AudioDevice {
                                name: parts[1].to_string(),
                                id: parts[0].to_string(),
                                description: parts.get(2).unwrap_or(&"").to_string(),
                                is_output: false,
                            });
                        }
                    }
                }
        }

        devices
    }

    #[napi]
    pub fn create_virtual_output(&self, name: String) -> bool {
        // Create virtual output device using PulseAudio module-null-sink
        if let Some(_ctx) = self.pulse_context.lock().unwrap().as_ref() {
            let output = std::process::Command::new("pactl")
                .args([
                    "load-module",
                    "module-null-sink",
                    &format!("sink_name={}", name),
                    &format!("sink_properties=device.description=\"{}\"", name),
                ])
                .output();

            match output {
                Ok(out) => out.status.success(),
                Err(_) => false,
            }
        } else {
            false
        }
    }

    #[napi]
    pub fn route_audio(&self, source: String, destination: String) -> bool {
        if let Some(_ctx) = self.pulse_context.lock().unwrap().as_ref() {
            let output = std::process::Command::new("pactl")
                .args([
                    "load-module",
                    "module-loopback",
                    &format!("source={}", source),
                    &format!("sink={}", destination),
                ])
                .output();

            match output {
                Ok(out) => out.status.success(),
                Err(_) => false,
            }
        } else {
            false
        }
    }

    #[napi] 
    pub fn create_link_outputs(&self) -> bool {
        // Create the 4 BEACN Link outputs
        let output_names = [
            "BEACN_Link_Out",
            "BEACN_Link_2_Out", 
            "BEACN_Link_3_Out",
            "BEACN_Link_4_Out"
        ];

        for name in output_names.iter() {
            if !self.create_virtual_output(name.to_string()) {
                return false;
            }
        }

        true
    }
}
