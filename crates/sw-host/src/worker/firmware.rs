use super::{Firmware, Message, Work};
use crate::release::{Package, Version};
use anyhow::Result;
use std::{
    path::{Path, PathBuf},
    sync::mpsc::{self, Receiver, Sender, TryRecvError},
    thread,
};

pub(super) enum Request {
    Background,
    Check,
    Install(Box<Work>),
}

struct Download {
    reason: Request,
    result: Receiver<Result<Option<Package>>>,
}

pub(super) struct FirmwareUpdates {
    cache: Option<PathBuf>,
    release: Option<Package>,
    local: Option<Package>,
    download: Option<Download>,
}

impl FirmwareUpdates {
    pub fn new(cache: Option<PathBuf>) -> Self {
        let release = cache.as_deref().and_then(Package::cached);
        Self {
            cache,
            release,
            local: None,
            download: None,
        }
    }

    pub fn open(&mut self, path: &Path) -> Result<()> {
        self.local = Some(Package::open(path)?);
        Ok(())
    }

    pub fn selected(&self) -> Option<&Package> {
        self.local.as_ref().or(self.release.as_ref())
    }

    pub fn selection(&self) -> Option<Firmware> {
        if let Some(p) = &self.local {
            Some(Firmware::Local(p.manifest.version.to_string()))
        } else {
            self.release
                .as_ref()
                .map(|p| Firmware::Release(p.manifest.version.to_string()))
        }
    }

    pub fn release_version(&self) -> Option<&Version> {
        self.release.as_ref().map(|p| &p.manifest.version)
    }

    pub fn request(
        &mut self,
        reason: Request,
        wake: &Sender<Message>,
        fetch: impl FnOnce(Option<&Version>) -> Result<Option<Package>> + Send + 'static,
    ) {
        if let Some(task) = &mut self.download {
            if matches!(reason, Request::Install(_))
                || matches!(
                    (&task.reason, &reason),
                    (Request::Background, Request::Check)
                )
            {
                task.reason = reason;
            }
            return;
        }
        let wake = wake.clone();
        let version = self.release_version().cloned();
        let cache = self.cache.clone();
        let (sender, result) = mpsc::channel();
        self.download = Some(Download { reason, result });
        thread::spawn(move || {
            let package = fetch(version.as_ref());
            if let (Ok(Some(p)), Some(dir)) = (&package, cache)
                && let Err(error) = p.store(&dir)
            {
                eprintln!("Couldn’t store firmware for offline use: {error:#}");
            }
            if sender.send(package).is_ok() {
                let _ = wake.send(Message::FirmwareReady);
            }
        });
    }

    pub fn finish(&mut self) -> Option<(Request, Result<()>)> {
        let result = match self.download.as_ref()?.result.try_recv() {
            Ok(result) => result,
            Err(TryRecvError::Empty) => return None,
            Err(TryRecvError::Disconnected) => {
                Err(anyhow::anyhow!("Firmware download interrupted"))
            }
        };
        let reason = self.download.take()?.reason;
        Some((
            reason,
            result.map(|package| {
                if let Some(p) = package {
                    self.release = Some(p);
                }
            }),
        ))
    }
}

#[cfg(test)]
mod tests;
