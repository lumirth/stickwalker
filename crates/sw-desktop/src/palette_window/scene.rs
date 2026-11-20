use crate::*;
use host::rgb888;

/// Sample scene, drawn at integer scale in the selected palette.
pub fn scene(colors: [u16; 4]) -> impl IntoElement {
    canvas(
        |_, _, _| (),
        move |bounds, _, window, _| {
            let scale = (f32::from(bounds.size.width) / 96.).floor().max(1.);
            for (y, row) in include_str!("../../assets/palette-scene.txt")
                .lines()
                .enumerate()
            {
                let bytes = row.as_bytes();
                let mut x = 0;
                while x < bytes.len() {
                    let start = x;
                    let shade = bytes[x];
                    while x < bytes.len() && bytes[x] == shade {
                        x += 1;
                    }
                    window.paint_quad(fill(
                        Bounds::new(
                            bounds.origin + point(px(start as f32 * scale), px(y as f32 * scale)),
                            size(px((x - start) as f32 * scale), px(scale)),
                        ),
                        rgb(rgb888(colors[(shade - b'0') as usize])),
                    ));
                }
            }
        },
    )
    .w(px(192.))
    .h(px(128.))
    .flex_shrink_0()
}
