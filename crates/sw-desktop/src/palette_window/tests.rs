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
        assert_eq!(settings.active_palette, 1);
        assert_eq!(settings.slots[0].name.as_str(), "Desk Palette");
        assert_eq!(settings.slots[0].colors[0], 0xf800);
        assert_eq!(settings.slots[1], PaletteSlot::default_at(1));
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
