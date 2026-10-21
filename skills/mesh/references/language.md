# Mesh language reference

This is the authoring reference for `.mesh` files. All examples use the syntax
accepted by the current `meshc` parser.

## 1. Document and canvas

```mesh
mesh "scene_name" {
  canvas {
    width: 1200
    height: 800
    background: #0B0D12
    font_family: "Open Sans"
    font_fallback: "Noto Sans"
  }
}
```

`mesh` may omit the name: `mesh { ... }`. The canvas can also use
`size: (1200, 800)` and `bg: #0B0D12`.

## 2. Variables

```mesh
let gap = 16
let ink = #E8E8E8
let accent = #D4B273
let origin = (80, 120)
let half = width * 0.5
```

Variables are evaluated at parse time. A point variable can be used as
`@origin`, `origin.x` or `origin.y` depending on the property context. Node
anchors always use the `@id.anchor` form.

## 3. Shape declarations

```mesh
rect "card" {
  x: 40
  y: 40
  width: 320
  height: 180
  radius: 18
  // radius: [top_left, top_right, bottom_right, bottom_left]
  fill: #14161A
}
```

A shape may have an id or not: `rect { ... }` is valid. Children are declared
inside `group`, `boolean`, `def`, `repeat`, `stack`, `grid`, `use`, and similar
containers.

### Common shape properties

| Property | Applies to | Meaning |
| :--- | :--- | :--- |
| `x`, `y` | most shapes | top-left position |
| `pos` / `top_left` | most shapes | same as `x,y` |
| `at` | text, rect | reference point; rect uses center if `at` |
| `center` | circles, rings, stars, polygons, text | center point |
| `width`, `height` / `w`, `h` | rect, text box | dimensions |
| `size` | text or `(w,h)` | font size or pair |
| `radius`, `r` | circle/star/ring/sector | radius |
| `inner_radius` | ring/sector | hole radius |
| `rx`, `ry` / `radii` | ellipse | radii |
| `from`, `to` | line | endpoints |
| `points` | polygon/polyline/spline | point list |
| `tension` | spline | curve tension |
| `closed` | polygon/polyline | close the contour |
| `rays` | star | number of points |
| `sides` | ngon/regular_polygon/hexagon | polygon sides |
| `start_angle`, `end_angle` | arc/sector | degrees |
| `corner_radius` | rect | rounded corners |

### Shape keywords

`rect`, `circle`, `ellipse`, `line`, `polygon`, `polyline`, `spline`, `path`,
`contour`, `boolean`, `text`, `group`, `layer`, `star`, `ngon`,
`regular_polygon`, `hexagon`, `ring`, `annulus`, `arc`, `sector`.

### Paths

```mesh
path "arrow" {
  move(20, 80)
  line(120, 80)
  cubic(150, 80, 170, 60, 190, 40)
  quad(210, 20, 230, 40)
  close()
  stroke: #FFFFFF
  stroke_width: 2
  cap: round
  join: round
  arrow: end
}
```

Path commands are `move(x,y)`, `line(x,y)`, `quad(cx,cy,x,y)`,
`cubic(c1x,c1y,c2x,c2y,x,y)`, and `close()`.

### Boolean shapes

```mesh
boolean "icon" {
  op: smooth_union(16)     // union | subtract | intersect | smooth_union(k)
  fill: @accent
  circle { center: (100, 100) radius: 40 }
  circle { center: (150, 100) radius: 30 }
}
```

`ring` is sugar for a circular subtraction. For `subtract`, the first child is
the outer shape and later children are removed.

## 4. Styling

