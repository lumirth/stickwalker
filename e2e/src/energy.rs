//! Pocket workload accounting. Coefficient ranges and supply assumptions: README.
use crate::{Motion, Pm, Rig};
use esp_soc::SocBus;

#[derive(Clone, Copy, Default)]
pub struct Components {
    imu_ma_cycles: f64,
    panel_cycles: u64,
    amp_cycles: u64,
    boost_cycles: u64,
}

impl Components {
    pub fn advance(&mut self, cycles: u64, pm: &Pm, imu: &Motion) {
        let ma = if !imu.accelerometer {
            0.0035
        } else if !imu.advanced_power_save {
            0.210
        } else {
            match imu.rate {
                0..=25 => 0.010,
                26..=50 => 0.017,
                _ => 0.0555,
            }
        };
        self.imu_ma_cycles += cycles as f64 * ma;
        if pm.rail() {
            self.panel_cycles += cycles;
        }
        if pm.amp_on() {
            self.amp_cycles += cycles;
        }
        if pm.boost_started.is_some() {
            self.boost_cycles += cycles;
        }
    }
}

pub struct Sample {
    time: u64,
    sleep: u64,
    memory_off: u64,
    wakes: u64,
    active: [u64; 3],
    imu_bytes: u64,
    pm_transactions: u64,
    components: Components,
}

pub struct Budget {
    pub wakes: u64,
    pub active_fraction: f64,
    pub memory_off_fraction: f64,
    pub low_ma: f64,
    pub high_ma: f64,
    pub row: String,
}

impl Sample {
    pub fn take(rig: &Rig) -> Self {
        Self {
            time: rig.machine.bus.cycles(),
            sleep: rig.machine.bus.sleep_residency(),
            memory_off: rig.machine.bus.vdd_spi_off_residency(),
            wakes: rig.machine.bus.sleep_entries,
            active: rig.machine.bus.active_residency,
            imu_bytes: rig.imu.borrow().bytes,
            pm_transactions: rig.pm.borrow().i2c_transactions,
            components: *rig.energy.borrow(),
        }
    }

    pub fn finish(self, rig: &Rig, name: &str) -> Result<Budget, String> {
        let end = Self::take(rig);
        let c = end.components;
        if c.panel_cycles != self.components.panel_cycles
            || c.amp_cycles != self.components.amp_cycles
            || c.boost_cycles != self.components.boost_cycles
        {
            return Err("Pocket workload left the display, amplifier or IR boost powered".into());
        }
        let time = (end.time - self.time) as f64;
        let sleep = (end.sleep - self.sleep) as f64 / time;
        let memory_off = (end.memory_off - self.memory_off) as f64 / time;
        let active =
            std::array::from_fn::<_, 3, _>(|i| (end.active[i] - self.active[i]) as f64 / time);
        let imu = (c.imu_ma_cycles - self.components.imu_ma_cycles) / time;
        // CPU duration already includes I2C waits, RTC calibration and resume.
        // Adding per-wake CPU charge again would double-count those costs.
        // Charge standby memory only while its shared supply is actually enabled.
        // Board overhead is separate: it is not an observed software cost.
        let estimate = |cpu: [f64; 3], memory: f64| {
            let core: f64 = active.iter().zip(cpu).map(|(t, ma)| t * ma).sum();
            (core + sleep * 0.240 + imu + memory * (1.0 - memory_off)) * 3.3 / (3.7 * 0.90)
        };
        let low_ma = estimate([10.0, 22.0, 51.2], 0.152);
        let high_ma = estimate([25.0, 36.1, 65.9], 0.180);
        let wakes = end.wakes - self.wakes;
        let active_fraction = 1.0 - sleep;
        let row = format!(
            "{name},{:.3},{wakes},{:.6},{},{},{memory_off:.6},{low_ma:.4},{high_ma:.4}\n",
            time / 240_000_000.0,
            active_fraction,
            end.imu_bytes - self.imu_bytes,
            end.pm_transactions - self.pm_transactions
        );
        Ok(Budget {
            wakes,
            active_fraction,
            memory_off_fraction: memory_off,
            low_ma,
            high_ma,
            row,
        })
    }
}

pub const HEADER: &str = "state,seconds,wakes,active_fraction,imu_bytes,pm_transactions,memory_off_fraction,components_low_ma,components_high_ma\n";
