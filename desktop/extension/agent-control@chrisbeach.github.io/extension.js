// Agent Control: bring a Claude Code session's terminal window to the front
// and mark it, on request over D-Bus from the agent-control-window helper.
//
//   FocusWindowByPid(pid)       raise the one window owned by process `pid`
//   FocusWindowByTitle(needle)  raise the one window whose title contains `needle`
//   Highlight(state, label)     mark the window that has the focus (the one
//                               just raised) as the row tapped on the display:
//                               two rings pulse out from it, then an outline
//                               and a tab with `label` stay, in the colour the
//                               display gives `state`
//
// The two Focus methods answer "raised" once the window holds the focus,
// "refused" when the compositor kept the focus elsewhere, "notfound" when no
// window matches and "ambiguous" when several do. Nothing is raised then:
// bringing forward a window that may be the wrong one is worse than doing
// nothing.
//
// The mark follows the window as it moves and goes as soon as anything else
// takes the focus, the window closes, or another window is marked.
//
// This has to run inside gnome-shell: under Wayland only the compositor can
// present a window, and the built-in org.gnome.Shell.FocusApp refuses callers
// outside GNOME's own allowlist.
//
// Raising does not go through Meta.Window.activate(). That lands in mutter's
// window_activate(), which drops any request whose timestamp predates the last
// user input and marks the window as demanding attention instead (the shell
// shows "<app> is ready" and leaves the window where it was). A tap on the
// display is not input as far as the compositor is concerned, so there is no
// fresh timestamp to beat that check with. Meta.Window.focus() and raise() set
// the focus directly and carry no such test.

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import St from 'gi://St';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const BUS_NAME = 'org.gnome.Shell.Extensions.AgentControl';
const OBJECT_PATH = '/org/gnome/Shell/Extensions/AgentControl';
const IFACE = `
<node>
  <interface name="${BUS_NAME}">
    <method name="FocusWindowByPid">
      <arg type="u" direction="in" name="pid"/>
      <arg type="s" direction="out" name="result"/>
    </method>
    <method name="FocusWindowByTitle">
      <arg type="s" direction="in" name="needle"/>
      <arg type="s" direction="out" name="result"/>
    </method>
    <method name="Highlight">
      <arg type="s" direction="in" name="state"/>
      <arg type="s" direction="in" name="label"/>
      <arg type="s" direction="out" name="result"/>
    </method>
  </interface>
</node>`;

// The display's state colours (firmware/shared/ui.c), so the window is marked
// as its row is on the board.
const STATE_COLOURS = {
    waiting: '#FFAA33', failed: '#FF5544', done: '#73BF69', working: '#B9D4FF',
    running: '#B9D4FF', idle: '#B994C9', stopped: '#8A8AA0',
};
const LABEL_CHARS = 72;      // the display's title buffer
const TAB_HEIGHT = 26;
const TAB_INSET = 18;        // the tab's left edge, in from the window's
const OUTLINE = 2;           // px, just outside the window's frame
const CORNER = 15;           // a libadwaita window's corner, plus the outline
const PULSE_FROM = 2;        // px outside the frame where a ring starts
const PULSE_TO = 30;         // and where it has faded out
const PULSE_MS = 650;
const PULSE_GAP_MS = 180;    // the second ring follows the first

export default class AgentControlExtension extends Extension {
    enable() {
        this._dbus = Gio.DBusExportedObject.wrapJSObject(IFACE, this);
        this._dbus.export(Gio.DBus.session, OBJECT_PATH);
        this._nameId = Gio.DBus.session.own_name(
            BUS_NAME, Gio.BusNameOwnerFlags.REPLACE, null, null);
    }

    disable() {
        this._clearHighlight();
        if (this._nameId) {
            Gio.DBus.session.unown_name(this._nameId);
            this._nameId = null;
        }
        this._dbus?.unexport();
        this._dbus = null;
    }

    // ---- raising ----

    // Ordinary application windows only: a dialog or a menu shares its
    // parent's pid, and often its title.
    _windows() {
        return global.get_window_actors()
            .map(actor => actor.meta_window)
            .filter(win => win && win.get_window_type() === Meta.WindowType.NORMAL);
    }

    _focusOnly(test) {
        const matches = this._windows().filter(test);
        if (matches.length === 0)
            return 'notfound';
        if (matches.length > 1)
            return 'ambiguous';
        return this._present(matches[0]);
    }

    _present(win) {
        const time = global.display.get_current_time_roundtrip();

        Main.overview.hide();
        if (win.minimized)
            win.unminimize();

        const workspace = win.get_workspace();
        if (workspace && workspace !== global.workspace_manager.get_active_workspace())
            workspace.activate(time);

        win.raise();
        win.focus(time);

        return global.display.focus_window === win ? 'raised' : 'refused';
    }

    // For a program that keeps one window per process, such as a terminal
    // started on its own (`ptyxis -s`, kitty, foot, alacritty).
    FocusWindowByPid(pid) {
        if (!pid)
            return 'notfound';
        return this._focusOnly(win => win.get_pid() === pid);
    }

    // The caller passes something unique enough to name one window: a marker
    // it has just written into a terminal's title.
    FocusWindowByTitle(needle) {
        if (!needle)
            return 'notfound';
        return this._focusOnly(win => (win.get_title() ?? '').includes(needle));
    }

    // ---- the mark ----

    // "highlighted"; "nowindow" when no ordinary window has the focus;
    // "badstate" for a state the display does not have.
    Highlight(state, label) {
        const colour = STATE_COLOURS[state];
        if (!colour)
            return 'badstate';
        const win = global.display.focus_window;
        const actor = win?.get_compositor_private();
        if (!win || !actor || win.get_window_type() !== Meta.WindowType.NORMAL)
            return 'nowindow';
        this._clearHighlight();
        this._highlight(win, actor, colour, (label ?? '').trim().toUpperCase().slice(0, LABEL_CHARS));
        return 'highlighted';
    }

