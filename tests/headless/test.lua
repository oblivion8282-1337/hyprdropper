-- Minimal config for the headless integration test. Mirrors the parts of the real config that matter.

hl.monitor({ output = "WAYLAND-1", mode = "1920x1080@60", position = "0x0", scale = "1" })
hl.monitor({ output = "HEADLESS-2", mode = "1920x1080@60", position = "1920x540", scale = "1.333333" })
hl.monitor({ output = "", mode = "1920x1080@60", position = "auto", scale = "1" })

hl.config({
    general = {
        gaps_in = 5,
        gaps_out = 10,
        border_size = 2,
        layout = "scrolling",
    },
    scrolling = {
        column_width = 0.5,
        fullscreen_on_one_column = false,
        follow_focus = true,
        follow_min_visible = 1.0,
        direction = "right",
    },
    animations = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    -- the donation/news popups float over the test area and would catch simulated clicks
    ecosystem = { no_donation_nag = true, no_update_news = true },
    binds = { drag_threshold = 10 },
    decoration = { glow = { color = "rgba(42a5f5ee)" } },
})

hl.bind("mouse:272", hl.dsp.window.drag(), { mouse = true })
hl.bind("SUPER + mouse_down", hl.dsp.focus({ workspace = "e+1" }))
hl.bind("SUPER + mouse_up", hl.dsp.focus({ workspace = "e-1" }))
