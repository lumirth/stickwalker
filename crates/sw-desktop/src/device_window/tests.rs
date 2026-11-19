use super::{ExitState, FileAction, PendingExit, StickView};
use crate::updater::AppUpdate;
use gpui_kit::{AnyWindowHandle, AppContext, Entity, TestAppContext, WindowOptions};

fn open_device(cx: &mut TestAppContext) -> (AnyWindowHandle, Entity<StickView>) {
    cx.update(|cx| {
        gpui_kit::init(cx);
        gpui_kit::open_window(WindowOptions::default(), cx, |w, cx| {
            cx.new(|cx| StickView::new(w, cx))
        })
        .unwrap()
    })
}

#[gpui_kit::test]
fn erasing_a_reused_port_requires_a_fresh_confirmation(cx: &mut TestAppContext) {
    use host::usb::Port;
    use host::worker::{ConnectionState, Presentation};

    let (window, view) = open_device(cx);
    let port = Port {
        path: "COM4".into(),
        serial: String::new(),
    };
    let connected = Presentation {
        ports: vec![port.clone()],
        connection: ConnectionState::Unmanaged { port: port.clone() },
        ..Default::default()
    };
    let present = |cx: &mut TestAppContext, state| {
        cx.update_window(window, |_, w, cx| {
            view.update(cx, |v, cx| v.receive(state, w, cx))
        })
        .unwrap();
    };
    present(cx, connected.clone());
    for gap in [
        Some(ConnectionState::Disconnected),
        Some(ConnectionState::Reconnecting { port }),
        None,
    ] {
        cx.update_window(window, |_, w, cx| {
            w.dispatch_action(Box::new(crate::EraseInstall), cx);
        })
        .unwrap();
        cx.run_until_parked();
        assert!(cx.has_pending_prompt());
        let still_attached = gap.is_none();
        if let Some(connection) = gap {
            present(
                cx,
                Presentation {
                    connection,
                    ..Default::default()
                },
            );
        }
        present(cx, connected.clone());
        cx.simulate_prompt_answer("Erase and install");
        cx.run_until_parked();
        // The preview worker has a closed command channel. Only a valid approval reaches it.
        cx.update(|cx| {
            assert_eq!(
                view.read(cx).local_error.as_deref(),
                still_attached.then_some("The Stick connection closed.")
            )
        });
    }
}

#[gpui_kit::test]
fn importing_with_the_editor_open_survives_immediate_quit(cx: &mut TestAppContext) {
    let (handle, view) = open_device(cx);
    cx.update_window(handle, |_, w, cx| {
        view.update(cx, |v, cx| {
            v.show_state(4, w, cx);
            v.open_palette(false, cx);
            assert!(v.editor.is_some());
        });
        w.dispatch_action(Box::new(crate::ImportPalettes), cx);
    })
    .unwrap();
    cx.run_until_parked();
    let file = tempfile::NamedTempFile::new().unwrap();
    std::fs::write(file.path(), "Fern|BADA55|17320B\nDusk|FEDCBA|102030").unwrap();
    cx.simulate_path_prompt_response(|_| Some(vec![file.path().to_owned()]));
    cx.run_until_parked();
    cx.update_window(handle, |_, _, cx| {
        view.update(cx, |v, cx| {
            let imported = v.state.connection.info().unwrap().settings;
            assert_eq!(imported.slots[0].name.as_str(), "Fern");
            assert_eq!(imported.slots[1].name.as_str(), "Dusk");
            let issued = v.next_revision;
            v.request_exit(PendingExit::Quit, cx);
            assert_eq!(v.state.connection.info().unwrap().settings, imported);
            assert_eq!(
                v.next_revision, issued,
                "quit resubmitted stale editor settings"
            );
        });
    })
    .unwrap();
}

#[gpui_kit::test]
fn a_failed_palette_write_cannot_settle_its_retry(cx: &mut TestAppContext) {
    let (handle, view) = open_device(cx);
    cx.update_window(handle, |_, w, cx| {
        view.update(cx, |v, cx| {
            v.show_state(4, w, cx);
            let failed = v.issue_palette_revision();
            let mut snapshot = v.state.clone();
            snapshot.failed_revision = Some(failed);
            v.present(snapshot.clone(), w, cx);
            assert!(v.pending_revision.is_none());
            let retry = v.issue_palette_revision();
            assert!(retry > failed);
            v.request_exit(PendingExit::Restart, cx);
            v.present(snapshot.clone(), w, cx);
            assert_eq!(v.exit, ExitState::Draining(PendingExit::Restart));
            snapshot.failed_revision = Some(retry);
            v.present(snapshot, w, cx);
            assert!(v.pending_revision.is_none());
            assert_eq!(v.exit, ExitState::Running);
        });
    })
    .unwrap();
}

#[gpui_kit::test]
fn opening_the_firmware_picker_or_quitting_cancels_a_waiting_install(cx: &mut TestAppContext) {
    use host::worker::Command;
    let (handle, view) = open_device(cx);
    let (download, reply) = async_channel::bounded(1);
    cx.update_window(handle, |_, w, cx| {
        view.update(cx, |v, cx| {
            v.show_state(4, w, cx);
            v.preview = None;
            v.state.firmware = None;
            v.await_firmware(
                v.state.connection.target().unwrap(),
                Command::CleanInstall,
                reply,
                cx,
            );
            v.refresh_shell(cx);
            assert!(v.editable());
            assert!(!cx.global::<crate::AppState>().menu.can_install);
            v.files(FileAction::Package, w, cx);
        });
    })
    .unwrap();
    cx.run_until_parked();
    assert!(cx.did_prompt_for_paths());
    assert!(download.try_send(()).is_err());
    cx.simulate_path_prompt_response(|_| None);
    cx.run_until_parked();
    let (download, reply) = async_channel::bounded(1);
    cx.update_window(handle, |_, _, cx| {
        view.update(cx, |v, cx| {
            assert!(v.pending_install.is_none() && v.local_error.is_none());
            v.await_firmware(
                v.state.connection.target().unwrap(),
                Command::Update,
                reply,
                cx,
            );
            v.request_exit(PendingExit::Quit, cx);
            assert_eq!(v.exit, ExitState::Exiting(PendingExit::Quit));
        });
    })
    .unwrap();
    cx.run_until_parked();
    assert!(download.try_send(()).is_err());
}

#[gpui_kit::test]
fn a_failed_or_cancelled_sparkle_restart_releases_the_window(cx: &mut TestAppContext) {
    use crate::updater::UpdateEvent;
    use gpui_updater_core::{DriverEvent, ErrorKind, UpdateError};
    let (window, view) = open_device(cx);
    let failure = UpdateError::new(ErrorKind::Other, "could not relaunch");
    cx.update_window(window, |_, _, cx| {
        view.update(cx, |v, cx| {
            for event in [DriverEvent::Settled, DriverEvent::Failed(failure)] {
                v.exit = ExitState::Exiting(PendingExit::Restart);
                v.updates.state = AppUpdate::Restarting;
                v.updates.accept(UpdateEvent::Driver(Box::new(DriverEvent::Installing)));
                v.refresh_shell(cx);
                assert!(v.locked());
                let failed = matches!(event, DriverEvent::Failed(_));
                v.updates.accept(UpdateEvent::Driver(Box::new(event)));
                v.refresh_shell(cx);
                assert_eq!(v.exit, ExitState::Running);
                assert!(!v.locked());
                if failed {
                    assert!(matches!(&v.updates.state, AppUpdate::Failed(s) if s == "Couldn’t restart to update."));
                }
            }
        });
    })
    .unwrap();
}
