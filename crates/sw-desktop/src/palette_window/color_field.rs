use color_space::{FromColor, Okhsl, Srgb};
use gpui_kit::*;
use host::{rgb565, rgb888};
use std::{cell::Cell, rc::Rc};

const COLS: usize = 80;
const ROWS: usize = 64;

pub fn coordinates(color: u16) -> Okhsl {
    let [_, r, g, b] = rgb888(color).to_be_bytes();
    Okhsl::from_color(Srgb::new(r, g, b).into_format::<f32>())
}

fn color_at(hue: f32, saturation: f32, lightness: f32) -> u16 {
    let rgb: Srgb<u8> = Srgb::from_color(Okhsl::new(hue, saturation, lightness)).into_format();
    rgb565(u32::from_be_bytes([0, rgb.red, rgb.green, rgb.blue]))
}

#[derive(Clone, Copy, PartialEq)]
pub enum Surface {
    Plane,
    Hue,
}

pub struct Picker {
    hue: f32,
    // Several field positions can produce the same RGB565 value.
    position: Point<f32>,
    color: Option<u16>,
    plane: Rc<Vec<u16>>,
    pub plane_bounds: Rc<Cell<Bounds<Pixels>>>,
    pub hue_bounds: Rc<Cell<Bounds<Pixels>>>,
}
impl Picker {
    pub fn new() -> Self {
        let mut picker = Self {
            hue: -1.,
            position: Point::default(),
            color: None,
            plane: Default::default(),
            plane_bounds: Default::default(),
            hue_bounds: Default::default(),
        };
        picker.set_hue(0.);
        picker
    }
    fn set_hue(&mut self, hue: f32) {
        if self.hue == hue {
            return;
        }
        self.hue = hue;
        self.plane = Rc::new(
            (0..ROWS * COLS)
                .map(|i| {
                    color_at(
                        hue,
                        (i % COLS) as f32 / (COLS - 1) as f32,
                        1. - (i / COLS) as f32 / (ROWS - 1) as f32,
                    )
                })
                .collect(),
        );
    }
    pub fn sync_color(&mut self, color: u16) {
        if self.color == Some(color) {
            return;
        }
        let c = coordinates(color);
        self.position = point(c.saturation.clamp(0., 1.), (1. - c.lightness).clamp(0., 1.));
        if c.saturation > 0.001 && (0.001..0.999).contains(&c.lightness) {
            self.set_hue(c.hue.into_positive_degrees());
        }
        self.color = Some(color);
    }
    pub fn pick(&mut self, surface: Surface, cursor: Point<Pixels>) -> u16 {
        let bounds = match surface {
            Surface::Plane => self.plane_bounds.get(),
            Surface::Hue => self.hue_bounds.get(),
        };
        let y = ((cursor.y - bounds.top()) / bounds.size.height).clamp(0., 1.);
        match surface {
            Surface::Plane => {
                let x = ((cursor.x - bounds.left()) / bounds.size.width).clamp(0., 1.);
                self.position = point(x, y);
            }
            Surface::Hue => self.set_hue(y * 360.),
        }
        let col = ((self.position.x * COLS as f32) as usize).min(COLS - 1);
        let row = ((self.position.y * ROWS as f32) as usize).min(ROWS - 1);
        let color = self.plane[row * COLS + col];
        self.color = Some(color);
        color
    }
    pub fn marker(&self, surface: Surface) -> Point<f32> {
        match surface {
            Surface::Plane => self.position,
            Surface::Hue => point(0.5, self.hue / 360.),
        }
    }
    pub fn surface(&self, surface: Surface) -> impl IntoElement {
        let bounds = match surface {
            Surface::Plane => self.plane_bounds.clone(),
            Surface::Hue => self.hue_bounds.clone(),
        };
        let colors = self.plane.clone();
        let marker = self.marker(surface);
        canvas(
            move |b, _, _| bounds.set(b),
            move |b, _, w, _| {
                let cols = if surface == Surface::Hue { 1 } else { COLS };
                for y in 0..ROWS {
                    let mut x = 0;
                    while x < cols {
                        let start = x;
                        let color = if cols == 1 {
                            color_at(y as f32 * 360. / ROWS as f32, 0.95, 0.62)
                        } else {
                            colors[y * COLS + x]
                        };
                        x += 1;
                        while x < cols && colors[y * COLS + x] == color {
                            x += 1;
                        }
                        let left = b.size.width * (start as f32 / cols as f32);
                        let right = b.size.width * (x as f32 / cols as f32);
                        let top = b.size.height * (y as f32 / ROWS as f32);
                        let bottom = b.size.height * ((y + 1) as f32 / ROWS as f32);
                        w.paint_quad(fill(
                            Bounds::new(
                                b.origin + point(left, top),
                                size(right - left, bottom - top),
                            ),
                            rgb(rgb888(color)),
                        ));
                    }
                }
                let center = b.origin + point(b.size.width * marker.x, b.size.height * marker.y);
                for (radius, color) in [(5., 0x171717), (3.5, 0xffffff)] {
                    let shape = Bounds::new(
                        center - point(px(radius), px(radius)),
                        size(px(radius * 2.), px(radius * 2.)),
                    );
                    w.paint_quad(
                        outline(shape, rgb(color), BorderStyle::Solid).corner_radii(px(radius)),
                    );
                }
            },
        )
        .size_full()
    }
}
