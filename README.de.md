# hyprdropper

**Ziehen wie in niri für Hyprlands Scrolling-Layout.** · [English](README.md)

Zieht man ein gekacheltes Fenster per `bindm` (z. B. `SUPER` + linke Maustaste), macht Hyprland es normalerweise
sofort schwebend, das Layout schließt die Lücke und die Nachbarfenster springen. hyprdropper lässt das Layout
stehen und zeigt, wo das Fenster landen wird:

- **Nichts springt beim Ziehen.** Das Fenster bleibt im Layout, seine alte Stelle bleibt als Lücke frei
  (gestrichelt umrandet).
- **Einfügebalken.** Senkrecht zwischen zwei Spalten heißt „neue Spalte hier“, waagerecht über/unter einem
  Fenster heißt „in diese Spalte“. Bei jedem neuen Ziel zieht er aus der Mitte auf, sein Leuchten atmet langsam.
- **Weiches Licht.** Der Balken wirft einen weichen Lichthof (Hyprlands eigener Schatten-Shader) auf die
  Nachbarfenster, deren Rahmen in der Akzentfarbe aufleuchten.
- **Das Fenster folgt der Maus**, auf 60 % um die Greifstelle verkleinert und leicht transparent, und fliegt
  beim Loslassen von der Maus an seinen neuen Platz.
- **Die Spaltenbreite bleibt erhalten.**
- **Rand-Scrollen:** am äußeren Bildschirmrand scrollt die Ansicht mit (nicht an Rändern, an die ein anderer
  Monitor grenzt). Das **Mausrad** scrollt die Ansicht beim Ziehen.
- **Mehrere Monitore:** Vorschau und Ablegen funktionieren auch dort, auch auf leeren Workspaces.
- **Esc** bricht ab, Loslassen über der Lücke stellt alles wieder her.

Schwebende, gruppierte, Vollbild-/maximierte Fenster und andere Layouts verhalten sich wie ohne Plugin.

## Voraussetzungen

- Hyprland **0.57.x** mit `general.layout = "scrolling"`
- Eine Maus-Bewegen-Belegung, z. B. Lua: `hl.bind("SUPER + mouse:272", hl.dsp.window.drag(), { mouse = true })`

## Installation

### Mit hyprpm (empfohlen)

```sh
hyprpm update
hyprpm add https://github.com/oblivion8282-1337/hyprdropper
hyprpm enable hyprdropper
```

Aktivierte Plugins beim Start laden (Lua-Config):

```lua
hl.on("hyprland.start", function() hl.dispatch(hl.dsp.exec_cmd("hyprpm reload -n")) end)
```

Nach Hyprland-Updates `hyprpm update` ausführen.

### Von Hand

Braucht die Hyprland-Header (auf Arch im Paket `hyprland` enthalten), `make` und einen C++23-Compiler.

```sh
git clone https://github.com/oblivion8282-1337/hyprdropper && cd hyprdropper
scripts/install.sh            # baut, führt die Unit-Tests aus, installiert ~/.local/lib/hyprdropper/hyprdropper.so
```

Dann in die `hyprland.lua`:

```lua
hl.plugin.load(os.getenv("HOME") .. "/.local/lib/hyprdropper/hyprdropper.so")
```

Plugins müssen nach jedem Hyprland-Update neu gebaut werden. Auf Arch/CachyOS richtet
`scripts/install-pacman-hook.sh` einen pacman-Hook ein, der das automatisch macht (`--remove` entfernt ihn).

## Einstellungen

Alles sind normale Config-Werte; nur eintragen, was abweichen soll. Die Reihenfolge zu `hl.plugin.load(...)` ist
egal, Änderungen greifen beim Neuladen der Config.

```lua
hl.config({
    plugin = {
        hyprdropper = {
            enabled         = true,   -- aus = Hyprlands normales Ziehen
            -- Balken
            use_theme_color = true,   -- Akzentfarbe aus decoration.glow.color (siehe unten)
            col             = { bar = "rgba(42a5f5ff)" }, -- eigene Farbe
            bar_width       = 6,      -- Dicke in px
            bar_animation   = true,   -- Aufziehen + atmendes Leuchten
            glow_strength   = 1.0,    -- weicher Lichthof um den Balken, 0 = aus
            neighbor_glow   = true,   -- Rahmen der Nachbarfenster leuchten auf
            -- gezogenes Fenster
            drag_scale      = 0.6,    -- Größe beim Ziehen, 1 = unverändert
            drag_opacity    = 0.8,    -- Deckkraft
            show_gap        = true,   -- alte Stelle gestrichelt umranden
            -- Scrollen beim Ziehen
            edge_size       = 48,     -- Breite der Rand-Zone in px
            edge_speed      = 1800,   -- max. Rand-Scroll-Geschwindigkeit in px/s
            wheel_step      = 0.25,   -- Mausrad-Schritt, Anteil der Bildschirmbreite
        },
    },
})
```

