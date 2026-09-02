---
name: godot-ui-i18n-accessibility
description: Godot 4 Control layout, containers, themes, focus, internationalization, bidirectional text, and accessibility.
---

# Godot UI, Internationalization, and Accessibility

## Scope
Use when working with Control interfaces, layout, themes, focus, localized content, text direction, or accessibility.

## Native model
- Control defines a rectangle using anchors and offsets relative to its parent Control or Viewport, plus minimum-size and
  size-flag constraints.
- Container owns the layout of its child Controls while they are children of that Container. Each Container type applies
  a specific arrangement using child minimum sizes and size flags.
- Theme resources provide typed colors, constants, fonts, font sizes, icons, and StyleBoxes through hierarchy and type
  lookup; local theme overrides take precedence.
- GUI input is delivered to Controls through Viewport hit testing and `_gui_input()`. Mouse filtering controls propagation;
  keyboard and controller input use the current focus owner and focus-neighbor graph.
- TranslationServer selects locale and resolves translations. Project translation resources and domains define available
  messages.
- Layout direction and text direction support left-to-right and right-to-left content. Control accessibility properties
  expose names, descriptions, roles, values, and relationships to assistive technology where supported.
- Viewport content scale and stretch settings map project UI coordinates to window and display sizes.

## Compatibility and prerequisites
- Font coverage, shaping, locale resources, accessibility backend, input method, and platform window behavior vary by target.
- Container layout, theme inheritance, translation, and focus operate on the instantiated Control tree, not isolated files.

## Authoritative state
The live Control tree and rectangles, minimum sizes, Container assignment, resolved Theme items, focus owner, input events,
TranslationServer locale and messages, accessibility output, Viewport scale, and rendered interface are authoritative.

## Official references
- https://docs.godotengine.org/en/latest/tutorials/ui/size_and_anchors.html
- https://docs.godotengine.org/en/latest/tutorials/ui/gui_containers.html
- https://docs.godotengine.org/en/latest/tutorials/ui/gui_skinning.html
- https://docs.godotengine.org/en/latest/tutorials/ui/gui_navigation.html
- https://docs.godotengine.org/en/latest/tutorials/i18n/internationalizing_games.html
- https://docs.godotengine.org/en/latest/tutorials/ui/gui_accessibility.html
- https://docs.godotengine.org/en/latest/tutorials/rendering/multiple_resolutions.html