```mesh
rect "panel" {
  x: 40 y: 40 width: 420 height: 220 radius: 16
  fill: #151922
  stroke: @accent
  stroke_width: 1.5
  stroke_align: inner      // center | inner | outer
  opacity: 0.95
  blend: screen            // normal | add | screen | multiply | overlay
  grain: 0.02
  dash: [8, 5]
  dash_offset: -t * 40
  arrow: end
  arrow_size: 8
  clip: @clip_shape
  shadow(0, 18, 40, fade(#000000, 0.55))
  glow(14, fade(#65D9FF, 0.35))
  rotate: -2
  translate: (12, 0)
  scale: 1.02
  matrix: (1, 0, 0, 1, 12, 18)
}
```

`stroke` defaults to width `1.5` if `stroke_width` is not set. `clip` references
a previous shape id. `blend` and `clip` also work on groups.

## 5. Paint servers

### Linear

```mesh
linear_gradient "accent" {
  angle: 135
  stop(0.0, #65D9FF)
  stop(1.0, #A78BFA)
}
```

Alternatively use `from: (0, 0)` and `to: (1, 1)`. Use `space: world` for
canvas-space coordinates.

### Radial and conic

```mesh
radial_gradient "halo" {
  center: (0.5, 0.5)
  radius: 0.5
  stop(0.0, fade(#65D9FF, 0.4))
  stop(1.0, fade(#65D9FF, 0.0))
}

conic_gradient "dial" {
  center: (0.5, 0.5)
  angle: 0
  stop(0.0, #65D9FF)
  stop(1.0, #A78BFA)
}
```

### Procedural gradients

```mesh
mesh_gradient "tensor" {
  rows: 2
  cols: 2
  patch(0.0, 0.0, #A78BFA)
  patch(1.0, 0.0, #65D9FF)
  patch(0.0, 1.0, #D4B273)
  patch(1.0, 1.0, #7EE787)
}

diffusion_gradient "aurora" {
  point(0.15, 0.2, #A78BFA, weight: 1.4)
  point(0.85, 0.3, #65D9FF, weight: 1.2)
  point(0.5, 0.9, #7EE787, weight: 1.3)
  falloff: 2.0
}

noise_gradient "paper" {
  scale: 4
  octaves: 4
  persistence: 0.5
  lacunarity: 2.0
  seed: 7
  grain: 0.02
  stop(0.0, #101216)
  stop(1.0, #1A1D22)
}
```

`mesh_gradient` uses `patch(u, v, color, weight)`. `diffusion_gradient` uses
`point(u, v, color, weight)`; `falloff` controls the blend.

## 6. Components and layout

### Reusable defs

```mesh
def "label" {
  text "inner" {
    at: (0, 0)
    content: "Label"
    size: 18
    fill: #FFFFFF
  }
}

use "label_a" {
  target: @label
  at: (40, 60)
  scale: 1.0
  rotate: 0
  opacity: 1
  blend: normal
  clip: @mask
}
```

### Repeat

```mesh
repeat "ticks" {
  count: 12
  pivot: (400, 300)
  step_rotate: 30
  step_translate: (0, 0)
  step_scale: 1
  line {
    from: (400, 210)
    to: (400, 220)
    stroke: #94A3B8
    stroke_width: 2
  }
}
```

Inside the body, `i` is the zero-based index and `rep_t` is `i/(count-1)`.
`count` is clamped to 1..1024.

### Stack and grid

```mesh
stack "row" {
  x: 40
  y: 40
  gap: 12
  direction: horizontal   // horizontal | vertical | row | column
  align: center
  padding: 12
  fill: #14161A
  radius: 12
  text { at: (0, 0) content: "One" size: 18 fill: #FFFFFF }
  text { at: (0, 0) content: "Two" size: 18 fill: #FFFFFF }
}

grid "cards" {
  x: 40 y: 160
  cols: 3
  gap: 16
  rect { x: 0 y: 0 width: 180 height: 100 radius: 12 fill: #1A1D22 }
  rect { x: 0 y: 0 width: 180 height: 100 radius: 12 fill: #1A1D22 }
  rect { x: 0 y: 0 width: 180 height: 100 radius: 12 fill: #1A1D22 }
}
```

