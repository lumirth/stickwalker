use super::{CheckFeedback, ExitState, StickView};
use crate::AppState;
use gpui_kit::{Context, Task, Window};
use host::worker::{Command, ConnectionState, DeviceTarget};

pub(super) struct PendingInstall {
    target: DeviceTarget,
    command: Command,
    // Dropping the task cancels this window's install intent. The fetch may still fill the cache.
    downloading: Option<Task<()>>,
}

impl PendingInstall {
    pub(super) fn status(&self) -> &'static str {
        if self.downloading.is_some() {
            "Downloading firmware…"
        } else {
            "Waiting to install firmware…"
        }
    }
}

impl StickView {
    pub(super) fn install(&mut self, w: &mut Window, cx: &mut Context<Self>) {
        self.clear_local_error(cx);
        if !cx.global::<AppState>().menu.can_install {
            return;
        }
        if matches!(self.state.connection, ConnectionState::Unmanaged { .. }) {
            self.confirm(
                &format!("Install Stickwalker on {}?", self.device_label()),
                "This replaces the device’s current software. Existing Stickwalker saves and settings are kept.",
                "Install", Command::Update, w, cx,
            );
            return;
        }
        self.start_install(Command::Update, cx);
    }

    pub(super) fn start_install(&mut self, command: Command, cx: &mut Context<Self>) {
        if self.exit != ExitState::Running
            || self.pending_install.is_some()
            || self.locked()
            || self.previewing()
            || !command.allowed(&self.state.connection)
        {
            return;
        }
        self.latest_check = CheckFeedback::Firmware;
        if self.state.firmware.is_some() {
            self.send(command);
        } else if let Some(target) = self.state.connection.target() {
            let (reply, result) = async_channel::bounded(1);
            match self
                .worker
                .commands
                .send(None, Command::DownloadFirmware(reply))
            {
                Ok(()) => self.await_firmware(target, command, result, cx),
                Err(error) => self.local_error = Some(error.to_string()),
            }
        }
        self.refresh_shell(cx);
    }

    pub(super) fn await_firmware(
        &mut self,
        target: DeviceTarget,
        command: Command,
        result: async_channel::Receiver<()>,
        cx: &mut Context<Self>,
    ) {
        let task = cx.spawn(async move |view, cx| {
            let downloaded = result.recv().await.is_ok();
            let _ = view.update(cx, |view, cx| {
                if !downloaded {
                    view.pending_install = None;
                    view.local_error = Some("Couldn’t download firmware. Check your connection or open a firmware file.".into());
                } else if let Some(pending) = &mut view.pending_install {
                    pending.downloading = None;
                }
                view.refresh_shell(cx);
            });
        });
        self.pending_install = Some(PendingInstall {
            target,
            command,
            downloading: Some(task),
        });
    }

    pub(super) fn resume_install(&mut self) {
        let Some(pending) = &self.pending_install else {
            return;
        };
        if self.exit != ExitState::Running
            || self.previewing()
            || !pending.command.allowed(&self.state.connection)
            || !self
                .state
                .connection
                .target()
                .is_some_and(|current| pending.target.matches(&current))
        {
            self.pending_install = None;
            return;
        }
        if pending.downloading.is_some() || self.locked() {
            return;
        }
        let Some(pending) = self.pending_install.take() else {
            return;
        };
        if let Err(error) = self
            .worker
            .commands
            .send(Some(pending.target), pending.command)
        {
            self.local_error = Some(error.to_string());
        }
    }
}
