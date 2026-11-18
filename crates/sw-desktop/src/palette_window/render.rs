use super::*;
impl PaletteEditor {
    fn rail(&self, cx: &Context<Self>) -> impl IntoElement {
        let active = self.settings.active_palette as usize;
        div()
            .v_flex()
            .w(rems(10.5))
            .flex_shrink_0()
            .h_full()
            .p_2()
            .gap_1()
            .border_r_1()
            .border_color(cx.theme().border)
            .children(
                self.settings
                    .slots
                    .into_iter()
                    .enumerate()
                    .map(|(i, slot)| {
                        let item = Choice {
                            value: i,
                            title: slot.name.as_str().to_owned().into(),
                            colors: slot.colors,
                        };
                        Button::new(("palette-row", i))
                            .debug_selector(move || format!("palette-row-{i}"))
                            .ghost()
                            .small()
                            .w_full()
                            .selected(i == active)
                            .disabled(!self.enabled)
                            .accessibility_label(slot.name.as_str().to_string())
                            .child(item.row(cx))
                            .on_click(cx.listener(move |v, _, w, cx| {
                                if !v.enabled {
                                    return;
                                }
                                v.commit_inputs(w, cx);
                                v.settings.active_palette = i as u8;
                                v.sync_name(w, cx);
                                v.changed(w, cx);
                            }))
                    }),
            )
            .child(div().flex_1())
            .child(self.more(cx))
    }
    fn render_slot(&self, cx: &Context<Self>) -> impl IntoElement {
        let i = self.settings.active_palette as usize;
        div()
            .h_flex()
            .gap_1()
            .child(
                div()
                    .flex_1()
                    .min_w_0()
                    .child(Input::new(&self.name).small().disabled(!self.enabled)),
            )
            .child(
                Button::new("reset-palette")
                    .small()
                    .ghost()
                    .label("Reset")
                    .disabled(!self.enabled || self.settings.slots[i] == PaletteSlot::default_at(i))
                    .on_click(cx.listener(move |v, _, w, cx| {
                        v.settings.slots[i] = PaletteSlot::default_at(i);
                        v.midtones[i] = Midtones::from_colors(v.settings.slots[i].colors);
                        v.sync_name(w, cx);
                        v.changed(w, cx);
                    })),
            )
    }
    fn render_shades(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let two_color = self.midtones[self.settings.active_palette as usize].automatic();
        let mode = ButtonGroup::new("color-mode")
            .small()
            .disabled(!self.enabled)
            .child(
                Button::new("four-colors")
                    .label("Manual")
                    .selected(!two_color),
            )
            .child(
                Button::new("two-colors")
                    .label("Blend midtones")
                    .selected(two_color),
            )
            .on_click(cx.listener(|v, values: &Vec<usize>, w, cx| {
                v.set_two_color(values.first() == Some(&1), w, cx);
            }));
        let shades = self
            .settings
            .colors()
            .into_iter()
            .enumerate()
            .map(|(shade, color)| {
                let label = ["Background", "Light", "Dark", "Ink"][shade];
                let auto = two_color && (shade == 1 || shade == 2);
                let owner = cx.entity().downgrade();
                let swatch =
                    gpui_kit::base::ColorSwatch::new(("shade", shade), rgb(rgb888(color)).into())
                        .w_full()
                        .h_12()
                        .bg(rgb(rgb888(color)))
                        .rounded_sm()
                        .border_2()
                        .border_color(if self.shade == shade {
                            cx.theme().primary
                        } else {
                            cx.theme().border
                        })
                        .selected(self.shade == shade)
                        .disabled(!self.enabled || auto)
                        .accessibility_label(if auto {
                            format!("{label}, auto-blended")
                        } else {
                            format!("Edit {label}")
                        })
                        .on_click(move |_, _, w, cx| {
                            let _ = owner.update(cx, |v, cx| {
                                v.commit_inputs(w, cx);
                                v.shade = shade;
                                v.sync(w, cx);
                                v.sync_hex(w, cx);
                                cx.notify();
                            });
                        });
                div()
                    .v_flex()
                    .flex_1()
                    .min_w_0()
                    .gap_1()
                    .child(
                        div()
                            .debug_selector(move || format!("shade-well-{shade}"))
                            .p(px(2.))
                            .border_1()
                            .rounded_md()
                            .border_color(if self.shade == shade {
                                cx.theme().primary
                            } else {
                                cx.theme().background
                            })
                            .child(swatch),
                    )
                    .child(
                        div()
                            .h_flex()
                            .gap_1()
                            .child(div().text_xs().child(label))
                            .when(auto, |d| {
                                d.child(
                                    div()
                                        .text_xs()
                                        .text_color(cx.theme().muted_foreground)
                                        .child("Auto"),
                                )
                            }),
                    )
                    .child(
                        div()
                            .text_xs()
                            .font_family("monospace")
                            .text_color(cx.theme().muted_foreground)
                            .child(format!("#{:06X}", rgb888(color))),
                    )
            });
        div()
            .h_flex()
            .gap_4()
            .items_start()
            .child(
                div()
                    .v_flex()
                    .flex_1()
                    .gap_3()
                    .child(
                        div()
                            .h_flex()
                            .gap_1()
                            .child(mode)
                            .child(div().flex_1())
                            .child(
                                Button::new("copy-palette")
                                    .small()
                                    .ghost()
                                    .icon(assets::icon("copy"))
                                    .accessibility_label("Copy Palette")
                                    .tooltip("Copy Palette")
                                    .on_click(cx.listener(|v, _, w, cx| v.copy(w, cx))),
                            )
                            .child(
                                Button::new("paste-palette")
                                    .small()
                                    .ghost()
                                    .icon(assets::icon("paste"))
                                    .accessibility_label("Paste Palette")
                                    .tooltip("Paste Palette")
                                    .disabled(!self.enabled)
                                    .on_click(cx.listener(|v, _, w, cx| v.paste(w, cx))),
                            ),
                    )
                    .child(div().h_flex().items_start().gap_2().children(shades)),
            )
            .child(crate::palette_window::scene::scene(self.settings.colors()))
    }
    fn drag_color(
        &mut self,
        surface: crate::palette_window::color_field::Surface,
        at: Point<Pixels>,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let old = self.settings.colors()[self.shade];
        let new = self.picker.pick(surface, at);
        if old != new {
            self.set_color(new, w, cx);
        } else {
            cx.notify();
        }
    }
    fn finish_drag(&mut self, e: &MouseUpEvent, w: &mut Window, cx: &mut Context<Self>) {
        if let Some(surface) = self.dragging.take()
            && self.enabled
        {
            self.drag_color(surface, e.position, w, cx);
        }
    }
    fn render_picker(&self, cx: &Context<Self>) -> impl IntoElement {
        use crate::palette_window::color_field::Surface;
        let surface = |kind: Surface| {
            let hue = kind == Surface::Hue;
            div()
                .id(if hue { "picker-hue" } else { "picker-plane" })
                .h_full()
                .relative()
                .border_1()
                .border_color(cx.theme().border)
                .cursor_crosshair()
                .map(|d| if hue { d.w_5() } else { d.flex_1() })
                .child(self.picker.surface(kind))
                .on_mouse_down(
                    MouseButton::Left,
                    cx.listener(move |v, e: &MouseDownEvent, w, cx| {
                        if v.enabled {
                            v.commit_inputs(w, cx);
                            w.focus(&v.focus, cx);
                            v.dragging = Some(kind);
                            v.drag_color(kind, e.position, w, cx);
                        }
                    }),
                )
        };
        let shade = ["Background", "Light", "Dark", "Ink"][self.shade];
        div()
            .v_flex()
            .flex_1()
            .min_h(rems(8.5))
            .gap_2()
            .border_t_1()
            .border_color(cx.theme().border)
            .pt_3()
            .child(
                div()
                    .h_flex()
                    .gap_2()
                    .child(div().flex_1().child(format!("{shade} color")))
                    .child(
                        div()
                            .w(rems(6.))
                            .child(Input::new(&self.hex).small().disabled(!self.enabled)),
                    ),
            )
            .child(
                div()
                    .h_flex()
                    .gap_2()
                    .flex_1()
                    .min_h(rems(5.))
                    .max_h(rems(8.5))
                    .child(surface(Surface::Plane))
                    .child(surface(Surface::Hue)),
            )
    }
}
impl Render for PaletteEditor {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let message = self.error.clone().or_else(|| {
            (!self.enabled).then(|| {
                "Palette editing is unavailable while your Stick is busy or disconnected.".into()
            })
        });
        let footer = crate::device_window::render::feedback(message, self.error.is_some(), cx)
            .when(self.error.is_some(), |row| {
                row.child(
                    Button::new("dismiss-palette-error")
                        .xsmall()
                        .ghost()
                        .label("Dismiss")
                        .on_click(cx.listener(|v, _, _, cx| {
                            v.error = None;
                            cx.notify();
                        })),
                )
            });
        let content = div()
            .v_flex()
            .flex_1()
            .min_h_0()
            .id("palette-editor")
            .overflow_y_scroll()
            .px_4()
            .py_3()
            .gap_3()
            .child(self.render_slot(cx))
            .child(self.render_shades(cx))
            .child(self.render_picker(cx));
        div()
            .v_flex()
            .size_full()
            .bg(cx.theme().background)
            .text_color(cx.theme().foreground)
            .text_sm()
            .on_mouse_move(cx.listener(|v, e: &MouseMoveEvent, w, cx| {
                if let Some(surface) = v.dragging
                    && e.pressed_button == Some(MouseButton::Left)
                    && v.enabled
                {
                    v.drag_color(surface, e.position, w, cx);
                }
            }))
            .on_mouse_up(MouseButton::Left, cx.listener(Self::finish_drag))
            .on_mouse_up_out(MouseButton::Left, cx.listener(Self::finish_drag))
            .track_focus(&self.focus)
            .key_context("Stickwalker Palettes")
            .on_action(cx.listener(|v, _: &crate::CloseWindow, w, cx| {
                v.commit_inputs(w, cx);
                w.remove_window();
            }))
            .map(|root| crate::menus::forward_device_actions(root, self.owner))
            .on_action(|_: &crate::AppearanceDark, w, cx| {
                crate::preferences::set_appearance(Some(true), w, cx)
            })
            .on_action(|_: &crate::AppearanceLight, w, cx| {
                crate::preferences::set_appearance(Some(false), w, cx)
            })
            .on_action(|_: &crate::AppearanceSystem, w, cx| {
                crate::preferences::set_appearance(None, w, cx)
            })
            .on_action(cx.listener(|v, _: &crate::CopyPalette, w, cx| v.copy(w, cx)))
            .on_action(cx.listener(|v, _: &crate::PastePalette, w, cx| v.paste(w, cx)))
            .on_action(cx.listener(|v, _: &crate::CopyAllPalettes, w, cx| {
                v.action(PaletteAction::CopyAll, w, cx)
            }))
            .on_action(cx.listener(|v, _: &crate::PastePaletteSet, w, cx| {
                v.action(PaletteAction::PasteSet, w, cx)
            }))
            .on_action(cx.listener(|v, _: &crate::ImportPalettes, w, cx| {
                v.action(PaletteAction::Import, w, cx)
            }))
            .on_action(cx.listener(|v, _: &crate::ExportPalettes, w, cx| {
                v.action(PaletteAction::Export, w, cx)
            }))
            .on_action(cx.listener(|v, _: &crate::ResetAllPalettes, w, cx| {
                v.action(PaletteAction::ResetAll, w, cx)
            }))
            .child({
                let bar = crate::TitleBar::new();
                #[cfg(not(target_os = "macos"))]
                let bar = bar.child(self.menu_bar.clone());
                bar.child(div().flex_1().child("Palettes"))
            })
            .child(
                div()
                    .h_flex()
                    .items_stretch()
                    .flex_1()
                    .min_h_0()
                    .child(self.rail(cx))
                    .child(
                        div()
                            .v_flex()
                            .flex_1()
                            .min_w_0()
                            .min_h_0()
                            .child(content)
                            .child(footer),
                    ),
            )
    }
}
