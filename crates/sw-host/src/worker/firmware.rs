use super::{Firmware, Message};
use crate::release::{Manifest, Package};
use anyhow::Result;
use std::{
    path::PathBuf,
    sync::mpsc::{self, Receiver, Sender, TryRecvError},
    thread,
};

pub(super) enum Request {
    Background,
    Check,
    Install(async_channel::Sender<()>),
}

struct Download {
    explicit_check: bool,
    install: Option<async_channel::Sender<()>>,
    result: Receiver<Result<Option<Package>>>,
}

pub(super) struct FirmwareUpdates {
    cache: Option<PathBuf>,
    release: Option<Package>,
    download: Option<Download>,
}

impl FirmwareUpdates {
    pub fn new(cache: Option<PathBuf>) -> Self {
        let release = cache.as_deref().and_then(Package::cached);
        Self {
            cache,
            release,
            download: None,
        }
    }

    pub fn release(&self) -> Option<&Package> {
        self.release.as_ref()
    }

    pub fn selection(&self) -> Option<Firmware> {
        self.release.as_ref().map(|p| Firmware {
            version: p.manifest.version.clone(),
        })
    }

    pub fn request(
        &mut self,
        reason: Request,
        wake: &Sender<Message>,
        fetch: impl FnOnce(Option<&Manifest>) -> Result<Option<Package>> + Send + 'static,
    ) {
        let download = self.download.get_or_insert_with(|| {
            let wake = wake.clone();
            let current = self.release.as_ref().map(|p| p.manifest.clone());
            let cache = self.cache.clone();
            let (sender, result) = mpsc::channel();
            thread::spawn(move || {
                let package = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    let package = fetch(current.as_ref())?;
                    if let (Some(p), Some(dir)) = (&package, cache)
                        && let Err(error) = p.store(&dir)
                    {
                        eprintln!("Couldn’t store firmware for offline use: {error:#}");
                    }
                    Ok(package)
                }))
                .unwrap_or_else(|_| Err(anyhow::anyhow!("Firmware download interrupted")));
                if sender.send(package).is_ok() {
                    let _ = wake.send(Message::FirmwareReady);
                }
            });
            Download {
                explicit_check: false,
                install: None,
                result,
            }
        });
        match reason {
            Request::Background => {}
            Request::Check => download.explicit_check = true,
            Request::Install(reply) => download.install = Some(reply),
        }
    }

    pub fn finish(&mut self) -> Option<(bool, Result<()>)> {
        let result = match self.download.as_ref()?.result.try_recv() {
            Ok(result) => result,
            Err(TryRecvError::Empty) => return None,
            Err(TryRecvError::Disconnected) => {
                Err(anyhow::anyhow!("Firmware download interrupted"))
            }
        };
        let download = self.download.take()?;
        let result = result.and_then(|package| {
            if let Some(p) = package {
                self.release = Some(p);
            }
            anyhow::ensure!(self.release.is_some(), "No verified firmware available");
            if let Some(reply) = download.install {
                let _ = reply.try_send(());
            }
            Ok(())
        });
        Some((download.explicit_check, result))
    }
}

#[cfg(test)]
mod tests;
