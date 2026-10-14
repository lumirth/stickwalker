# Edit built-in artwork

Copy the images and palette you want to edit from `assets/placeholders/` into `assets/local/`, or [extract them from your own ROM](../docs/build.md#artwork).

Build with `--artwork local` to use your edits.

Open NCG images in [NitroPaint](https://github.com/Garhoogin/NitroPaint), or convert them to BMP for another editor.

```sh
uv run -m tools.assets convert assets/local/walker.ncg walker.bmp
# Edit walker.bmp.
uv run -m tools.assets convert walker.bmp assets/local/walker.ncg --force
```

Keep the original dimensions and use opaque grayscale values 255, 170, 85 and 0. The converter also accepts 173 and 82. Edit `font.bmp` directly, using only 255 and 0. Turn off antialiasing.

If conversion flags byte `0x53` in `walker.ncg`, keep pixel 9,9 light gray or black, and pixels 9,8 and 9,10 through 9,15 white or dark gray. Coordinates start at zero.

The bundled placeholders are [CC0](placeholders/LICENSE.md).
