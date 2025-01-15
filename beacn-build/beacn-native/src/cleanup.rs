use napi_derive::napi;

#[napi]
pub fn cleanup_virtual_devices() -> bool {
    // Get list of BEACN Link module IDs
    let output = std::process::Command::new("pactl")
        .args(["list", "short", "modules"])
        .output()
        .expect("Failed to execute pactl");

    let modules = String::from_utf8_lossy(&output.stdout);
    
    // Find and unload BEACN Link modules
    for line in modules.lines() {
        if line.contains("beacn_link_") {
            if let Some(module_id) = line.split('\t').next() {
                let _ = std::process::Command::new("pactl")
                    .args(["unload-module", module_id])
                    .output();
            }
        }
    }
    true
}
