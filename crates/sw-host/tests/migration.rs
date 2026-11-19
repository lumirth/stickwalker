use host::{protocol, save};
use std::{fs, io::Read, path::PathBuf};

fn fixture(name: &str) -> Vec<u8> {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../e2e/fixtures")
        .join(name);
    let bytes = fs::read(path).unwrap();
    let mut output = Vec::new();
    flate2::read::ZlibDecoder::new(bytes.as_slice())
        .read_to_end(&mut output)
        .unwrap();
    output
}

#[test]
fn shipped_storage_formats_recover_the_last_committed_edits() {
    for (input, expected) in [
        ("public-raw.bin.zlib", "raw-expected.eeprom.zlib"),
        ("public-littlefs.bin.zlib", "littlefs-expected.eeprom.zlib"),
    ] {
        let save::PublicPartition::StickwalkerSave(image) =
            save::classify_public_partition(&fixture(input))
        else {
            panic!("Historical fixture was not recognized");
        };
        assert_eq!(image.bytes(), fixture(expected));
    }
    let (image, _) = save::decode_current(&fixture("current.bin.zlib")).unwrap();
    assert_eq!(image, fixture("current-expected.eeprom.zlib"));
}

#[test]
fn damaged_banks_do_not_resurrect_the_retired_import() {
    let mut flash = fixture("current.bin.zlib");
    for base in [0x10000, 0xf0000] {
        flash[base + 64] ^= 1;
    }
    assert!(save::decode_current(&flash).is_err());
}

#[test]
fn serial_stream_recovers_after_boot_noise_truncation_and_corruption() {
    let good = protocol::frame(1, 71, &[0, 0xc0, 0xdb, 11]);
    let mut stream = b"ESP-ROM boot log\r\n".to_vec();
    stream.extend(&good[..good.len() / 2]);
    stream.extend([0xc0, 0xdb, 0x22, 0xc0]);
    let mut bad = good.clone();
    bad[4] ^= 1;
    stream.extend(bad);
    stream.extend(&good);
    let mut decoder = protocol::Decoder::default();
    let packets: Vec<_> = stream.into_iter().filter_map(|b| decoder.push(b)).collect();
    assert_eq!(packets.len(), 1);
    assert_eq!(&packets[0][8..12], &[0, 0xc0, 0xdb, 11]);
}

#[test]
fn public_layout_distinguishes_factory_data_from_damaged_walks() {
    use save::PublicPartition::*;
    assert!(matches!(
        save::classify_public_partition(&vec![0xff; 0x180000]),
        EmptyOrForeign
    ));
    assert!(matches!(
        save::classify_public_partition(&fixture("empty-littlefs.bin.zlib")),
        EmptyOrForeign
    ));
    let mut unreadable = fixture("public-littlefs.bin.zlib");
    unreadable[..8192].fill(0xff);
    assert!(matches!(
        save::classify_public_partition(&unreadable),
        DamagedSave(_)
    ));
    for (name, marker, header) in [
        ("public-raw.bin.zlib", 0x53574531u32, 28),
        ("public-littlefs.bin.zlib", 0x50574531u32, 12),
    ] {
        let mut bytes = fixture(name);
        assert!(matches!(
            save::classify_public_partition(&bytes),
            StickwalkerSave(_)
        ));
        let offset = bytes
            .windows(4)
            .position(|w| w == marker.to_le_bytes())
            .unwrap();
        bytes[offset + header + 100] ^= 0x40;
        assert!(
            matches!(save::classify_public_partition(&bytes), DamagedSave(_)),
            "{name}"
        );
    }
}

#[test]
fn missing_littlefs_entries_do_not_make_a_checkpoint_safe_to_erase() {
    let mut image = fixture("public-littlefs.bin.zlib");
    image[0x1004] ^= 1;
    assert!(matches!(
        save::classify_public_partition(&image),
        save::PublicPartition::DamagedSave(_)
    ));
}

#[test]
fn legacy_preferences_keep_controls_and_acquire_named_slots() {
    use host::{DeviceSettings, PALETTE_NAMES, legacy};
    let defaults = legacy::preferences(&vec![0xff; 0x5000]);
    assert_eq!(
        (
            defaults.active_palette.index(),
            defaults.reverse,
            defaults.layout
        ),
        (2, false, 1)
    );
    assert_eq!(defaults.mappings, [1, 0, 1, 1]);
    assert_eq!(defaults.slots[2].colors[0], 0);
    assert_eq!(defaults.slots[2].colors[3], 0xffff);
    for (slot, name) in defaults.slots.iter().zip(PALETTE_NAMES) {
        assert_eq!(slot.name.as_str(), name);
    }
    assert_eq!(
        DeviceSettings::from_bytes(&defaults.to_bytes()).unwrap(),
        defaults
    );

    let captured = fixture("public-nvs.bin.zlib");
    assert_eq!(captured.len(), 0x5000);
    let settings = legacy::preferences(&captured);
    // Captured layout-v2 = 6, dark = 1, with older entries still in the page.
    assert!(!settings.reverse);
    assert_eq!(settings.layout, 1);
    assert_eq!(settings.mappings, [1, 0, 1, 1]);
    assert_eq!(settings.slots[2].colors, [0, 0x528a, 0xad75, 0xffff]);
}

#[test]
fn a_damaged_littlefs_checkpoint_only_blocks_a_newer_save() {
    let original = fixture("public-littlefs-pair.bin.zlib");
    for sequence in [7u32, 8] {
        let mut bytes = original.clone();
        let header = [0x50574531u32.to_le_bytes(), sequence.to_le_bytes()].concat();
        let at = bytes
            .windows(header.len())
            .position(|w| w == header)
            .unwrap();
        let mut bad_header = original.clone();
        bad_header[at] ^= 1;
        assert!(matches!(
            save::classify_public_partition(&bad_header),
            save::PublicPartition::DamagedSave(_)
        ));
        bytes[at + 12 + 128] ^= 1;
        let result = save::classify_public_partition(&bytes);
        if sequence == 7 {
            let save::PublicPartition::StickwalkerSave(latest) = result else {
                panic!("Lost the intact newer checkpoint")
            };
            assert_eq!(latest.bytes()[0x100], 8);
        } else {
            assert!(matches!(result, save::PublicPartition::DamagedSave(_)));
        }
    }
}

#[test]
fn eeprom_import_rejects_oversized_files_and_invalid_headers() {
    let file = tempfile::NamedTempFile::new().unwrap();
    file.as_file().set_len(1 << 30).unwrap();
    assert!(save::EepromImage::load(file.path()).is_err());
    file.as_file().set_len(save::EEPROM as u64).unwrap();
    assert!(save::EepromImage::load(file.path()).is_err());
    let bytes = fixture("current-expected.eeprom.zlib");
    fs::write(file.path(), &bytes).unwrap();
    assert_eq!(save::EepromImage::load(file.path()).unwrap().bytes(), bytes);
}
