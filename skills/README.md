# Mesh Agent Skills

Portable Agent Skills (`SKILL.md` + YAML frontmatter) for writing Mesh without
reading the C++ implementation. Each skill is a self-contained directory with a
`SKILL.md` frontmatter description and optional references, examples and
scripts.

| Skill | Use |
| :--- | :--- |
| [`mesh/`](mesh/SKILL.md) | Core `.mesh` authoring: document structure, shapes, paint, anchors, layout, expressions, validation |
| [`mesh-typography/`](mesh-typography/SKILL.md) | Font declarations, shaping, fallback, baseline, wrapping and text-as-clip |
| [`mesh-motion/`](mesh-motion/SKILL.md) | Time expressions, loops, easing/spring, deterministic stills and MP4/GIF export |

## Install

Copy the skill directories into the skills folder used by your agent:

```bash
# OpenCode
cp -R skills/mesh skills/mesh-typography skills/mesh-motion ~/.opencode/skills/

# Claude Code
mkdir -p ~/.claude/skills
cp -R skills/mesh skills/mesh-typography skills/mesh-motion ~/.claude/skills/
```

The main `mesh` skill is the entry point. The two focused skills are optional
expansions for font-heavy or motion-heavy work.

## Validate

From the Mesh repository root:

```bash
bash skills/mesh/scripts/validate-examples.sh
```

This runs `meshc validate` on every bundled example. The same check runs in CI.

## Writing a new skill

- Directory name must match the `name` frontmatter field.
- `description` must say what the skill does and when it should be loaded; this
  is the routing text.
- Keep `SKILL.md` lean. Move long tables and rarely used detail into
  `references/`.
- Every `.mesh` example must pass `meshc validate`.
- Use English only.
