---
name: mesh-motion
description: "Use when adding animation to a Mesh .mesh scene or exporting Mesh motion: time-based expressions, oscillate/pulse/spring easing, animated dashes, repeat-driven charts, deterministic stills, MP4/GIF/sprite-sheet export, or choosing the right animation timestamp. Complements the core mesh skill."
---

# Mesh motion and export

Mesh animation is **expressions over `t`**. There is no `animate {}` block and no
wall-clock state. Every exporter evaluates a deterministic timestamp, so the
same source produces the same frame.

## Time model

- `t` and `time` are seconds.
- `render --time 2.4` evaluates exactly `t = 2.4`.
- `animate` evaluates frames at `frame / fps`.
- Use `oscillate(t, speed_hz, min, max, phase_deg)` for loops.
- Use `ease_*` and `spring` for entrances and one-shot motion.

## Motion functions

```mesh
let bob = oscillate(t, 0.25, -8, 8, 0)
let fade = pulse(t, 0.6)
let sweep = saw(t, 0.2, 0)
let ping = ping_pong(t, 0.35)
let entrance = ease_out_cubic(t, 0.2, 0.8, 40, 0)
let settle = spring(t, 0.3, 120, 0, 180, 22)
let wave = sin(t * 90)                  // degrees
let wave_rad = sin_rad(t * 1.57)        // radians
```

Available: `oscillate`, `wave`, `pulse`, `saw`, `ping_pong`, `ease_in_out`,
`ease_out_cubic`, `ease_out_back`, `spring`.

Colors can animate too:

```mesh
let tint = mix(#65D9FF, #A78BFA, pulse(t, 0.35))
let glow_tint = fade(tint, 0.4)
```

## Animated properties

Any numeric property can use an expression: position, size, radius, opacity,
rotation, stroke width, dash offset, gradient geometry, and text size.

```mesh
rect "meter" {
  x: 48
  y: 260
  width: oscillate(t, 0.18, 120, 520, 0)
  height: 8
  radius: 4
  fill: @accent
}

circle "orb" {
  center: (400 + oscillate(t, 0.2, -24, 24), 240)
  radius: 16
  fill: @accent
  glow(14, fade(#65D9FF, 0.4))
}

line "scanline" {
  from: (48, 320)
  to: (752, 320)
  stroke: fade(#FFFFFF, 0.25)
  stroke_width: 1
  dash: [6, 8]
  dash_offset: -t * 40
}
```

Use `rotate: t * 30` for continuous rotation. For a pivot other than the shape
center, combine `translate` and `rotate` or animate the center coordinates
directly.

## Repeat-driven motion

```mesh
repeat "bars" {
  count: 18
  let h = oscillate(t, 0.14, 24, 130, i * 22)
  let x = 48 + i * 38
  rect {
    x: x
    y: 260 - h
    width: 20
    height: h
    radius: 10
    fill: mix(#65D9FF, #A78BFA, rep_t)
  }
}
```

`i` is the index and `rep_t` is the normalized index. The phase offset
`i * 22` makes the bars move as a wave rather than in sync.

## Deterministic stills

Rendering a still is the fastest way to debug motion:

```bash
meshc render scene.mesh -o t0.png --aa 1 --time 0
meshc render scene.mesh -o t2.png --aa 1 --time 2.4
```

Choose a hero timestamp where the composition is complete. Do not hide required
content behind an entrance animation; the still may be captured before it
arrives.

## Video export

```bash
meshc animate scene.mesh -o scene.mp4 --duration 6 --fps 30 --width 1600 --height 1000 --aa 4
```

MP4 export uses native H.264 when the build has FFmpeg libraries:

- H.264 High, yuv420p, CRF 18, preset medium;
- no audio, `+faststart`;
- deterministic frames at `frame / fps`;
- odd canvas sizes are padded to even dimensions.

Other outputs:

```bash
meshc animate scene.mesh -o scene.gif --duration 3 --fps 20
meshc animate scene.mesh -o sheet.png --duration 3 --fps 20
meshc compile scene.mesh --target svg  -o scene.svg
meshc compile scene.mesh --target html -o scene.html
```

If MP4 is unavailable, `meshc` reports the missing FFmpeg backend; PNG, GIF,
sprite-sheet, SVG and HTML still work.

## Motion quality

- Prefer one or two slow motions over many competing loops.
- Keep loops at different speeds (e.g. 0.12 Hz body, 0.35 Hz pulse, 0.8 Hz blink)
  so they do not march in lockstep.
- Use `oscillate` for breathing, floating and ambient drift.
- Use `spring`/`ease_*` for one-time entrances; they settle at the target.
- Animate only properties that matter to the story.
- Inspect the still at `t = 0` and at the hero time; check both.

## Example

`examples/orbit.mesh` is a validated motion scene with an orbit, a pulsing
meter, a dash sweep and a repeat-driven chart.
