---
name: mesh
description: "Use when writing, editing, reviewing, or debugging a Mesh .mesh or .meshb document, or when the user mentions Mesh syntax, meshc, declarative vector graphics in Mesh, Mesh SDF shapes, Mesh scenes, .mesh examples, or asks an agent to generate a vector illustration or UI in Mesh. Gives a complete, current authoring workflow and syntax without requiring the C++ parser source."
---

# Mesh authoring

Write valid `.mesh` source first, then validate and render it. Do not infer the
language from the C++ implementation when this skill has the syntax.

## Core workflow

```bash
# from the Mesh repository root
./build/meshc validate scene.mesh
./build/meshc render scene.mesh -o scene.png --aa 4
./build/meshc inspect scene.mesh --json
```

If rendering looks wrong, render a still at the relevant time:

```bash
./build/meshc render scene.mesh -o debug.png --aa 1 --time 2.4
```

Only use `pack`/`unpack` when the task is about the binary format. `.meshb` is
a compiled artifact; edit `.mesh` source.

## Minimum valid document

```mesh
mesh "hello" {
  canvas {
    width: 800
    height: 480
    background: #0B0D12
    font_family: "Open Sans"
  }

  rect "card" {
    x: 40
    y: 40
    width: 720
    height: 400
    radius: 24
    fill: #151922
    stroke: fade(#FFFFFF, 0.08)
    stroke_width: 1.5
  }

  text "title" {
    at: (72, 104)
    baseline: true
    content: "Hello, Mesh."
    size: 40
    weight: 700
    fill: #F8FAFC
  }
}
```

Coordinates are pixels from the top-left. `y` grows downward. Width and height
are numbers; arithmetic and expressions are allowed.

## Syntax map

| Need | Form |
| :--- | :--- |
| Document | `mesh "name" { ... }` |
| Canvas | `canvas { width: 1200 height: 800 background: #0B0D12 }` |
| Variable | `let pad = 40` or `let ink = #E8E8E8` |
| Reusable paint | `linear_gradient "accent" { angle: 135 stop(0, #65D9FF) stop(1, #A78BFA) }` |
| Reusable component | `def "label" { ... }` then `use "label_a" { target: @label at: (20, 20) }` |
| Shape id | `rect "card" { ... }` |
| Fill | `fill: #14161A`, `fill: @accent`, `fill: none` |
| Stroke | `stroke: #FFFFFF stroke_width: 1.5 stroke_align: inner` |
| Shadow | `shadow(0, 16, 36, fade(#000000, 0.55))` |
| Glow | `glow(18, fade(#65D9FF, 0.45))` |
| Clip | `clip: @card` on the child/effect node |
| Animation | use `t` in expressions; there are no `animate` blocks |

## Shapes

| Keyword | Required core properties | Notes |
| :--- | :--- | :--- |
| `rect` | `x y width height` | `radius` or `radius: [tl, tr, br, bl]` |
| `circle` | `center: (x, y) radius` | |
| `ellipse` | `center rx ry` | |
| `line` | `from: (x, y) to: (x, y)` | stroke only |
| `polygon` | `points: [(x, y), ...]` | add `closed: true` |
| `polyline` | `points: [(x, y), ...]` | open by default |
| `spline` | `points: [(x, y), ...]` | `tension: 0.5` |
| `path` | path commands | `move/line/quad/cubic/close` |
| `star` | `center radius rays` | optional `inner_radius` |
| `ngon` / `hexagon` | `center radius sides` | |
| `ring` | `center radius inner_radius` | boolean subtraction |
| `sector` / `arc` | `center radius start_angle end_angle` | degrees |
| `boolean` | `op` + child shapes | `union`, `subtract`, `intersect`, `smooth_union(k)` |
| `group` / `layer` | child shapes | local transform and clip |
| `text` | `at content size` | see typography skill |
| `repeat` | `count` + body | exposes `i` and `rep_t` |
| `stack` / `grid` | child shapes | simple auto-layout |

Path example:

```mesh
path "wave" {
  move(20, 120)
  cubic(80, 40, 160, 200, 220, 120)
  quad(260, 80, 300, 120)
  stroke: #65D9FF
  stroke_width: 3
  cap: round
}
```

Boolean example:

```mesh
boolean "blob" {
  op: smooth_union(20)
  fill: @accent
  circle { center: (180, 200) radius: 70 }
  circle { center: (250, 200) radius: 54 }
  circle { center: (215, 150) radius: 42 }
}
```

