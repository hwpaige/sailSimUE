# GPU A/B log

Stuck render decisions after a measured Prefer-ON (or equivalent) A/B. One row per stick.

| Date | Scene | Change | Design | Frame | Stick | Caveat |
|------|-------|--------|--------|-------|-------|--------|
| 2026-09-21 | Prefer-ON harbor | `r.Lumen.Reflections.DownsampleFactor` 2 → 4 (Allow stayed 1) | PASS (`ocean-dsf4-editor-hull-20260921-232036.png`) | ~27.9 ms / ~36 FPS vs ~32 ms stick bar (settled ~27.7–27.9) | YES | moored=0 that window |
