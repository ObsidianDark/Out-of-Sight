## Out of Sight

A Geode mod for Geometry Dash that hides off-screen level objects and reduces
the visual detail of objects that are scaled down, to help with performance
in large/detailed levels.

## Examples

**Debug overlay** — the red rectangle shows the current culling bounds.
Objects outside it are hidden; objects inside are rendered normally.

**Hitboxes untouched** — with hitboxes enabled, geometry and collision are
unaffected by culling.

**Tight margin** — camera margin set to `-100px`, culling objects slightly
before they'd normally leave the screen.

## Features

- **Hitboxes are untouched** — this mod does not change any in-game
  hitboxes or collision, so it's not cheating.
- **Camera culling** — objects outside the current view are hidden
  (`setVisible(false)`) instead of being rendered every frame.
- **Level-of-detail (LOD)** *(work in progress)* — objects that are scaled
  below a threshold (e.g. via a scale trigger) get simplified: their glow is
  hidden and their opacity is reduced slightly. Geometry, scale, and
  collision are never touched, so gameplay is unaffected. This feature is
  still being worked on and may not behave correctly in all cases.
- **Triggers are never touched** — by default, trigger objects are excluded
  from both culling and LOD so gameplay logic can't be affected.
- Optional debug overlay showing the current culling bounds.

## Settings

| Setting | Description |
|---|---|
| `enable-culling` | Turn camera culling on/off. |
| `cell-size` | Size (in pixels) of the spatial grid used for culling. Smaller = more precise but more buckets to check; larger = fewer buckets but more objects per bucket. |
| `culling-margin` | Extra padding (in pixels) around the screen edge before an object is culled. |
| `ignore-triggers` | If enabled, trigger objects are never hidden or LOD'd. |
| `enable-lod` | Turn the level-of-detail system on/off. **(WIP — see Known limitations.)** |
| `lod-threshold` | Object scale (as a fraction of normal, e.g. `0.5`) below which LOD kicks in. |
| `lod-hysteresis` | Buffer around the threshold (e.g. `0.15` = 15%) to prevent flickering between LOD states. |
| `debug-culling` | Draws a red rectangle showing the current culling bounds. |

## Building

This is a standard [Geode](https://geode-sdk.org/) mod. With the Geode CLI
installed:

```sh
geode build
```

See the [Geode docs](https://docs.geode-sdk.org/) for setup instructions if
you're building for the first time.

## Notes / Known limitations

- **LOD is a work in progress** and may not behave correctly yet — glow or
  opacity changes may not apply consistently in all cases. Camera culling
  itself is stable; LOD is the part still being refined.
- LOD is applied via glow-sprite visibility and opacity only — no scaling —
  so collision and aspect ratio are never affected.
- Background/parallax layers rendered outside `m_objectLayer` are not
  affected by this mod.
- This is a really simple mod, so in the future versions expect more from it...

## Things to add

- Fixing parallax renders.
- Stabilizing the LOD system.

## Special Thanks

- "Sodium" mod from Minecraft.
- "Distant Horizons" mod from Minecraft.
