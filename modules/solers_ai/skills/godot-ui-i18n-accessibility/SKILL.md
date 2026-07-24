---
name: godot-ui-i18n-accessibility
description: Godot 4 Control layout with Containers, Theme ownership, focus order, TranslationServer/locale, accessibility labels, and multi-resolution verification.
---

# UI, Internationalization, and Accessibility

## When to use
Use for menus, HUDs, forms, themes, localization, keyboard/controller focus, RTL, or accessibility. World-space 2D/3D labels are not a substitute for Control trees.

## Facts
| Piece | Role |
|-------|------|
| Containers | `VBox`/`HBox`/`Grid`/`Margin`/`PanelContainer` — primary layout authority |
| Anchors / size flags | Responsive fill; avoid absolute pixel layout for full screens |
| `Theme` / `ThemeTypeVariation` | Central colors, fonts, styleboxes — not per-node copy-paste |
| Focus | `focus_mode`, neighbor paths, `grab_focus()`; Controllers use ui_* InputMap |
| Text | Prefer `tr("KEY")` / `TranslationServer`; CSV or `.translation` resources |
| Stretch | `display/window/stretch/*` — UI scale vs game viewport |
| A11y | Accessible names/descriptions on Controls where the platform exposes them |

## Laws
- Layout lives in Containers + anchors; scripts do not set `position` every frame for responsive UI.
- One Theme owner per visual language; overrides are exceptions, not the system.
- Every actionable control is reachable by keyboard/gamepad focus.
- User-facing strings are translation keys — not hard-coded English in shipping UI.

## Recipes
**Screen shell:** root `Control` full-rect → `MarginContainer` → content `VBoxContainer`.
**Modal:** `CanvasLayer` + dimmer `ColorRect` mouse-filter stop + focused default button.
**Locale swap:** `TranslationServer.set_locale(code)` then refresh any non-auto text.

## Traps
| Wrong | Correct |
|-------|---------|
| Absolute positioning for whole menus | Containers + size flags |
| Duplicating StyleBox on every button | Theme / type variation |
| Focus stuck on hidden controls | `visible` + `focus_mode` discipline; re-grab on show |
| Text that fits EN only | Test long DE/FR strings and RTL if required |
| Putting HUD in world `Node2D` without CanvasLayer | Separate UI CanvasLayer |

## Verify
1. `scene.inspect` Control tree, Theme paths, stretch settings.
2. `runtime.control` at 16:9 and tall mobile sizes; tab/gamepad through focus.
3. `viewport.capture` for clipping/contrast; swap locale and re-capture.
4. `runtime.observe` for missing translations / layout errors.
