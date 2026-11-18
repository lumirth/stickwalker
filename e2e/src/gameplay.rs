//! Drive the compiled game through its menus and inspect visible game outcomes.
use crate::Rig;
use emu_core::Bus;
use esp_soc::SocBus;
use std::{error::Error, fs, path::Path};
type Result<T> = std::result::Result<T, Box<dyn Error>>;

fn address(rig: &Rig, name: &str) -> Result<u32> {
    rig.machine
        .symbols
        .iter()
        .find(|(_, value)| value.as_str() == name)
        .map(|(&address, _)| address)
        .ok_or_else(|| format!("Missing {name}").into())
}
fn byte(rig: &mut Rig, at: u32) -> Result<u8> {
    rig.machine
        .bus
        .read8(at)
        .map_err(|e| format!("{e:?}").into())
}
fn half(rig: &mut Rig, at: u32) -> Result<u16> {
    rig.machine
        .bus
        .read16(at)
        .map_err(|e| format!("{e:?}").into())
}
fn menu(rig: &mut Rig, state: u32, selection: u8) -> Result<()> {
    rig.press("side")?;
    rig.run(600)?;
    if byte(rig, state + 49)? != 1 {
        return Err("Home did not open the game menu".into());
    }
    for _ in 0..selection {
        rig.press("side")?;
    }
    rig.run(600)?;
    rig.center()?;
    rig.run(600)?;
    Ok(())
}

pub fn inventory(rig: &mut Rig, output: &Path) -> Result<()> {
    let state = address(rig, "g_state")?;
    let eeprom = address(rig, "eeprom")?;
    if half(rig, eeprom + 0xce80 + 12)? == 0 || half(rig, eeprom + 0xce80 + 60)? == 0 {
        return Err("The captured Pokemon or Dowsing item was lost on cold boot".into());
    }
    menu(rig, state, 4)?;
    if byte(rig, state + 49)? != 10 {
        return Err("Inventory did not open after cold boot".into());
    }
    rig.snapshot(output, "inventory")?;
    Ok(())
}

pub fn run(rig: &mut Rig, output: &Path) -> Result<()> {
    let state = address(rig, "g_state")?;
    let view = address(rig, "g_ui")? + 8;
    let eeprom = address(rig, "eeprom")?;
    let daily = rig
        .machine
        .bus
        .read32(state + 28)
        .map_err(|e| format!("{e:?}"))?;
    let threshold = half(rig, eeprom + 0x8f00 + 130)?;
    if threshold == 0 || daily < u32::from(threshold) {
        return Err("Gameplay fixture must qualify for a real nonzero Group A threshold".into());
    }
    // Brightness belongs to the original game settings, including across the host overlay.
    menu(rig, state, 5)?;
    rig.press("side")?; // Select Shade.
    rig.center()?;
    let before = rig.machine.bus.pwm_output(38);
    rig.press("side")?;
    if rig.machine.bus.pwm_output(38) == before { rig.press("power")?; }
    let shade = rig.machine.bus.pwm_output(38);
    if shade == before { return Err("Game Shade did not change the physical backlight PWM".into()); }
    rig.snapshot(output, "game-shade")?;
    rig.center()?; // Save and return Home.
    rig.open_settings()?;
    rig.press("power")?; // Back to game, wrapping from Controls.
    rig.center()?;
    if rig.machine.bus.pwm_output(38) != shade {
        return Err("Host Settings overrode the game's Shade".into());
    }
    let watts = half(rig, state + 14)?;
    menu(rig, state, 1)?;
    if byte(rig, state + 49)? != 2 || half(rig, state + 14)? != watts - 3 {
        return Err("Dowsing did not charge three Watts and open".into());
    }
    for _ in 0..6 {
        if byte(rig, view + 3)? == byte(rig, view + 5)? {
            break;
        }
        rig.press("side")?;
    }
    rig.center()?;
    rig.run(2000)?;
    if byte(rig, view + 1)? != 2 {
        return Err("The correct Dowsing patch did not reveal an item".into());
    }
    let item = half(rig, view + 10)?;
    let item_index = byte(rig, view)?;
    rig.snapshot(output, "dowsing")?;
    rig.center()?;
    rig.run(600)?;
    if byte(rig, state + 49)? != 0 || half(rig, eeprom + 0xce80 + 60)? != item || item == 0 {
        return Err("Dowsing reward did not reach the inventory".into());
    }
    if item_index >= 9 {
        return Err("High-step Dowsing selected only the zero-step fallback".into());
    }

    let mut group_a = false;
    let mut captured = false;
    let mut searches = 0;
    while searches < 12 && !(group_a && captured) {
        searches += 1;
        let watts = half(rig, state + 14)?;
        menu(rig, state, 0)?;
        if byte(rig, state + 49)? != 3 || half(rig, state + 14)? != watts - 10 {
            return Err("Radar did not charge ten Watts and open".into());
        }
        for _ in 0..480 {
            if byte(rig, state + 49)? != 3 {
                break;
            }
            if byte(rig, view + 1)? == 0 {
                if byte(rig, view + 2)? == byte(rig, view + 5)? {
                    rig.center()?;
                } else {
                    rig.press("side")?;
                }
            } else {
                rig.run(63)?;
            }
        }
        if byte(rig, state + 49)? != 4 {
            return Err("Radar did not reach battle".into());
        }
        group_a |= byte(rig, view)? == 1;
        rig.snapshot(output, "battle")?;
        for _ in 0..2400 {
            if byte(rig, state + 49)? != 4 {
                break;
            }
            let phase = byte(rig, view + 1)?;
            if phase == 2 {
                if byte(rig, view + 3)? > 2 {
                    rig.press("power")?;
                } else {
                    rig.center()?;
                }
            } else if matches!(phase, 6 | 7 | 9 | 16)
                || phase == 1 && byte(rig, view + 4)? >= byte(rig, view + 5)?
            {
                rig.center()?;
            } else {
                rig.run(63)?;
            }
        }
        if byte(rig, state + 49)? != 0 {
            return Err("Battle did not return Home".into());
        }
        captured = half(rig, eeprom + 0xce80 + 12)? != 0;
    }
    if !group_a || !captured {
        return Err("Group A encounter and capture were not reached".into());
    }
    menu(rig, state, 4)?;
    if byte(rig, state + 49)? != 10 {
        return Err("Inventory did not open".into());
    }
    rig.snapshot(output, "inventory")?;
    fs::write(output.join("gameplay.txt"), format!("daily_steps={daily}\ngroup_a_threshold={threshold}\nradar_searches={searches}\ndowsing_item={item}\ndowsing_tier={item_index}\ncaptured_species={}\n", half(rig, eeprom + 0xce80 + 12)?))?;
    Ok(())
}
