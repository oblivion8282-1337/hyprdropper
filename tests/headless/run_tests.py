#!/usr/bin/env python3
"""Integration tests for hyprdropper.

Runs a throw-away Hyprland instance nested in a headless mutter (own D-Bus session), so the user's
real session is never touched. Input is simulated with Hyprland's own hyprtester plugin.

usage: run_tests.py --plugin build/hyprdropper.so --tester /path/to/hyprtestplugin.so [--keep]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")

PASSED, FAILED = 0, []


class Instance:
    def __init__(self, plugin, tester):
        self.plugin = os.path.abspath(plugin)
        self.tester = os.path.abspath(tester)
        self.tmp = tempfile.mkdtemp(prefix="hyprdropper-test-")
        self.parent_name = f"dptest-parent-{os.getpid()}"
        self.procs = []
        self.sig = None

    # -- lifecycle
    def start(self):
        env = dict(os.environ)
        for k in ("WAYLAND_DISPLAY", "DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE"):
            env.pop(k, None)
        mutter = subprocess.Popen(
            ["dbus-run-session", "--", "mutter", "--headless", "--wayland", "--no-x11", "--virtual-monitor", "1920x1080", "--wayland-display", self.parent_name],
            env=env, stdout=open(os.path.join(self.tmp, "mutter.log"), "w"), stderr=subprocess.STDOUT, start_new_session=True)
        self.procs.append(mutter)
        self._wait(lambda: os.path.exists(os.path.join(RUNTIME, self.parent_name)), 10, "mutter socket")

        xdg = os.path.join(self.tmp, "xdg")
        os.makedirs(os.path.join(xdg, "hyprdropper"))
        with open(os.path.join(xdg, "hyprdropper", "approved-versions"), "w") as f:
            f.write("# test\n0.57\n")

        before = set(os.listdir(os.path.join(RUNTIME, "hypr")))
        # the built-in list of tested versions is ignored so the version gate can be tested via the file
        env2 = dict(env, WAYLAND_DISPLAY=self.parent_name, XDG_CONFIG_HOME=xdg, HYPRLAND_NO_CRASHREPORTER="1", HYPRDROPPER_IGNORE_BUILTIN_VERSIONS="1")
        hl = subprocess.Popen(["Hyprland", "--config", os.path.join(HERE, "test.lua")], env=env2, cwd=self.tmp,
                              stdout=open(os.path.join(self.tmp, "hyprland.log"), "w"), stderr=subprocess.STDOUT, start_new_session=True)
        self.procs.append(hl)
        self.hl = hl

        def found():
            new = set(os.listdir(os.path.join(RUNTIME, "hypr"))) - before
            for s in new:
                if os.path.exists(os.path.join(RUNTIME, "hypr", s, ".socket.sock")):
                    self.sig = s
                    return True
            return False

        self._wait(found, 15, "hyprland socket")
        self._wait(lambda: self.ctl("monitors").startswith("Monitor"), 10, "hyprland ready")
        assert self.ctl("output create headless HEADLESS-2") == "ok"
        time.sleep(0.5)
        assert self.ctl(f"plugin load {self.tester}") == "ok", "tester plugin"
        r = self.ctl(f"plugin load {self.plugin}")
        assert r == "ok", f"hyprdropper plugin: {r}"
        assert "hooks: true" in self.ctl("hyprdropper"), self.ctl("hyprdropper")

    def stop(self):
        if self.sig:
            try:
                self.ctl("dispatch hl.dsp.exit()", timeout=3)
            except Exception:
                pass
        time.sleep(0.5)
        for p in reversed(self.procs):
            if p.poll() is None:
                try:
                    os.killpg(p.pid, 15)
                except ProcessLookupError:
                    pass
        for p in self.procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, 9)
        if self.sig:
            shutil.rmtree(os.path.join(RUNTIME, "hypr", self.sig), ignore_errors=True)

    def alive(self):
        return self.hl.poll() is None

    @staticmethod
    def _wait(cond, timeout, what):
        end = time.time() + timeout
        while time.time() < end:
            try:
                if cond():
                    return
            except Exception:
                pass
            time.sleep(0.1)
        raise RuntimeError(f"timeout waiting for {what}")

    # -- hyprctl
    def ctl(self, args, timeout=5):
        env = dict(os.environ, HYPRLAND_INSTANCE_SIGNATURE=self.sig)
        out = subprocess.run(["hyprctl"] + split(args), env=env, capture_output=True, text=True, timeout=timeout)
        return (out.stdout + out.stderr).strip()

    def jctl(self, what):
        return json.loads(self.ctl(f"-j {what}"))

    def dispatch(self, lua):
        r = self.ctl(f"dispatch {lua}")
        assert r == "ok", f"dispatch {lua}: {r}"

    def eval(self, lua):
        r = self.ctl(f"eval {lua}")
        assert r == "ok", f"eval {lua}: {r}"

    def move(self, x, y):
        self.dispatch(f"hl.dsp.cursor.move({{ x = {x}, y = {y} }})")

    def press(self):
        self.eval("hl.plugin.test.click(272, 1)")

    def release(self):
        self.eval("hl.plugin.test.click(272, 0)")

    def status(self):
        st = {}
        for line in self.ctl("hyprdropper").splitlines():
            if ":" in line:
                k, v = line.split(":", 1)
                st[k.strip()] = v.strip()
        return st

    # -- windows
    def clients(self):
        return [c for c in self.jctl("clients") if c["class"] != "hyprland-donate-screen"]

    def tiled(self, ws=None):
        cs = [c for c in self.clients() if not c["floating"] and (ws is None or c["workspace"]["name"] == ws)]
        return sorted(cs, key=lambda c: (c["at"][0], c["at"][1]))

    def layout(self, ws="1"):
        return [(c["class"], c["at"][0], c["at"][1], c["size"][0], c["size"][1]) for c in self.tiled(ws)]

    def order(self, ws="1"):
        return [c["class"] for c in self.tiled(ws)]

    def win(self, cls):
        for c in self.clients():
            if c["class"] == cls:
                return c
        raise KeyError(cls)

    def center(self, cls, fx=0.5, fy=0.5):
        c = self.win(cls)
        return int(c["at"][0] + c["size"][0] * fx), int(c["at"][1] + c["size"][1] * fy)

    def spawn(self, cls, ws=None):
        n = len(self.clients())
        if ws:
            self.dispatch(f"hl.dsp.focus({{ workspace = '{ws}' }})")
        self.dispatch(f"hl.dsp.exec_cmd('alacritty --config-file /dev/null --class {cls}')")
        self._wait(lambda: len(self.clients()) > n, 10, f"window {cls}")
        time.sleep(0.4)

    def kill_all(self):
        for c in self.clients():
            self.dispatch(f"hl.dsp.window.close({{ window = 'address:{c['address']}' }})")
        self._wait(lambda: len(self.clients()) == 0, 10, "windows closed")
        for c in self.jctl("clients"):
            self.dispatch(f"hl.dsp.window.close({{ window = 'address:{c['address']}' }})")
        time.sleep(0.3)

    def reset(self, classes=("a", "b", "c")):
        """Fresh workspace 1 on WAYLAND-1 with the given windows, view on the first two."""
        if self.status().get("active") == "true":
            self.release()
        self.kill_all()
        self.ctl("reload")
        time.sleep(0.3)
        self.dispatch("hl.dsp.focus({ monitor = 'WAYLAND-1' })")
        self.dispatch("hl.dsp.focus({ workspace = '1' })")
        for c in classes:
            self.spawn(c)
        # bring the first column into view
        self.dispatch(f"hl.dsp.focus({{ window = 'class:{classes[0]}' }})")
        time.sleep(0.3)


def split(s):
    # hyprctl wants the lua code as a single argument after the verb
    parts = s.split(" ", 1)
    if parts[0] == "-j":
        rest = parts[1].split(" ", 1)
        return ["-j"] + rest
    if parts[0] in ("dispatch", "eval"):
        return parts
    return s.split(" ")


def check(name, cond, detail=""):
    global PASSED
    if cond:
        PASSED += 1
        print(f"    ok   {name}")
    else:
        FAILED.append(name)
        print(f"    FAIL {name} {detail}")


def begin_drag(I, cls, fx=0.5, fy=0.5):
    x, y = I.center(cls, fx, fy)
    I.move(x, y)
    I.press()
    I.move(x + 30, y + 10)  # past drag_threshold (10)
    time.sleep(0.05)


# --------------------------------------------------------------------------- scenarios

def t_nothing_moves_and_new_column_after(I):
    I.reset()
    before = I.layout()
    order = I.order()
    first, second = order[0], order[1]
    begin_drag(I, first)
    x, y = I.center(second, 0.9, 0.5)
    I.move(x, y)
    time.sleep(0.2)
    st = I.status()
    check("drag is active and lifted", st.get("active") == "true" and st.get("lifted") == "true", st)
    check("nothing moved while dragging", I.layout() == before, f"{before} vs {I.layout()}")
    check("preview says new column", st.get("drop") == "new_column", st)
    check("bar exists", st.get("bar") != "none", st)
    check("window drawn under cursor (offset)", st.get("offset", "0 0") != "0 0", st)
    width_before = I.win(first)["size"][0]
    I.release()
    time.sleep(0.3)
    o = I.order()
    check("dropped right of the target", o.index(first) == o.index(second) + 1, o)
    check("width kept", abs(I.win(first)["size"][0] - width_before) <= 2, (width_before, I.win(first)["size"][0]))
    check("drag ended", I.status().get("active") == "false")


def t_into_column(I):
    I.reset()
    o = I.order()
    first, second = o[0], o[1]
    begin_drag(I, first)
    x, y = I.center(second, 0.5, 0.25)
    I.move(x, y)
    time.sleep(0.1)
    st = I.status()
    check("preview says into column", st.get("drop") == "into_column", st)
    bar = list(map(float, st["bar"].split())) if st.get("bar", "none") != "none" else None
    check("horizontal bar", bar is not None and bar[2] > bar[3], st)
    I.release()
    time.sleep(0.3)
    f, s = I.win(first), I.win(second)
    check("same column (same x)", abs(f["at"][0] - s["at"][0]) <= 2, (f["at"], s["at"]))
    check("dragged one is above", f["at"][1] < s["at"][1], (f["at"], s["at"]))


def t_own_slot_restores(I):
    I.reset()
    before = I.layout()
    o = I.order()
    begin_drag(I, o[0])
    x, y = I.center(o[0], 0.5, 0.5)
    I.move(x, y)
    st = I.status()
    check("own slot -> restore", st.get("drop") == "restore" and st.get("bar") == "none", st)
    I.release()
    time.sleep(0.3)
    check("layout unchanged after drop on own slot", I.layout() == before, f"{before} vs {I.layout()}")


def t_esc_cancels(I):
    I.reset()
    before = I.layout()
    o = I.order()
    begin_drag(I, o[0])
    x, y = I.center(o[1], 0.9, 0.5)
    I.move(x, y)
    check("would move before Esc", I.status().get("drop") == "new_column")
    I.eval("hl.plugin.test.keybind(1, 0, 9)")   # Esc down (evdev 1 + 8)
    time.sleep(0.1)
    check("Esc ended the drag", I.status().get("active") == "false", I.status())
    I.eval("hl.plugin.test.keybind(0, 0, 9)")   # Esc up
    I.release()
    time.sleep(0.3)
    check("layout unchanged after Esc", I.layout() == before, f"{before} vs {I.layout()}")


def t_click_without_moving(I):
    I.reset()
    before = I.layout()
    o = I.order()
    x, y = I.center(o[0])
    I.move(x, y)
    I.press()
    I.move(x + 3, y + 3)  # below the threshold
    st = I.status()
    check("below threshold: not lifted", st.get("active") == "true" and st.get("lifted") == "false", st)
    I.release()
    time.sleep(0.3)
    check("click does not change the layout", I.layout() == before, f"{before} vs {I.layout()}")


def t_cross_monitor_empty(I):
    I.reset()
    o = I.order()
    w = I.win(o[0])["size"][0]
    begin_drag(I, o[0])
    m = [m for m in I.jctl("monitors") if m["name"] == "HEADLESS-2"][0]
    lw, lh = m["width"] / m["scale"], m["height"] / m["scale"]
    I.move(int(m["x"] + lw / 2), int(m["y"] + lh / 2))  # middle of HEADLESS-2 (scaled)
    time.sleep(0.2)
    st = I.status()
    check("bar on empty other monitor", st.get("drop") == "new_column" and st.get("bar") != "none", st)
    check("dragged window visible on other monitor (offset)", st.get("offset", "0 0") != "0 0", st)
    I.release()
    time.sleep(0.4)
    c = I.win(o[0])
    mons = {m["id"]: m["name"] for m in I.jctl("monitors")}
    check("window moved to HEADLESS-2", mons.get(c["monitor"]) == "HEADLESS-2", (c["monitor"], c["workspace"]))
    check("still tiled", not c["floating"])
    # the column keeps its width *fraction*; the monitors differ in logical size
    lw_src = 1920
    lw_dst = m["width"] / m["scale"]
    expect = w * (lw_dst / lw_src)
    check("width fraction kept on other monitor", abs(c["size"][0] - expect) <= expect * 0.03, (w, c["size"][0], round(expect)))


def t_cross_monitor_next_to(I):
    I.reset()
    # put a window on HEADLESS-2 first
    I.dispatch("hl.dsp.focus({ monitor = 'HEADLESS-2' })")
    I.spawn("d")
    I.dispatch("hl.dsp.focus({ monitor = 'WAYLAND-1' })")
    I.dispatch("hl.dsp.focus({ window = 'class:a' })")
    time.sleep(0.2)
    o = I.order()
    begin_drag(I, o[0])
    x, y = I.center("d", 0.1, 0.5)
    I.move(x, y)
    time.sleep(0.1)
    st = I.status()
    check("bar left of d on other monitor", st.get("drop") == "new_column", st)
    I.release()
    time.sleep(0.4)
    dws = I.win("d")["workspace"]["name"]
    cs = I.tiled(dws)
    names = [c["class"] for c in cs]
    check("dropped left of d", names.index(o[0]) + 1 == names.index("d") if o[0] in names else False, names)


def t_wheel_scrolls_not_workspace(I):
    I.reset(("a", "b", "c", "e"))
    ws_before = I.jctl("activeworkspace")["name"]
    begin_drag(I, I.order()[0])
    xs_before = [c["at"][0] for c in I.tiled("1")]
    I.eval("hl.plugin.test.scroll(15)")
    time.sleep(0.3)
    xs_after = [c["at"][0] for c in I.tiled("1")]
    check("wheel scrolled the tape", xs_after != xs_before, (xs_before, xs_after))
    check("wheel did not switch workspace", I.jctl("activeworkspace")["name"] == ws_before)
    I.eval("hl.plugin.test.keybind(1, 0, 9)")
    I.eval("hl.plugin.test.keybind(0, 0, 9)")
    I.release()


def t_edge_scroll(I):
    I.reset(("a", "b", "c", "e"))
    # scroll so that there is room on both sides
    I.dispatch("hl.dsp.focus({ window = 'class:c' })")
    time.sleep(0.3)
    begin_drag(I, "c")
    # right edge of WAYLAND-1 at y=100: HEADLESS-2 starts at y=540, so nothing is beyond -> scrolls
    xs0 = [c["at"][0] for c in I.tiled("1")]
    I.move(1918, 100)
    time.sleep(0.5)
    xs1 = [c["at"][0] for c in I.tiled("1")]
    check("edge scroll at free right edge", xs1 != xs0, (xs0, xs1))
    # right edge at y=800: HEADLESS-2 is beyond -> no scrolling
    I.move(1918, 800)
    time.sleep(0.15)
    xs2 = [c["at"][0] for c in I.tiled("1")]
    time.sleep(0.4)
    xs3 = [c["at"][0] for c in I.tiled("1")]
    check("no edge scroll where another monitor is beyond", xs2 == xs3, (xs2, xs3))
    # left edge: scrolls back
    I.move(1, 500)
    time.sleep(0.5)
    xs4 = [c["at"][0] for c in I.tiled("1")]
    check("edge scroll at left edge", xs4 != xs3, (xs3, xs4))
    I.eval("hl.plugin.test.keybind(1, 0, 9)")
    I.eval("hl.plugin.test.keybind(0, 0, 9)")
    I.release()


def t_window_closes_during_drag(I):
    I.reset()
    o = I.order()
    begin_drag(I, o[0])
    I.move(*I.center(o[1], 0.9, 0.5))
    I.dispatch(f"hl.dsp.window.close({{ window = 'class:{o[0]}' }})")
    time.sleep(0.5)
    check("compositor alive after window closed mid-drag", I.alive())
    check("drag ended after window closed", I.status().get("active") == "false", I.status())
    I.release()
    time.sleep(0.2)
    check("compositor alive after release", I.alive())


def t_width_preserved_narrow(I):
    I.reset()
    o = I.order()
    I.dispatch(f"hl.dsp.focus({{ window = 'class:{o[0]}' }})")
    I.dispatch("hl.dsp.layout('colresize 0.333')")
    time.sleep(0.3)
    w = I.win(o[0])["size"][0]
    begin_drag(I, o[0])
    I.move(*I.center(o[1], 0.9, 0.5))
    I.release()
    time.sleep(0.3)
    check("narrow width kept after move", abs(I.win(o[0])["size"][0] - w) <= 2, (w, I.win(o[0])["size"][0]))


def t_floating_unchanged(I):
    I.reset(("a",))
    I.dispatch("hl.dsp.focus({ window = 'class:a' })")
    I.dispatch("hl.dsp.window.float({ action = 'set' })")
    time.sleep(0.2)
    begin_drag(I, "a")
    check("floating drag not managed by plugin", I.status().get("active") == "false", I.status())
    I.release()


def t_disabled_falls_back(I):
    I.reset()
    I.eval("hl.config({ ['plugin.hyprdropper.enabled'] = false })")
    o = I.order()
    begin_drag(I, o[0])
    st = I.status()
    check("disabled: plugin not active", st.get("active") == "false", st)
    check("disabled: Hyprland floats the window as usual", I.win(o[0])["floating"])
    I.release()
    time.sleep(0.2)
    I.eval("hl.config({ ['plugin.hyprdropper.enabled'] = true })")


def t_reload_and_workspace_switch_during_drag(I):
    I.reset()
    o = I.order()
    begin_drag(I, o[0])
    I.ctl("reload")
    time.sleep(0.3)
    check("alive after reload mid-drag", I.alive())
    I.dispatch("hl.dsp.focus({ workspace = '3' })")
    time.sleep(0.2)
    I.move(900, 500)
    time.sleep(0.2)
    check("alive after workspace switch mid-drag", I.alive())
    I.release()
    time.sleep(0.4)
    check("alive after drop on other workspace", I.alive())
    c = I.win(o[0])
    check("window still tiled and mapped", not c["floating"] and c["mapped"], c)
    I.dispatch("hl.dsp.focus({ workspace = '1' })")


def t_unload_during_drag(I):
    I.reset()
    before = I.layout()
    o = I.order()
    begin_drag(I, o[0])
    I.move(*I.center(o[1], 0.9, 0.5))
    r = I.ctl(f"plugin unload {I.plugin}")
    time.sleep(0.3)
    check("unload mid-drag ok", r == "ok" and I.alive(), r)
    with open(f"/proc/{I.hl.pid}/maps") as f:
        mapped = os.path.basename(I.plugin) in f.read()
    check("plugin really unmapped after unload (no GNU unique symbols)", not mapped)
    I.release()
    time.sleep(0.3)
    check("layout unchanged after unload mid-drag", I.layout() == before, f"{before} vs {I.layout()}")
    r = I.ctl(f"plugin load {I.plugin}")
    check("reload plugin", r == "ok", r)


def t_version_gate(I):
    I.reset()
    approved = os.path.join(I.tmp, "xdg", "hyprdropper", "approved-versions")
    with open(approved, "w") as f:
        f.write("0.1\n")
    r1 = I.ctl(f"plugin unload {I.plugin}")
    r2 = I.ctl(f"plugin load {I.plugin}")
    st = I.status()
    check("unapproved version: plugin loads but stays inactive", r1 == "ok" and r2 == "ok" and st.get("hooks") == "false", (r1, r2, st))
    o = I.order()
    begin_drag(I, o[0])
    check("unapproved version: Hyprland's normal drag (window floats)", I.win(o[0])["floating"])
    I.release()
    time.sleep(0.3)
    check("unapproved version: compositor fine", I.alive())
    with open(approved, "w") as f:
        f.write("0.57\n")
    I.ctl(f"plugin unload {I.plugin}")
    r3 = I.ctl(f"plugin load {I.plugin}")
    check("approved again: hooks active", r3 == "ok" and I.status().get("hooks") == "true", I.status())


def t_visuals_scale_gap_landing(I):
    I.reset()
    o = I.order()
    first, second = o[0], o[1]
    slot = I.win(first)
    begin_drag(I, first)
    I.move(*I.center(second, 0.9, 0.5))
    time.sleep(0.4)
    st = I.status()
    check("window shrinks while dragged", st.get("scale", "").endswith("0.600") and st.get("scalers") == "1", st)
    check("gap outline shown at the old slot", st.get("gap", "none") != "none", st)
    if st.get("gap", "none") != "none":
        gx, gy, gw, gh = map(float, st["gap"].split())
        check("gap matches the old slot (window + border)", abs(gx - slot["at"][0]) <= 3 and abs(gw - slot["size"][0]) <= 6, (st["gap"], slot["at"], slot["size"]))
    I.release()
    time.sleep(0.6)
    st = I.status()
    check("scaler retired after landing", st.get("scalers") == "0", st)
    check("gap gone after drop", st.get("gap") == "none", st)
    check("compositor alive", I.alive())


def t_unload_while_scaled(I):
    I.reset()
    o = I.order()
    begin_drag(I, o[0])
    I.move(*I.center(o[1], 0.9, 0.5))
    time.sleep(0.3)
    check("scaled before unload", I.status().get("scalers") == "1", I.status())
    r = I.ctl(f"plugin unload {I.plugin}")
    time.sleep(0.3)
    # force a few frames with the window on screen
    I.move(*I.center(o[1], 0.5, 0.5))
    time.sleep(0.3)
    check("unload with live scaler: alive", r == "ok" and I.alive(), r)
    I.release()
    time.sleep(0.2)
    r = I.ctl(f"plugin load {I.plugin}")
    check("reload after unload with scaler", r == "ok" and I.alive(), r)


def t_close_while_settling(I):
    I.reset()
    o = I.order()
    begin_drag(I, o[0])
    I.move(*I.center(o[1], 0.9, 0.5))
    time.sleep(0.2)
    I.release()
    # close immediately while the scaler animates back
    I.dispatch(f"hl.dsp.window.close({{ window = 'class:{o[0]}' }})")
    time.sleep(0.6)
    check("close during settle: alive", I.alive())
    check("close during settle: no scaler left", I.status().get("scalers") == "0", I.status())


def t_scale_disabled(I):
    I.reset()
    I.eval("hl.config({ ['plugin.hyprdropper.drag_scale'] = 1.0, ['plugin.hyprdropper.show_gap'] = false })")
    o = I.order()
    begin_drag(I, o[0])
    I.move(*I.center(o[1], 0.9, 0.5))
    time.sleep(0.2)
    st = I.status()
    check("drag_scale=1: no scaler", st.get("scalers") == "0", st)
    check("show_gap=false: no gap", st.get("gap") == "none", st)
    I.release()
    time.sleep(0.2)
    I.eval("hl.config({ ['plugin.hyprdropper.drag_scale'] = 0.6, ['plugin.hyprdropper.show_gap'] = true })")


def t_bar_jumps_between_targets(I):
    """Moving from 'above W' to 'below W' must not glide the bar through the middle of W."""
    I.reset()
    o = I.order()
    begin_drag(I, o[0])
    I.move(*I.center(o[1], 0.5, 0.2))
    time.sleep(0.3)
    top = I.status()
    I.move(*I.center(o[1], 0.5, 0.8))  # status is read right after the motion, before any tick
    st = I.status()
    def box(v):
        return [round(float(x)) for x in v.split()]
    ok = "bar_shown" in st and "bar" in st and box(st["bar_shown"]) == box(st["bar"])
    check("bar snaps to the new target (no travel through the middle)", ok, (top.get("bar"), st.get("bar_shown"), st.get("bar")))
    I.eval("hl.plugin.test.keybind(1, 0, 9)")
    I.eval("hl.plugin.test.keybind(0, 0, 9)")
    I.release()


def t_neighbor_glow(I):
    I.reset()
    o = I.order()
    begin_drag(I, o[0])
    I.move(*I.center(o[1], 0.9, 0.5))       # new column between o[1] and o[2] -> two neighbours
    time.sleep(0.2)
    check("seam: two neighbours lit", I.status().get("neighbors") == "2", I.status())
    I.move(*I.center(o[1], 0.5, 0.2))       # top of o[1] -> only o[1] (nothing above it)
    time.sleep(0.2)
    check("top of a window: one neighbour lit", I.status().get("neighbors") == "1", I.status())
    I.move(*I.center(o[0], 0.5, 0.5))       # own slot -> restore, nothing lit
    time.sleep(0.2)
    check("own slot: nothing lit", I.status().get("neighbors") == "0", I.status())
    I.release()
    time.sleep(0.2)
    check("after drop: nothing lit", I.status().get("neighbors") == "0", I.status())


def t_color_source(I):
    I.reset()
    st = I.status()
    check("default: theme colour", st.get("bar_color", "").startswith("theme 42a5f5"), st.get("bar_color"))
    I.eval("hl.config({ plugin = { hyprdropper = { col = { bar = 'rgba(ff8800ff)' } } } })")
    st = I.status()
    check("only col.bar set: own colour", st.get("bar_color") == "own ff8800", st.get("bar_color"))
    I.eval("hl.config({ plugin = { hyprdropper = { use_theme_color = true } } })")
    st = I.status()
    check("use_theme_color = true wins over col.bar", st.get("bar_color", "").startswith("theme"), st.get("bar_color"))
    I.eval("hl.config({ plugin = { hyprdropper = { use_theme_color = false } } })")
    st = I.status()
    check("use_theme_color = false: own colour", st.get("bar_color") == "own ff8800", st.get("bar_color"))
    I.ctl("reload")
    time.sleep(0.3)
    st = I.status()
    check("after reload: back to theme", st.get("bar_color", "").startswith("theme"), st.get("bar_color"))


def t_animation_leaves(I):
    I.reset()
    anims = I.ctl("animations")
    check("own animation leaves exist", all(f"name: {n}" in anims for n in ("hyprdropper", "hyprdropperScale", "hyprdropperBar")), anims[-300:])
    r = I.ctl("eval hl.animation({ leaf = 'hyprdropperScale', enabled = true, speed = 20, bezier = 'default' })")
    check("hl.animation accepts our leaf", r == "ok", r)
    I.eval("hl.config({ animations = { enabled = true } })")
    o = I.order()
    begin_drag(I, o[0])
    time.sleep(0.15)  # speed 20 = 2 s for the whole shrink, so we must be mid-way
    st = I.status()
    sc = float(st.get("scale", "1 -> 1").split()[0]) if "scale" in st else 1.0
    check("shrink runs on the hyprdropperScale curve (mid-way)", 0.6 < sc < 1.0, st.get("scale"))
    I.eval("hl.plugin.test.keybind(1, 0, 9)")
    I.eval("hl.plugin.test.keybind(0, 0, 9)")
    I.release()
    time.sleep(2.5)
    check("scaler retired after the slow grow-back", I.status().get("scalers") == "0", I.status())
    I.ctl("reload")  # drops the override and turns animations off again (test config)
    time.sleep(0.3)
    anims = I.ctl("animations")
    blk = anims[anims.find("name: hyprdropperScale"):][:200]
    check("reload drops removed overrides", "speed: 0.00" in blk or "overriden: 0" in blk or "enabled: -1" in blk, blk)


SCENARIOS = [
    t_nothing_moves_and_new_column_after,
    t_into_column,
    t_own_slot_restores,
    t_esc_cancels,
    t_click_without_moving,
    t_cross_monitor_empty,
    t_cross_monitor_next_to,
    t_wheel_scrolls_not_workspace,
    t_edge_scroll,
    t_window_closes_during_drag,
    t_width_preserved_narrow,
    t_floating_unchanged,
    t_disabled_falls_back,
    t_reload_and_workspace_switch_during_drag,
    t_unload_during_drag,
    t_visuals_scale_gap_landing,
    t_unload_while_scaled,
    t_close_while_settling,
    t_scale_disabled,
    t_bar_jumps_between_targets,
    t_neighbor_glow,
    t_color_source,
    t_animation_leaves,
    t_version_gate,
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--tester", required=True)
    ap.add_argument("--keep", action="store_true", help="leave the instance running")
    ap.add_argument("-k", default="", help="only run scenarios containing this string")
    args = ap.parse_args()

    I = Instance(args.plugin, args.tester)
    try:
        I.start()
        for t in SCENARIOS:
            if args.k and args.k not in t.__name__:
                continue
            print(f"  {t.__name__}")
            try:
                t(I)
            except Exception as e:  # a scenario error is a failure, keep going
                FAILED.append(f"{t.__name__}: {e!r}")
                print(f"    ERROR {e!r}")
            if not I.alive():
                FAILED.append(f"{t.__name__}: Hyprland died")
                print("    Hyprland DIED")
                break
    finally:
        if args.keep:
            print(f"kept: HYPRLAND_INSTANCE_SIGNATURE={I.sig}  logs in {I.tmp}")
        else:
            I.stop()

    print(f"\n{PASSED} checks passed, {len(FAILED)} failed")
    for f in FAILED:
        print(f"  - {f}")
    return 0 if not FAILED else 1


if __name__ == "__main__":
    sys.exit(main())
