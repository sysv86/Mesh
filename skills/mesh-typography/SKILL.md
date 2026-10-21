---
name: mesh-typography
description: "Use when writing or debugging text in Mesh .mesh files: font declarations, font families and local TTF/OTF/TTC files, Fontconfig fallback, weight/style/stretch, HarfBuzz features, letter/word/line spacing, wrapping, baseline placement, text gradients/strokes, or text used as clip geometry. Complements the core mesh skill."
---

# Mesh typography

Text in Mesh is shaped into real glyph outlines at parse time. Those outlines
then use the same fill, stroke, gradient, shadow, glow, grain, clip, blend and
transform pipeline as every other shape.

## Working order

1. Declare fonts before the text that uses them.
2. Set `canvas.font_family` for the document default.
3. Use `font "id"` for reusable faces; use `font: "id"` in text.
4. Validate and render; text errors are parse-time errors.

## Minimal text

```mesh
mesh "type" {
  canvas {
    width: 800
    height: 300
    background: #0B0D12
    font_family: "Open Sans"
    font_fallback: "Noto Sans"
  }

  text "title" {
    at: (48, 120)
    baseline: true
    content: "Type is geometry."
    size: 54
    weight: 700
    fill: #F8FAFC
  }
}
```

`at:` anchors the visual centre of the ink by default. Add `baseline: true` to
anchor the first baseline instead, which is usually what you want when matching
SVG or designing from a baseline grid.

## Font declarations

```mesh
font "display" {
  family: "Open Sans"
  fallback: "Noto Sans, DejaVu Sans"
  weight: 700
  style: normal       // normal | italic | oblique
  stretch: 100        // percent, or condensed / expanded
  index: 0            // face index inside a .ttc/.otc
}

font "serif" {
  src: "fonts/NotoSerif-Regular.ttf"  // relative to the .mesh file
  family: "Noto Serif"
}
```

`src` wins over `family` when both are present. Local files accept `.ttf`,
`.otf`, `.ttc` and `.otc`. Relative paths resolve against the directory that
contains the `.mesh` file.

## Text properties

| Property | Meaning |
| :--- | :--- |
| `at` | ink centre, or baseline origin with `baseline: true` |
| `baseline` | `true` / `false` |
| `align` | `left`, `center`, `right` |
| `content` | UTF-8 string; `\n` is a hard line break |
| `size` | font size in pixels |
| `font` | declared font id |
| `font_family` | family name, optionally comma-separated |
| `font_src` | local font file |
| `font_fallback` | comma-separated fallback families |
| `font_weight` / `weight` | 100..900 or `thin`..`black` |
| `font_style` | `normal`, `italic`, `oblique` |
| `font_stretch` | percent or keyword |
| `font_features` | OpenType features, e.g. `"kern,liga,ss01"` |
| `font_language` | BCP-47 language tag, e.g. `"en"` |
| `letter_spacing` | extra advance per character |
| `word_spacing` | extra space between words |
| `line_height` | multiplier of font size |
| `max_width` | wrapping width |
| `wrap` | `true` enables wrapping with `max_width` |
| `fill` / `stroke` | normal Mesh paint values |

## Text as geometry

Text can be a clip target or a boolean child because the shaping result is a real
outline.

```mesh
text "clip_word" {
  at: (48, 300)
  baseline: true
  content: "MESH"
  size: 140
  weight: 700
  fill: #FFFFFF
  opacity: 0
}

rect "fill" {
  x: 0
  y: 0
  width: width
  height: height
  fill: @accent
  clip: @clip_word
}
```

The text must be declared before the clipped node. The text can be invisible;
the clip still uses its outline.

## Effects

```mesh
text "gradient_title" {
  at: (48, 140)
  baseline: true
  content: "Gradient"
  size: 64
  weight: 700
  fill: @accent
  stroke: #FFFFFF
  stroke_width: 0.6
  shadow(0, 10, 28, fade(#000000, 0.45))
  glow(14, fade(#65D9FF, 0.25))
}
```

Everything that works on a path works here: gradients, dashes, grain, blend
modes, clipping, transforms and Boolean composition.

## Text clipping and accessibility

When exporting SVG, shaped text is emitted as exact `<path>` geometry with
`data-mesh-text` and `data-mesh-font` attributes. The original string remains in
the scene and in `.mesh` source. Keep readable text content in the source even
when the fill is transparent or clipped.

## Common errors

- `Unknown font id` — declare the font before the text, or use `font_family`.
- `font file not found` — check the path relative to the `.mesh` file.
- Text renders black on a dark canvas — set `fill` explicitly.
- Text does not wrap — set both `max_width` and `wrap: true`.
- Baseline looks off — add `baseline: true` and check `at.y`.
- Missing glyphs — add a fallback family with coverage for that script.

## Example

`examples/text-card.mesh` is a validated text specimen. See also the core
`mesh` skill for scene composition and the `mesh-motion` skill for text
animation.
