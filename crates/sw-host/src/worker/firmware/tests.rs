use super::*;
use crate::{
    release::{FEED, hash},
    usb::Port,
    worker::{ActiveDevice, Command, HardwareOperation, Presentation, WorkerState},
};
use std::{
    collections::BTreeMap,
    fs,
    sync::Arc,
    time::{Duration, Instant},
};

// A release server response with an ESP application descriptor and partition table.
fn assets(version: &str) -> BTreeMap<String, Vec<u8>> {
    let mut app = vec![0; 208];
    app[0] = 0xe9;
    app[32..36].copy_from_slice(&0xabcd5432u32.to_le_bytes());
    app[48..48 + version.len()].copy_from_slice(version.as_bytes());
    let mut table = Vec::new();
    for (name, at, size) in [
        ("nvs", 0x9000u32, 0x6000u32),
        ("factory", 0x10000, 0x200000),
        ("walker2", 0x610000, 0x1f0000),
    ] {
        let mut row = [0; 32];
        row[..2].copy_from_slice(&[0xaa, 0x50]);
        row[4..8].copy_from_slice(&at.to_le_bytes());
        row[8..12].copy_from_slice(&size.to_le_bytes());
        row[12..12 + name.len()].copy_from_slice(name.as_bytes());
        table.extend(row);
    }
    let mut files = BTreeMap::new();
    let mut images = Vec::new();
    for (kind, name, offset, bytes) in [
        ("application", "stickwalker.bin", 0x10000, app),
        ("bootloader", "bootloader.bin", 0, vec![0xe9]),
        ("partitions", "partition-table.bin", 0x8000, table),
    ] {
        images.push(serde_json::json!({"kind": kind, "file": name, "offset": offset, "sha256": hash(&bytes)}));
        files.insert(name.into(), bytes);
    }
    files.insert("firmware.json".into(), serde_json::to_vec(&serde_json::json!({
        "schema": 1, "product": "StickS3", "version": version, "storage": "SWK6", "images": images,
        "settings": {"offset": 0x9000, "size": 0x6000}, "state": {"offset": 0x610000, "size": 0x1f0000}
    })).unwrap());
    files
}

fn release(version: &str) -> Package {
    let mut files = assets(version);
    let base =
        format!("https://github.com/lumirth/stickwalker/releases/download/firmware-v{version}/");
    Package::fetch(None, |url, _| {
        let name = if url == FEED {
            "firmware.json"
        } else {
            url.strip_prefix(&base).unwrap()
        };
        Ok(files.remove(name).unwrap())
    })
    .unwrap()
    .unwrap()
}

#[test]
fn offline_restart_uses_the_newest_complete_package() {
    let dir = tempfile::tempdir().unwrap();
    let cache = dir.path();
    for version in ["0.1.9", "0.1.10"] {
        release(version).store(cache).unwrap();
    }
    let cached = Package::cached(cache).unwrap();
    assert_eq!(cached.manifest.version, Version::new(0, 1, 10));
    for version in ["0.1.10", "0.1.9"] {
        let result = Package::fetch(Some(&cached.manifest.version), |url, _| {
            assert_eq!(
                url, FEED,
                "an unchanged or older release must not fetch images"
            );
            Ok(assets(version).remove("firmware.json").unwrap())
        });
        assert!(result.unwrap().is_none());
    }

    let mut damaged = assets("0.1.11");
    damaged.get_mut("stickwalker.bin").unwrap().truncate(32);
    let (wake, inbox) = mpsc::channel();
    let mut updates = FirmwareUpdates::new(Some(cache.into()));
    updates.request(Request::Check, &wake, |current| {
        Package::fetch(current, move |url, _| {
            Ok(damaged.remove(url.rsplit('/').next().unwrap()).unwrap())
        })
    });
    inbox.recv_timeout(Duration::from_secs(5)).unwrap();
    let Some((Request::Check, Err(error))) = updates.finish() else {
        panic!("accepted a damaged firmware image");
    };
    assert!(error.to_string().contains("stickwalker.bin"));
    assert_eq!(updates.release_version(), Some(&cached.manifest.version));
    assert!(!cache.join("0.1.11").exists());

    fs::write(cache.join("0.1.10/stickwalker.bin"), b"partial").unwrap();
    assert_eq!(
        Package::cached(cache).unwrap().manifest.version,
        Version::new(0, 1, 9)
    );
    release("0.1.10").store(cache).unwrap();
    assert_eq!(
        Package::cached(cache).unwrap().manifest.version,
        Version::new(0, 1, 10)
    );
}

