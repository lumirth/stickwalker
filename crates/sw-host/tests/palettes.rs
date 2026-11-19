use host::{DeviceSettings, PaletteName, palette, rgb565};

#[test]
fn interoperable_palette_text() {
    let amber = palette::parse("Amber|F7E2AF|764200").unwrap();
    assert!(amber.two_color);
    assert_eq!(amber.name.unwrap().as_str(), "Amber");
    assert_eq!(amber.colors[0], rgb565(0xf7e2af));
    assert_eq!(amber.colors[3], rgb565(0x764200));
    // Linear-light thirds are brighter than byte-space interpolation.
    assert_eq!(
        palette::blend_endpoints(0xffffff, 0),
        [0xffffff, 0xd5d5d5, 0x9c9c9c, 0]
    );
    let named = palette::parse("Pea Soup|FFFFFF|AAAAAA|555555|000000").unwrap();
    let raw = palette::parse("#FFFFFF AAAAAA #555555 000000").unwrap();
    assert_eq!(named.colors, raw.colors);
    assert!(raw.name.is_none());
    let set = palette::parse_many(
        "# My palettes\nAmber|F7E2AF|764200\n\n#FFFFFF #AAAAAA #555555 #000000",
    )
    .unwrap();
    assert_eq!(set.len(), 2);
    assert!(palette::parse_many(&"Amber|F7E2AF|764200\n".repeat(7)).is_err());
    assert!(
        format!(
            "{:#}",
            palette::parse_many("Amber|F7E2AF|764200\nbroken")
                .err()
                .unwrap()
        )
        .contains("Line 2")
    );
    assert!(PaletteName::new("123456789012345").is_ok());
    assert!(PaletteName::new("this name is too long").is_err());
    assert!(PaletteName::new("Pea|Soup").is_err());
    let mut settings = DeviceSettings::default();
    settings.slots[5].name = PaletteName::new("Pea Soup").unwrap();
    settings.slots[5].colors = named.colors;
    assert_eq!(
        DeviceSettings::from_bytes(&settings.to_bytes()).unwrap(),
        settings
    );
}

#[test]
fn ramps_round_trip_and_bad_hex_lines_are_not_comments() {
    let marked = palette::parse("\u{feff}# Blue|FFFFFF|000000").unwrap();
    assert_eq!(marked.name.unwrap().as_str(), "# Blue");
    let set = palette::parse_many("\u{feff}# Use Name|BG|INK\r\n# Blue|FFFFFF|000000\r\n").unwrap();
    assert_eq!(set.len(), 1);
    assert_eq!(set[0].name.unwrap().as_str(), "# Blue");
    let ramp = palette::parse("Paper|FFFFFF|000000").unwrap();
    // Published sRGB transfer curve at 2/3 and 1/3 luminance: D5 and 9C.
    assert_eq!(ramp.colors, [0xffff, 0xd6ba, 0x9cf3, 0]);
    let slot = host::PaletteSlot {
        name: PaletteName::new("Paper").unwrap(),
        colors: ramp.colors,
    };
    assert_eq!(palette::format(&slot), "Paper|FFFFFF|000000");
    let custom = host::PaletteSlot {
        colors: [0xffff, 0xf800, 0x07e0, 0],
        ..slot
    };
    assert_eq!(
        palette::format(&custom),
        "Paper|FFFFFF|FF0000|00FF00|000000"
    );
    for line in [
        "#FFFFFF #ZZZZZZ #555555 #000000",
        "#FFFFFF|AAAAAA|555555|00000G",
    ] {
        let error = palette::parse_many(&format!("# My set\n{line}\nAmber|F7E2AF|764200"))
            .err()
            .unwrap();
        assert!(format!("{error:#}").contains("Line 2"));
    }
}