    _highlight(win, actor, colour, label) {
        // One overlay in the window group, kept just above the window's actor:
        // a later window, menu or dialog stacks over it as over the window.
        const overlay = new St.Widget({reactive: false});
        const outline = new St.Widget({reactive: false});
        overlay.add_child(outline);
        let tab = null;
        if (label) {
            tab = new St.BoxLayout({
                reactive: false,
                style: `background-color: ${colour}; padding: 0 14px 0 6px; spacing: 8px; height: ${TAB_HEIGHT}px;`,
            });
            tab.add_child(new St.Widget({  // sized here, not in CSS: the tab is measured before styles apply
                width: 14, height: 14, y_align: Clutter.ActorAlign.CENTER,
                style: 'border-radius: 7px; background-color: #000;',
            }));
            tab.add_child(new St.Label({  // no letter-spacing: St measures without it and draws with it
                text: label,
                y_align: Clutter.ActorAlign.CENTER,
                style: 'color: #000; font-family: Antonio, sans-serif; font-weight: bold; font-size: 15px;',
            }));
            overlay.add_child(tab);
        }
        global.window_group.add_child(overlay);

        const mark = {win, actor, overlay, outline, tab, colour, signals: []};
        this._mark = mark;
        const on = (object, signal, handler) => mark.signals.push([object, object.connect(signal, handler)]);
        on(win, 'position-changed', () => this._layout(mark));
        on(win, 'size-changed', () => this._layout(mark));
        on(win, 'unmanaged', () => this._clearHighlight());
        on(global.display, 'notify::focus-window', () => {
            if (global.display.focus_window !== win)
                this._clearHighlight();
        });
        on(global.display, 'restacked', () => this._restack(mark));

        this._restack(mark);
        this._layout(mark);
        this._pulse(mark, 0);
        this._pulse(mark, PULSE_GAP_MS);
    }

    // Maximised, tiled or full screen there is no room around the window: the
    // outline is drawn on its edge and the tab inside it, over the title.
    _filling(win) {
        // GNOME 49 replaced get_maximized() with get_maximize_flags().
        const maximized = win.get_maximize_flags?.() ?? win.get_maximized?.() ?? 0;
        return win.is_fullscreen() || maximized !== 0 ||
            win.get_frame_rect().y - TAB_HEIGHT < Main.layoutManager.getWorkAreaForMonitor(win.get_monitor()).y;
    }

    _layout(mark) {
        const frame = mark.win.get_frame_rect();
        const filling = this._filling(mark.win);
        const out = filling ? 0 : OUTLINE;
        mark.outline.set_position(frame.x - out, frame.y - out);
        mark.outline.set_size(frame.width + 2 * out, frame.height + 2 * out);
        mark.outline.style = `border: ${OUTLINE}px solid ${mark.colour}; border-radius: ${filling ? 0 : CORNER}px;`;
        if (!mark.tab)
            return;
        // Its natural width, measured with its style applied (a fresh actor has
        // none yet, so it measures short), cut down only where the window is
        // narrower: the label then ends in an ellipsis.
        mark.tab.set_width(-1);
        mark.tab.ensure_style();
        const [, natural] = mark.tab.get_preferred_width(-1);
        const tabWidth = Math.min(natural, frame.width - 2 * TAB_INSET);
        if (tabWidth < natural)
            mark.tab.set_width(tabWidth);
        const radius = TAB_HEIGHT / 2;
        const base = mark.tab.style.replace(/border-radius:[^;]*;/, '');
        if (filling) {
            mark.tab.set_position(frame.x + Math.round((frame.width - tabWidth) / 2), frame.y + 7);
            mark.tab.style = `${base} border-radius: ${radius}px;`;
        } else {
            mark.tab.set_position(frame.x + TAB_INSET, frame.y - TAB_HEIGHT);
            mark.tab.style = `${base} border-radius: ${radius}px ${radius}px 0 0;`;
        }
    }

    _restack(mark) {
        if (mark.overlay.get_parent() === mark.actor.get_parent())
            global.window_group.set_child_above_sibling(mark.overlay, mark.actor);
    }

    // A ring that grows out from the window (in from its edge, when it fills
    // the screen) and fades.
    _pulse(mark, delay) {
        const frame = mark.win.get_frame_rect();
        const inward = this._filling(mark.win);
        const at = grow => ({
            x: frame.x - grow, y: frame.y - grow,
            width: frame.width + 2 * grow, height: frame.height + 2 * grow,
        });
        const from = at(inward ? 0 : PULSE_FROM), to = at(inward ? -PULSE_TO : PULSE_TO);
        const ring = new St.Widget({
            reactive: false, opacity: 0, ...from,
            style: `border: 3px solid ${mark.colour}; border-radius: ${inward ? 0 : CORNER + 3}px;`,
        });
        mark.overlay.add_child(ring);
        ring.ease({
            opacity: 230, duration: 60, delay, mode: Clutter.AnimationMode.LINEAR,
            onComplete: () => ring.ease({
                ...to, opacity: 0, duration: PULSE_MS, mode: Clutter.AnimationMode.EASE_OUT_QUAD,
                onComplete: () => ring.destroy(),
            }),
        });
    }

    _clearHighlight() {
        const mark = this._mark;
        if (!mark)
            return;
        this._mark = null;
        for (const [object, id] of mark.signals)
            object.disconnect(id);
        mark.overlay.destroy();
    }
}
