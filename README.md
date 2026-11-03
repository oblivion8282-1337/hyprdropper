# hyprdropper

**niri-like drag & drop for Hyprland's scrolling layout.** · [Deutsch](README.de.md)

When you drag a tiled window with `bindm` (e.g. `SUPER + left mouse`), Hyprland normally floats it right
away, the layout closes the hole and the neighbouring windows jump. hyprdropper keeps the layout still and
shows where the window will land:

- **Nothing jumps while dragging.** The window stays in the layout; its old spot stays free as a gap
  (outlined with a dashed frame).
- **Insert bar.** A vertical bar between two columns means "new column here", a horizontal bar above/below a
  window means "into this column". The bar grows in on every new target and its glow breathes slowly.
- **Soft light.** The bar casts a soft halo (Hyprland's own shadow shader) onto the neighbouring windows, whose
  borders light up in the accent colour.
- **The window follows the cursor**, shrunk to 60 % around the grab point and slightly transparent, and on
  release it flies from the cursor to its new place.
- **Column width is kept** when a window is moved.
- **Edge scrolling:** move to the outer screen edge and the view scrolls (not at edges shared with another
  monitor). The **mouse wheel** scrolls the view while dragging.
- **Multi-monitor:** preview and drop work on other monitors and on empty workspaces.
- **Esc** cancels; releasing over the gap puts everything back.

Floating, grouped, fullscreen/maximised windows and other layouts behave exactly as without the plugin.

## Requirements

- Hyprland **0.57.x** with `general.layout = "scrolling"`
- Mouse move bind, e.g. Lua: `hl.bind("SUPER + mouse:272", hl.dsp.window.drag(), { mouse = true })`

## Install

### With hyprpm (recommended)

```sh
hyprpm update
hyprpm add https://github.com/oblivion8282-1337/hyprdropper
hyprpm enable hyprdropper
```

Load enabled plugins at startup (Lua config):

```lua
hl.on("hyprland.start", function() hl.dispatch(hl.dsp.exec_cmd("hyprpm reload -n")) end)
```

After Hyprland updates run `hyprpm update`.

### Manually

Needs the Hyprland headers (on Arch they ship with the `hyprland` package), `make` and a C++23 compiler.

```sh
git clone https://github.com/oblivion8282-1337/hyprdropper && cd hyprdropper
scripts/install.sh            # builds, runs the unit tests, installs ~/.local/lib/hyprdropper/hyprdropper.so
```

Then add to `hyprland.lua`:

```lua
hl.plugin.load(os.getenv("HOME") .. "/.local/lib/hyprdropper/hyprdropper.so")
```

Plugins have to be rebuilt for every Hyprland update. On Arch/CachyOS `scripts/install-pacman-hook.sh` installs
a pacman hook that does this automatically (`--remove` to uninstall it).

## Configuration

Everything is a normal config value; only set what you want to change. The order relative to
`hl.plugin.load(...)` does not matter and changes apply on config reload.

```lua
hl.config({
    plugin = {
        hyprdropper = {
            enabled         = true,   -- off = Hyprland's normal drag
            -- insert bar
            use_theme_color = true,   -- accent from decoration.glow.color (see below)
            col             = { bar = "rgba(42a5f5ff)" }, -- own colour
            bar_width       = 6,      -- px
            bar_animation   = true,   -- grow-in + breathing glow
            glow_strength   = 1.0,    -- soft halo around the bar, 0 = off
            neighbor_glow   = true,   -- light up the borders of the windows next to the bar
            -- dragged window
            drag_scale      = 0.6,    -- size while dragging, 1 = unchanged
            drag_opacity    = 0.8,
            show_gap        = true,   -- dashed outline of the old spot
            -- scrolling while dragging
            edge_size       = 48,     -- edge zone width in px
            edge_speed      = 1800,   -- max edge-scroll speed in px/s
            wheel_step      = 0.25,   -- per wheel step, fraction of the screen width
        },
    },
})
```

Single option: `hl.config({ ["plugin.hyprdropper.glow_strength"] = 0.6 })` ·
query: `hyprctl getoption plugin:hyprdropper:glow_strength` ·
hyprlang config: `plugin { hyprdropper { glow_strength = 0.6 } }`.

**Colour:** without any setting the bar follows the theme accent (`decoration.glow.color`). Setting only
`col.bar` uses that colour; an explicitly set `use_theme_color` always decides.

How far the mouse has to move before a click becomes a drag is Hyprland's `binds.drag_threshold`.

## Animations

The shrinking of the dragged window and the bar's grow-in/glide run on Hyprland's animation system, on own
leaves that inherit from `windows` (so by default they move like your windows). `animations.enabled = false`
turns them off as well. Tune them like any other animation:

```lua
hl.animation({ leaf = "hyprdropper",      speed = 4, bezier = "default" })   -- both
hl.animation({ leaf = "hyprdropperScale", speed = 3, bezier = "myCurve" })   -- window shrink / grow back
hl.animation({ leaf = "hyprdropperBar",   speed = 5, bezier = "default" })   -- bar grow-in and glide
```

The landing flight uses Hyprland's normal `windowsMove` animation; the breathing glow is a loop and not part of
the animation tree (`bar_animation = false` turns grow-in and breathing off).

## Version safety

The plugin hooks into Hyprland internals that change between minor versions. It therefore only activates on
versions it has been tested with (currently **0.57**). On any other version it stays inactive, Hyprland keeps
working normally and you get a notification. To run it anyway, add the version (e.g. `0.58`) on its own line to
`~/.config/hyprdropper/approved-versions` and reload the plugin. `scripts/approve.sh` does this only after the
full integration test suite passed on your machine.

## Troubleshooting

`hyprctl hyprdropper` shows the state: whether the hooks are active, the current drag, the drop target, the
bar, why the last drag was not handled (`last_reject`) and per-function timings (`perf ...`,
reset with `hyprctl hyprdropper-perfreset`). Turn it off instantly with
`hyprctl eval 'hl.config({ ["plugin.hyprdropper.enabled"] = false })'` or unload it with `hyprctl plugin unload <path>`.

## Development

```sh
make                      # build/hyprdropper.so
make test                 # unit tests of the drop logic
scripts/build-tester.sh   # build Hyprland's hyprtester plugin (input simulation) for the installed version
python3 tests/headless/run_tests.py --plugin build/hyprdropper.so \
    --tester ~/.cache/hyprdropper/tester-$(pkg-config --modversion hyprland)/hyprtestplugin.so
python3 tests/headless/bench.py --plugin build/hyprdropper.so --tester <same>
```

The integration tests run a throw-away Hyprland nested in a headless `mutter` with its own D-Bus session, so
your session is never touched. They need `mutter`, `alacritty` and `python3`.

## License

BSD-3-Clause, see [LICENSE](LICENSE).