Children are translated by the layout pass; their BBoxes are used as item
sizes. Use `direction: grid` or the `grid` keyword for a 2D layout.

## 7. Text

```mesh
font "display" {
  family: "Open Sans"
  // src: "fonts/OpenSans-Bold.ttf" // optional local face
  fallback: "Noto Sans"
  weight: 700
  style: normal
  stretch: 100
}

text "headline" {
  at: (64, 120)
  baseline: true
  align: left
  content: "Type is geometry."
  size: 64
  font: "display"
  font_features: "kern,liga"
  font_language: "en"
  letter_spacing: 0.5
  word_spacing: 1.0
  line_height: 1.2
  max_width: 520
  wrap: true
  fill: #F8FAFC
  stroke: #65D9FF
  stroke_width: 0.75
}
```

`at:` anchors the ink center by default. `baseline: true` switches to the SVG
first baseline. `\n` creates a hard line break. `wrap: true` requires
`max_width`. Font declarations must precede the text that references them.
See the `mesh-typography` skill for the full font workflow.

## 8. Expressions and animation

Scene variables: `width`, `height`, `t`, `time`, `PI`, `TAU`, `min_dim`,
`max_dim`. Use `t` for deterministic animation.

```mesh
let pulse = oscillate(t, 0.35, 0.0, 1.0)
let x = @card.left + 24 + oscillate(t, 0.2, -8, 8)
let w = @card.width - 48
let tint = mix(#65D9FF, #A78BFA, pulse)
```

Math functions: `sin`, `cos`, `tan` (degrees), `sin_rad`, `cos_rad`, `sqrt`,
`abs`, `floor`, `ceil`, `round`, `fract`, `mod`, `min`, `max`, `pow`, `clamp`,
`lerp`, `smoothstep`, `polar_x`, `polar_y`, `noise`.

Time functions:

```mesh
oscillate(t, speed, min, max, phase_deg)
wave(t, speed, phase_deg)
pulse(t, speed)
saw(t, speed, phase)
ping_pong(t, speed)
ease_in_out(t, start, duration, from, to)
ease_out_cubic(t, start, duration, from, to)
ease_out_back(t, start, duration, from, to)
spring(t, start, from, to, stiffness, damping)
```

Anchor scalar properties: `left`, `right`, `top`, `bottom`, `width`, `height`,
`center_x`, `center_y`, `x`, `y`, `w`, `h`, `cx`, `cy`, `radius`. Point
properties: `center`, `top_left`, `top_right`, `bottom_left`, `bottom_right`,
`top_center`, `bottom_center`, `left_center`, `right_center`.

## 9. Canvas and export

```bash
meshc validate scene.mesh
meshc render scene.mesh -o scene.png --aa 4 --time 2.4
meshc animate scene.mesh -o scene.mp4 --duration 6 --fps 30
meshc compile scene.mesh --target svg -o scene.svg
meshc compile scene.mesh --target html -o scene.html
meshc pack scene.mesh -o scene.meshb
meshc unpack scene.meshb -o restored.mesh
```

`meshc animate` is extension-driven: `.mp4`/`.mov` = native H.264 (when FFmpeg
libraries are available), `.gif` = GIF89a, `.png` = horizontal sprite sheet.
`meshc render` uses `--time` for a deterministic still.

## 10. Common errors

- `Unknown property` — the property is not in this reference; remove it or use
  the documented spelling.
- `Unknown node reference '@id'` — the node id is missing or declared later.
- `Unknown font id` — declare the font before the text, or use `font_family`.
- `font file not found` — `src` is relative to the `.mesh` file's directory.
- A shape with no visible paint — set `fill` and/or `stroke`.
- A clip does nothing — `clip` targets a shape id declared earlier.
- Text is black on a dark scene — set `fill` on the text.
- MP4 is unavailable — the build has no FFmpeg libraries; use PNG/SVG or build
  a backend with FFmpeg.