#[test]
fn opening_local_firmware_during_a_check_keeps_it_selected() {
    let dir = tempfile::tempdir().unwrap();
    let cache = dir.path().join("cache");
    let files = dir.path().join("local");
    release("0.1.3").store(&files).unwrap();
    let (wake, inbox) = mpsc::channel();
    let mut firmware = FirmwareUpdates::new(Some(cache.clone()));
    firmware.request(Request::Background, &wake, |_| Ok(Some(release("0.2.0"))));
    firmware.open(&files.join("0.1.3/firmware.json")).unwrap();
    firmware.request(Request::Check, &wake, |_| panic!("duplicate download"));
    inbox.recv_timeout(Duration::from_secs(5)).unwrap();
    assert!(matches!(firmware.finish(), Some((Request::Check, Ok(())))));
    assert_eq!(
        firmware.selected().unwrap().manifest.version,
        Version::new(0, 1, 3)
    );
    assert!(matches!(firmware.selection(), Some(Firmware::Local(_))));
    assert_eq!(
        FirmwareUpdates::new(Some(cache)).release_version(),
        Some(&Version::new(0, 2, 0))
    );
}

#[test]
fn download_feedback_depends_on_who_requested_it() {
    let (wake, inbox) = mpsc::channel();
    let (events, _ui) = async_channel::unbounded();
    let busy = Arc::default();
    let port = Port {
        path: "COM4".into(),
        serial: "30EDA0001A20".into(),
    };
    let mut worker = WorkerState {
        active: ActiveDevice::Unmanaged(port),
        firmware: FirmwareUpdates::new(None),
        state: Presentation::default(),
        last_sent: None,
        discover: Instant::now(),
        poll: Instant::now(),
        events: &events,
        wake: &wake,
        busy: &busy,
    };
    worker.publish();
    let target = worker.state.connection.target();
    fn offline(_: Option<&Version>) -> Result<Option<Package>> {
        anyhow::bail!("offline")
    }
    worker.firmware.request(Request::Background, &wake, offline);
    inbox.recv_timeout(Duration::from_secs(5)).unwrap();
    worker.firmware_ready();
    assert_eq!(worker.state.error, None);
    assert_eq!(worker.state.firmware_feedback, None);

    worker.firmware.request(Request::Background, &wake, offline);
    worker.execute(Command::CheckFirmware).unwrap();
    inbox.recv_timeout(Duration::from_secs(5)).unwrap();
    worker.firmware_ready();
    assert_eq!(
        worker.state.error.as_deref(),
        Some("Couldn’t check for firmware updates.")
    );

    let install = || {
        Box::new(Work {
            command: Command::Update,
            target: target.clone(),
            operation: Some(HardwareOperation::begin(&busy).unwrap()),
        })
    };
    worker.firmware.request(Request::Background, &wake, offline);
    worker.handle_command(install());
    assert_eq!(
        worker.state.status.as_deref(),
        Some("Downloading firmware…")
    );
    inbox.recv_timeout(Duration::from_secs(5)).unwrap();
    assert!(worker.firmware_ready().is_none());
    assert_eq!(
        worker.state.error.as_deref(),
        Some(
            "Couldn’t download firmware. Check your internet connection or open a local firmware file."
        )
    );

    worker
        .firmware
        .request(Request::Background, &wake, |_| Ok(Some(release("0.1.0"))));
    worker.handle_command(install());
    inbox.recv_timeout(Duration::from_secs(5)).unwrap();
    let retry = worker.firmware_ready().unwrap();
    worker.active = ActiveDevice::Disconnected;
    worker.publish();
    worker.handle_command(retry);
    worker.publish();
    assert_eq!(
        worker.state.error.as_deref(),
        Some("The Stick is no longer available for this action.")
    );
    assert!(!worker.state.hardware_busy);
}
