use anyhow::{Context, Result};
use host::{protocol, release, save, usb};
use std::{
    env, fs,
    io::{Read, Write},
    path::Path,
};
fn main() -> Result<()> {
    let a: Vec<String> = env::args().collect();
    match a.get(1).map(String::as_str).unwrap_or("list") {
        "list" => {
            for p in usb::ports()? {
                println!("{} {}", p.path, p.serial);
            }
        }
        "decode" => {
            let bytes = if a[2] == "-" {
                let mut bytes = Vec::new();
                std::io::stdin().read_to_end(&mut bytes)?;
                bytes
            } else {
                fs::read(&a[2])?
            };
            let (image, runtime) = if bytes.len() == save::FLASH {
                save::decode_current(&bytes[save::WALKER..])?
            } else {
                save::decode_current(&bytes)?
            };
            if let Some(path) = a.get(3) {
                fs::write(path, image)?;
            } else {
                std::io::stdout().write_all(&image)?;
                std::io::stdout().write_all(&runtime)?;
            }
        }
        "decode-public" => {
            let bytes = fs::read(&a[2])?;
            fs::write(&a[3], save::decode_public(&bytes)?)?;
        }
        op => {
            let p = usb::ports()?
                .into_iter()
                .find(|p| a.get(2) == Some(&p.path))
                .context("Choose a connected port")?;
            if op == "install" {
                let package = release::Package::open(Path::new(&a[3]))?;
                let session = protocol::Session::open(&p.path)
                    .ok()
                    .and_then(|mut s| usb::Info::read(&mut s).ok().map(|_| s));
                let (_, i) =
                    host::flash::install(&p, session, &package, false, &mut |s, p| match p {
                        Some(v) => println!("{s} · {v}%"),
                        None => println!("{s}"),
                    })?;
                println!("Ready: {} {}", i.mac, i.version);
            } else {
                let mut s = protocol::Session::open(&p.path)?;
                let info = usb::Info::read(&mut s)?;
                match op {
                    "info" => println!(
                        "{} {} build={} mode={:?} battery={}mV preferences={:?}",
                        info.mac,
                        info.version,
                        info.build,
                        info.mode,
                        info.millivolts,
                        info.settings
                    ),
                    "export" => {
                        fs::write(&a[3], usb::read_eeprom(&mut s)?)?;
                    }
                    "import" => {
                        let bytes = save::read_eeprom(Path::new(&a[3]))?;
                        let start = std::time::Instant::now();
                        usb::write_eeprom(&mut s, &bytes)?;
                        println!("Imported EEPROM in {:?}", start.elapsed());
                    }
                    "palette" => {
                        let mut prefs = info.settings;
                        prefs.active_palette = a[3].parse()?;
                        s.request(protocol::SETTINGS, &prefs.to_bytes())?;
                    }
                    _ => anyhow::bail!("Unknown operation"),
                }
            }
        }
    }
    Ok(())
}