## Styles

```mesh
rect "panel" {
  x: 40 y: 40 width: 320 height: 180 radius: 18
  fill: #14161A
  stroke: @accent
  stroke_width: 1.5
  stroke_align: inner        // center | inner | outer
  opacity: 0.95
  blend: screen              // normal | add | screen | multiply | overlay
  grain: 0.02
  clip: @clip_shape
  shadow(0, 18, 40, fade(#000000, 0.6))
  glow(14, fade(#65D9FF, 0.35))
  rotate: -3
  translate: (12, 0)
  scale: 1.02
  matrix: (1, 0, 0, 1, 12, 18)
}
```

`dash: [8, 5]` and `dash_offset: -t * 40` create animated dashed strokes.
`arrow: end`, `start`, or `both` adds an arrowhead to lines/paths.

## Paint

```mesh
linear_gradient "sky" {
  angle: 135
  stop(0.0, #07111A)
  stop(0.55, #0E2A33)
  stop(1.0, #101B28)
}

radial_gradient "halo" {
  center: (0.5, 0.5)
  radius: 0.5
  stop(0.0, fade(#65D9FF, 0.35))
  stop(1.0, fade(#65D9FF, 0.0))
}

conic_gradient "dial" { center: (0.5, 0.5) angle: 0 stop(0, #65D9FF) stop(1, #A78BFA) }
noise_gradient "grain" { scale: 4 octaves: 4 persistence: 0.5 seed: 7 stop(0, #111111) stop(1, #1C1C1C) }
```

`mesh_gradient` and `diffusion_gradient` use `point(u, v, color, weight)` in
normalized 0..1 space. Use `linear_gradient`/`radial_gradient` for most work.

## Expressions and anchors

Built-in scene values: `width`, `height`, `t`, `time`, `PI`, `TAU`, `min_dim`,
`max_dim`.

```mesh
let pad = width * 0.05
let progress = 0.5 + 0.5 * sin(t * 90)
let x = @card.left + 24
let y = @card.top + 32
let w = @card.width - 48
```

Node anchors: `@id.left`, `.right`, `.top`, `.bottom`, `.width`, `.height`,
`.center_x`, `.center_y`, `.center`, `.top_left`, `.top_right`,
`.bottom_left`, `.bottom_right`, `.left_center`, `.right_center`,
`.top_center`, `.bottom_center`.

Math: `sin`/`cos`/`tan` take degrees; `sin_rad`/`cos_rad` take radians.
Also available: `sqrt abs floor ceil round fract mod min max pow clamp lerp
smoothstep polar_x polar_y noise`.

Color: `mix(c1, c2, t)`, `lighten(c, amount)`, `darken(c, amount)`,
`fade(c, alpha)`, `oklch(l, c, h, a)`, `oklab(...)`, `rgb(...)`.

## Motion

`t` is seconds. Use `oscillate(t, speed_hz, min, max, phase_deg)` for a smooth
loop. Use `pulse`, `wave`, `saw`, `ping_pong` for other rhythms. Use
`spring(t, start, from, to, stiffness, damping)`, `ease_in_out`, `ease_out_cubic`
or `ease_out_back` for entrances. See the `mesh-motion` skill for export.

```mesh
circle "orb" {
  center: (400 + oscillate(t, 0.2, -20, 20), 240)
  radius: 18
  fill: @accent
  glow(16, fade(#65D9FF, 0.45))
}
```

## Validation rules

- Unknown properties are hard errors. Do not invent names.
- `font` declarations must appear before the text that uses them.
- `clip: @id` requires a previously registered shape id.
- `shadow`/`glow` may be written as `shadow(...)`/`glow(...)` or with a colon.
- `stroke` automatically gets a 1.5 width if no width is supplied.
- Text fill defaults to black; set `fill` explicitly for dark scenes.
- A `.meshb` file is compiled output; render it, but edit `.mesh` source.
- MP4 export is optional; `meshc animate out.mp4` needs FFmpeg libraries.

## References

Load only what the task needs:

- `references/language.md` — complete property and syntax reference.
- `references/recipes.md` — cards, dashboards, gauges, charts, text clipping.
- `references/tooling.md` — CLI, validation, binary, video, CI.
- `examples/starter.mesh` — small validated scene.
- `examples/dashboard.mesh` — repeat, booleans, gradients, animation.

Before returning a `.mesh` file, run `./build/meshc validate` on it and fix every
parse error.
