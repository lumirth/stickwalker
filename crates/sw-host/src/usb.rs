use crate::{
    DeviceSettings,
    protocol::{self, Session},
    save,
};
use anyhow::{Context, Result, ensure};
use serialport::SerialPortType;
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Port {
    pub path: String,
    pub serial: String,
}
pub(crate) fn mac_id(serial: &str) -> Option<String> {
    let mac = serial.replace([':', '-'], "");
    (mac.len() == 12 && mac.bytes().all(|b| b.is_ascii_hexdigit()))
        .then(|| mac.to_ascii_uppercase())
}
impl Port {
    pub(crate) fn same_device(&self, other: &Self) -> bool {
        match (mac_id(&self.serial), mac_id(&other.serial)) {
            (Some(a), Some(b)) => a == b,
            _ => self == other,
        }
    }
    pub fn label(&self, ports: &[Port]) -> String {
        self.label_with_mac(ports, None)
    }
    pub(crate) fn label_with_mac(&self, ports: &[Port], active: Option<(&Port, &str)>) -> String {
        let suffix = |port: &Port| {
            active
                .filter(|(selected, _)| selected == &port)
                .and_then(|(_, reported)| mac_id(reported))
                .or_else(|| mac_id(&port.serial))
                .map(|mac| mac[8..].to_owned())
        };
        let path = std::path::Path::new(&self.path)
            .file_name()
            .unwrap_or_default()
            .to_string_lossy();
        match suffix(self) {
            Some(id)
                if ports
                    .iter()
                    .any(|p| p != self && suffix(p).as_ref() == Some(&id)) =>
            {
                format!("Stick {id} ({path})")
            }
            Some(id) => format!("Stick {id}"),
            None => format!("Stick {path}"),
        }
    }
}
pub fn ports() -> Result<Vec<Port>> {
    let mut ports: Vec<_> = serialport::available_ports()?
        .into_iter()
        .filter_map(|p| match p.port_type {
            SerialPortType::UsbPort(u)
                if u.vid == 0x303a
                    && u.pid == 0x1001
                    && !(cfg!(target_os = "macos") && p.port_name.starts_with("/dev/tty.")) =>
            {
                Some(Port {
                    path: p.port_name,
                    serial: u.serial_number.unwrap_or_default(),
                })
            }
            _ => None,
        })
        .collect();
    ports.sort_by(|a, b| (&a.serial, &a.path).cmp(&(&b.serial, &b.path)));
    Ok(ports)
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeviceMode {
    Ready,
    Linking,
    SaveFault,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Info {
    pub version: String,
    pub build: String,
    pub mac: String,
    pub mode: DeviceMode,
    pub millivolts: u16,
    pub settings: DeviceSettings,
    pub power: u8,
}
impl Info {
    pub fn battery_percent(&self) -> u32 {
        let scale = [
            (3300, 0),
            (3600, 10),
            (3720, 30),
            (3820, 55),
            (3990, 80),
            (4200, 100),
        ];
        let mv = u32::from(self.millivolts);
        if mv >= 4200 {
            100
        } else {
            scale
                .windows(2)
                .find(|w| mv >= w[0].0 && mv < w[1].0)
                .map(|w| w[0].1 + (mv - w[0].0) * (w[1].1 - w[0].1) / (w[1].0 - w[0].0))
                .unwrap_or(0)
        }
    }

    pub fn read(session: &mut Session) -> Result<Self> {
        let bytes = session.request(protocol::STATUS, &[])?;
        let base = 74 + crate::settings::SETTINGS_SIZE;
        ensure!(bytes.len() == base, "Unsupported device status");
        let mut remaining = bytes.as_slice();
        let mut take = |size| {
            let (field, rest) = remaining.split_at(size);
            remaining = rest;
            field
        };
        let version = String::from_utf8_lossy(take(32))
            .trim_end_matches('\0')
            .to_string();
        let build = take(32).iter().map(|b| format!("{b:02x}")).collect();
        let mac = take(6)
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect::<Vec<_>>()
            .join(":");
        let mode = match take(1)[0] {
            0 => DeviceMode::Ready,
            1 => DeviceMode::Linking,
            2 => DeviceMode::SaveFault,
            _ => anyhow::bail!("Unsupported device mode"),
        };
        let millivolts = u16::from_le_bytes(take(2).try_into().unwrap());
        let power = take(1)[0];
        let settings = DeviceSettings::from_bytes(take(crate::settings::SETTINGS_SIZE))?;
        Ok(Self {
            version,
            build,
            mac,
            mode,
            millivolts,
            settings,
            power,
        })
    }
}

pub fn read_eeprom(session: &mut Session) -> Result<Vec<u8>> {
    let mut bytes = Vec::with_capacity(save::EEPROM);
    while bytes.len() < save::EEPROM {
        let count = (save::EEPROM - bytes.len()).min(protocol::CHUNK);
        let mut request = (bytes.len() as u32).to_le_bytes().to_vec();
        request.extend((count as u16).to_le_bytes());
        let page = session
            .request(protocol::READ, &request)
            .with_context(|| format!("Could not export EEPROM at byte {}", bytes.len()))?;
        ensure!(page.len() == count, "Incomplete EEPROM export");
        bytes.extend(page);
    }
    save::validate_eeprom(&bytes)?;
    Ok(bytes)
}

pub fn write_eeprom(session: &mut Session, image: &save::EepromImage) -> Result<()> {
    for (index, chunk) in image.bytes().chunks(protocol::CHUNK).enumerate() {
        let mut page = ((index * protocol::CHUNK) as u32).to_le_bytes().to_vec();
        page.extend(chunk);
        session.request(protocol::WRITE, &page)?;
    }
    Ok(())
}

pub fn reset_walker(session: &mut Session) -> Result<()> {
    session.request(protocol::WRITE, &[])?;
    Ok(())
}
