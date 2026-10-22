-- Hyprland 0.55+ window rules for WeTypeX.

hl.window_rule({
  name = "wetypex-settings",
  match = { class = "^fcitx5-wetypex-settings$" },
  float = true,
  size = "715 569",
  center = true,
})

hl.window_rule({
  name = "wetypex-transfer",
  match = { class = "^fcitx5-wetypex-transfer$" },
  float = true,
  size = "317 519",
  center = true,
})

hl.window_rule({
  name = "wetypex-ai",
  match = { class = "^fcitx5-wetypex-ai$" },
  float = true,
  size = "440 566",
  center = true,
})
