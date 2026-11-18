use std::{env, fs, path::PathBuf};

fn main() {
    const FILES: [&str; 4] = [
        "firmware.json",
        "bootloader.bin",
        "partition-table.bin",
        "stickwalker.bin",
    ];
    let root =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("../../dist/firmware");
    println!("cargo:rerun-if-env-changed=STICKWALKER_BUNDLE_FIRMWARE");
    let required = env::var("STICKWALKER_BUNDLE_FIRMWARE").as_deref() == Ok("1");
    let mut complete = true;
    for name in FILES {
        let path = root.join(name);
        println!("cargo:rerun-if-changed={}", path.display());
        if !path.metadata().is_ok_and(|m| m.is_file() && m.len() > 0) {
            assert!(
                !required,
                "Required firmware file is missing or empty: {}",
                path.display()
            );
            complete = false;
        }
    }
    let mut source = String::from("&[");
    if complete {
        for name in FILES {
            let path = root.join(name);
            source.push_str(&format!("({name:?}, include_bytes!({path:?}) as &[u8]),"));
        }
    }
    source.push(']');
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    fs::write(out.join("firmware.rs"), source).unwrap();
}
