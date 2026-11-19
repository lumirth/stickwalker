use super::{DeviceSettings, PaletteEditor, PaletteSlot};
use gpui_kit::{
    AppContext, Bounds, Modifiers, Point, TestAppContext, VisualTestContext, WindowBounds,
    WindowOptions, px, size,
};
fn open_editor(
    cx: &mut TestAppContext,
) -> (gpui_kit::AnyWindowHandle, gpui_kit::Entity<PaletteEditor>) {
    cx.update(|cx| {
        gpui_kit::init(cx);
        gpui_kit::open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(Bounds::new(
                    Point::default(),
                    size(px(680.), px(430.)),
                ))),
                ..Default::default()
            },
            cx,
            |w, cx| {
                let owner = w.window_handle();
                cx.new(|cx| PaletteEditor::new(DeviceSettings::default(), owner, w, cx))
            },
        )
        .unwrap()
    })
}
#[gpui_kit::test]
fn switching_palettes_commits_the_draft_to_the_previous_palette(cx: &mut TestAppContext) {
    let (handle, editor) = open_editor(cx);
    cx.update_window(handle, |_, w, cx| {
        w.draw(cx).clear(cx);
        editor
            .read(cx)
            .name
            .clone()
            .update(cx, |input, cx| input.set_value("Desk| Palette", w, cx));
        editor
            .read(cx)
            .hex
            .clone()
            .update(cx, |input, cx| input.set_value("#FF0000", w, cx));
    })
    .unwrap();
    let mut visual = VisualTestContext::from_window(handle, cx);
    let target = visual
        .debug_bounds("palette-row-1")
        .expect("second palette row")
        .center();
    visual.simulate_click(target, Modifiers::default());
    cx.update_window(handle, |_, _, cx| {
        let settings = editor.read(cx).settings;
        assert_eq!(settings.active_palette.index(), 1);
        assert_eq!(settings.slots[0].name.as_str(), "Desk Palette");
        assert_eq!(settings.slots[0].colors[0], 0xf800);
        assert_eq!(settings.slots[1], PaletteSlot::default_at(1));
    })
    .unwrap();
}
#[gpui_kit::test]
fn switching_on_the_stick_replaces_a_focused_name_draft(cx: &mut TestAppContext) {
    use gpui_kit::Focusable;
    let (handle, editor) = open_editor(cx);
    cx.update_window(handle, |_, w, cx| {
        editor.update(cx, |editor, cx| {
            let focus = editor.name.read(cx).focus_handle(cx);
            w.focus(&focus, cx);
            editor
                .name
                .update(cx, |input, cx| input.set_value("Desk", w, cx));
            let mut incoming = editor.settings;
            incoming.active_palette = host::PaletteIndex::new(1).unwrap();
            editor.receive(Some(incoming), true, w, cx);
            assert!(focus.is_focused(w));
            assert_eq!(editor.take_committed_settings(w, cx), incoming);
        });
    })
    .unwrap();
}

#[gpui_kit::test]
fn dragging_through_white_keeps_the_ring_at_the_pointer(cx: &mut TestAppContext) {
    use crate::palette_window::color_field::Surface;
    use gpui_kit::{MouseButton, point};
    let (handle, editor) = open_editor(cx);
    let mut ui = VisualTestContext::from_window(handle, cx);
    let bounds = ui.update(|w, cx| {
        w.draw(cx).clear(cx);
        editor.read(cx).picker.plane_bounds.get()
    });
    let at = |x: f32, y: f32| bounds.origin + point(bounds.size.width * x, bounds.size.height * y);
    let left = MouseButton::Left;
    let mods = Modifiers::default();
    ui.simulate_mouse_down(at(0.5, 0.6), left, mods);
    for x in [0.2, 0.5, 0.9] {
        ui.simulate_mouse_move(at(x, 0.003), left, mods);
        ui.update(|_, cx| {
            let v = editor.read(cx);
            assert_eq!(v.settings.colors()[0], 0xffff);
            let ring = v.picker.marker(Surface::Plane);
            assert!((ring.x - x).abs() < 0.001 && (ring.y - 0.003).abs() < 0.001);
        });
    }
    ui.simulate_mouse_up(at(0.9, 0.003), left, mods);
    let hue = ui.update(|_, cx| editor.read(cx).picker.hue_bounds.get().center());
    ui.simulate_click(hue, mods);
    ui.update(|_, cx| {
        let ring = editor.read(cx).picker.marker(Surface::Plane);
        assert!((ring.x - 0.9).abs() < 0.001 && (ring.y - 0.003).abs() < 0.001);
    });
}
#[gpui_kit::test]
fn palette_errors_leave_the_picker_in_place_at_minimum_size(cx: &mut TestAppContext) {
    let (handle, editor) = open_editor(cx);
    let mut ui = VisualTestContext::from_window(handle, cx);
    let before = ui.update(|w, cx| {
        w.draw(cx).clear(cx);
        editor.read(cx).picker.plane_bounds.get()
    });
    ui.update(|w, cx| {
        editor.update(cx, |v, cx| v.apply_set("Broken|123456|oops", w, cx));
        w.draw(cx).clear(cx);
        let v = editor.read(cx);
        assert!(v.error.is_some());
        assert_eq!(v.picker.plane_bounds.get(), before);
        assert!(
            before.bottom() <= w.viewport_size().height - px(32.),
            "field {before:?} in viewport {:?}",
            w.viewport_size()
        );
    });
}

#[gpui_kit::test]
fn leaving_an_invalid_hex_draft_restores_the_target_shade(cx: &mut TestAppContext) {
    let (window, editor) = open_editor(cx);
    let mut ui = VisualTestContext::from_window(window, cx);
    ui.update(|w, cx| {
        editor.update(cx, |v, cx| {
            v.hex.update(cx, |input, cx| input.set_value("oops", w, cx));
            v.commit(w, cx, true);
            assert!(v.error.is_some());
        });
        w.draw(cx).clear(cx);
    });
    let light = ui.debug_bounds("shade-well-1").unwrap().center();
    ui.simulate_click(light, Modifiers::default());
    ui.update(|_, cx| {
        let v = editor.read(cx);
        assert!(v.error.is_none());
        assert_eq!(
            v.hex.read(cx).value().as_ref(),
            format!("#{:06X}", host::rgb888(v.settings.colors()[1]))
        );
    });
}

#[gpui_kit::test]
fn closing_pulls_draft_colors_without_emitting_an_edit(cx: &mut TestAppContext) {
    use std::{cell::Cell, rc::Rc};
    let (handle, editor) = open_editor(cx);
    let events = Rc::new(Cell::new(0));
    let count = events.clone();
    let _sub = cx.update(|cx| {
        cx.subscribe(&editor, move |_, _: &super::PaletteChanged, _| {
            count.set(count.get() + 1)
        })
    });
    cx.update_window(handle, |_, w, cx| {
        editor.update(cx, |e, cx| {
            e.hex
                .update(cx, |input, cx| input.set_value("#FF0000", w, cx));
            let settings = e.take_committed_settings(w, cx);
            assert_eq!(settings.colors()[0], 0xf800);
        });
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(events.get(), 0);
}
