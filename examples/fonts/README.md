# Bundled example font

These files are **Open Sans 3.003** (Regular, Bold, Italic), licensed under the
SIL Open Font License 1.1. See `OFL.txt`.

They are bundled only so that `examples/09_typography_specimen.mesh` can
demonstrate local TTF import and remain self-contained:

```mesh
font "display" {
  src: "fonts/OpenSans-Bold.ttf"
  family: "Open Sans"
}
```