Einzelne Option: `hl.config({ ["plugin.hyprdropper.glow_strength"] = 0.6 })` ·
abfragen: `hyprctl getoption plugin:hyprdropper:glow_strength` ·
hyprlang-Config: `plugin { hyprdropper { glow_strength = 0.6 } }`.

**Farbe:** Ohne Angaben folgt der Balken der Theme-Akzentfarbe (`decoration.glow.color`). Wer nur `col.bar`
setzt, bekommt diese Farbe; ein ausdrücklich gesetztes `use_theme_color` entscheidet immer.

Ab wann ein Klick zum Ziehen wird, regelt Hyprlands `binds.drag_threshold`.

## Animationen

Das Verkleinern des gezogenen Fensters sowie Aufziehen und Gleiten des Balkens laufen über Hyprlands
Animationssystem, auf eigenen Einträgen unter `windows` (standardmäßig bewegen sie sich also wie deine Fenster).
`animations.enabled = false` schaltet sie mit ab. Einstellen wie jede andere Animation:

```lua
hl.animation({ leaf = "hyprdropper",      speed = 4, bezier = "default" })   -- beides
hl.animation({ leaf = "hyprdropperScale", speed = 3, bezier = "meineKurve" }) -- Fenster verkleinern / zurückwachsen
hl.animation({ leaf = "hyprdropperBar",   speed = 5, bezier = "default" })   -- Balken aufziehen und gleiten
```

Der Landeflug nutzt Hyprlands normale `windowsMove`-Animation; das atmende Leuchten ist eine Schleife und nicht
Teil des Animationsbaums (`bar_animation = false` schaltet Aufziehen und Atmen ab).

## Versionssicherheit

Das Plugin greift in Hyprland-Interna ein, die sich zwischen Minor-Versionen ändern. Es aktiviert sich deshalb
nur auf getesteten Versionen (derzeit **0.57**). Auf anderen bleibt es inaktiv, Hyprland läuft normal weiter,
und es gibt eine Benachrichtigung. Wer es trotzdem nutzen will, trägt die Version (z. B. `0.58`) als eigene
Zeile in `~/.config/hyprdropper/approved-versions` ein und lädt das Plugin neu. `scripts/approve.sh` macht das
erst, nachdem die komplette Integrationstest-Suite auf dem eigenen Rechner bestanden hat.

## Diagnose

`hyprctl hyprdropper` zeigt den Zustand: aktive Hooks, laufender Drag, Ziel, Balken, warum der letzte Drag nicht
übernommen wurde (`last_reject`) und Zeitmessungen pro Funktion (`perf ...`, zurücksetzen mit
`hyprctl hyprdropper-perfreset`). Sofort ausschalten:
`hyprctl eval 'hl.config({ ["plugin.hyprdropper.enabled"] = false })'`, oder entladen mit
`hyprctl plugin unload <pfad>`.

## Entwicklung

```sh
make                      # build/hyprdropper.so
make test                 # Unit-Tests der Drop-Logik
scripts/build-tester.sh   # Hyprlands hyprtester-Plugin (Eingabesimulation) für die installierte Version bauen
python3 tests/headless/run_tests.py --plugin build/hyprdropper.so \
    --tester ~/.cache/hyprdropper/tester-$(pkg-config --modversion hyprland)/hyprtestplugin.so
python3 tests/headless/bench.py --plugin build/hyprdropper.so --tester <dasselbe>
```

Die Integrationstests laufen in einem Wegwerf-Hyprland, verschachtelt in einem headless `mutter` mit eigener
D-Bus-Sitzung; die eigene Session bleibt unberührt. Sie brauchen `mutter`, `alacritty` und `python3`.

## Lizenz

BSD-3-Clause, siehe [LICENSE](LICENSE).
