---
name: godot-ui-i18n-accessibility
description: Build responsive Godot Control interfaces with themes, focus, localization, accessibility, and multi-resolution verification.
---

# UI, Internationalization, and Accessibility

## When to use
Use for menus, HUDs, forms, themes, localization, keyboard/controller focus, or accessibility.

## Inspect first
- Read viewport/stretch settings, theme ownership, localization files, input actions, and the current Control hierarchy.
- Search for an existing reusable component before adding a new one.

## Recommended order
1. Express hierarchy and responsive layout with Containers, anchors, and size flags.
2. Centralize visual decisions in Theme resources; keep text and behavior out of decorative wrappers.
3. Add focus order, controller/keyboard navigation, readable labels, and localized strings.
4. Patch the smallest behavior script only where native signals and controls are insufficient.

## Validate
Capture representative desktop/mobile sizes and test long translations, RTL where required, focus traversal, input methods, contrast, clipping, and runtime errors.

## Common failures
Absolute positioning inside responsive screens, duplicated theme overrides, untranslated literals, invisible focus, and text that only fits one locale.
