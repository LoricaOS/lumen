# lumen architecture

This document describes the internals of the lumen compositor: the event loop,
the compositing pipeline, the client/server window protocol, input and cursor
handling, the embedded dropdown terminal, and the About window. It is grounded
in the source under `src/`; file and function references are given inline.

lumen is a single process, single-threaded, and poll-driven. There are no
worker threads in the compositor itself — concurrency comes from separate client
processes, the kernel's PTYs, and the `AF_UNIX` socket. Everything below happens
in one loop.

---

## 1. Startup (`main`, `src/main.c`)

`main()` runs a fixed sequence before entering the event loop:

1. **Nesting guard.** If `LUMEN_RUNNING` is already set in the environment,
   lumen refuses to start (a descendant re-exec — e.g. typing `lumen` in a
   lumen terminal — would otherwise map the framebuffer twice). It sets
   `LUMEN_RUNNING=1` before spawning any child.
2. **Framebuffer.** `syscall(513, &fb_info)` (`sys_fb_map`) maps the linear
   framebuffer and returns `{addr, width, height, pitch, bpp}`. lumen derives a
   pixel pitch (`pitch / (bpp/8)`) and allocates a heap **backbuffer** of
   `pitch_px * height * 4` bytes.
3. **Fonts.** `font_init()` (glyph) loads the bundled `Inter` and
   `JetBrains Mono` TTFs.
4. **Input.** `stdin` is switched to raw, non-canonical mode (`ECHO`, `ICANON`,
   `ISIG` cleared; `VMIN=0`, `VTIME=0` for non-blocking reads) and restored at
   exit via `atexit`. Job-control signals (`SIGTTIN`/`SIGTTOU`) are ignored so
   the compositor always owns the keyboard. `/dev/mouse` is opened non-blocking.
   `SIGUSR2` clears the `s_input_frozen` flag (used by the lock screen).
5. **Compositor + cursor.** `comp_init` zeroes the compositor, wires up the
   framebuffer and backbuffer surfaces, and centers the cursor. `cursor_init`
   builds the cursor sprite.
6. **Server.** `lumen_server_init()` creates `/run/lumen.sock`.
7. **Desktop.** The wallpaper (`/usr/share/wallpaper.raw`) and centered logo
   (`/usr/share/logo.raw`) are loaded if present. Desktop and overlay draw
   callbacks are registered (`desktop_draw_cb` draws the top bar;
   `overlay_draw_cb` draws the context menu).
8. **Boot crossfade.** lumen snapshots the current framebuffer (Bastion's login
   form) into an `mmap` buffer, composites the desktop into the backbuffer
   *without* writing the real framebuffer (by temporarily pointing the
   compositor's `fb.buf` at the backbuffer), then crossfades the saved frame
   into the composited desktop over ~250 ms (15 steps × 17 ms). This is the
   smooth login→desktop transition. It then prints `[LUMEN] ready` to stderr,
   the signal the boot/test harness waits for.
9. **Initial windows.** The dropdown terminal is created hidden; the About
   window is shown once (it carries the v1 software disclaimer).

The `mmap`/`munmap` for the one-shot snapshot buffer is deliberate: musl's
allocator would retain a freed `malloc` region of that size, so a true
`mmap`/`munmap` returns the ~4 MB to the OS and keeps lumen's footprint down.

---

## 2. The event loop (`main`, `src/main.c`)

One iteration, repeated forever, targeting ~60 fps (`sleep_ts` = 16 ms when
idle):

1. **Reap children.** `waitpid(-1, …, WNOHANG)` in a loop reaps exited clients.
   lumen is the parent of every app it spawns (`sys_spawn` sets ppid), and the
   kernel does not auto-reap on ignored `SIGCHLD`; without this each exited app
   would linger as a zombie holding pages and a process slot (the GUI would stop
   spawning after ~63 opens).
2. **Service clients.** `lumen_server_tick` accepts new connections and reads
   pending messages from existing clients.
3. **Keyboard.** A single non-blocking `read(0, …, 1)`. If the screen is locked
   (`s_input_frozen`), input is discarded. ESC starts escape-sequence collection
   (see §6). Otherwise the byte goes to the focused window's `on_key` callback,
   or, failing that, directly to its PTY (`win->tag`).
4. **Mouse.** All pending `/dev/mouse` events are drained and their deltas
   coalesced, then a single hit-test/dispatch is done (see §5). Top-bar
   interactions (volume slider, "Aegis" menu) and the context menu are handled
   here before falling through to `comp_handle_mouse`.
5. **PTY output.** Every window carrying a PTY (`win->tag >= 0`) is read and its
   output fed to the terminal emulator (`terminal_write`).
6. **Clock + live prefs.** Roughly once per second, `glyph_theme_reload_prefs`
   re-reads runtime preferences so Settings changes (clock format, timezone,
   natural scroll, animations, theme, wallpaper, night light) apply without a
   restart; whole-screen changes set `full_redraw`. The clock string is
   recomputed and, if changed, the top-bar strip is marked dirty.
7. **Composite + present.** If anything changed this frame, lumen hides the
   cursor, calls `comp_composite`, re-shows the cursor, and issues
   `syscall(515)` (`sys_present`). A mouse-only frame takes the fast cursor path
   (no recomposite).
8. **Second PTY pass.** After compositing (during which the shell got scheduler
   time), PTYs are read again to catch output that wasn't ready on the first
   pass, recompositing if any arrived.
9. **Animations.** While any window is mid-open-animation, frames are forced at
   ~60 fps; one extra composite lands the final full-chrome frame when the last
   animation finishes.
10. **Idle.** If nothing happened, sleep 16 ms to avoid busy-looping.

---

## 3. Window model (`src/compositor.c`, `src/compositor.h`)

The compositor holds a Z-ordered array `windows[MAX_WINDOWS]` (16 max), with the
top of the stack at the highest index. Each entry is a glyph `glyph_window_t`.
Relevant per-window state the compositor reads/writes includes `x`/`y`,
`surf_w`/`surf_h`, `client_w`/`client_h`, `visible`, `presented`, `chromeless`,
`frosted`, `focused_window`, `admin`, the open-animation fields
(`anim`, `anim_t0_ms`), the dirty-rect (`has_dirty`, `dirty_rect`), the
blur/panel cache fields, `blit_src` (for proxy windows), `tag` (PTY fd or −1),
and the input callbacks (`on_key`, `on_mouse_*`, `on_render`, `on_close`).

Window-management primitives:

- `comp_add_window` — push, focus, and (for in-process chromed windows) start
  the open animation. Forces `full_redraw`.
- `comp_remove_window` — marks the window's screen footprint dirty, removes it,
  fixes up focus/drag/DnD references, calls `glyph_window_destroy`, forces
  `full_redraw`.
- `comp_raise_window` — moves a window to the top of the stack.
- `comp_window_at` — top-down hit test against visible windows.

`win_screen_rect` returns a window's total footprint, inflated by `SHADOW_PAD`
(30 px) for chromed windows so the soft drop shadow is included in damage
tracking and never smears on move/close.

### Open animation

`comp_start_open_anim` begins a 150 ms scale+fade (skipped for chromeless
windows and when animations are disabled in Settings). `comp_has_anim` reports
whether any window is still animating and clears finished ones. During the
animation, `draw_window_open_anim` blits the client content scaled toward its
center with an ease-out cubic alpha ramp; chrome and frost are skipped until it
snaps to the full window.

---

## 4. Compositing pipeline (`comp_composite`, `src/compositor.c`)

Compositing is damage-driven. There are two paths.

### Full redraw

Triggered by `full_redraw` (first frame, window raise/move-release, theme or
wallpaper change, DnD release). It:

1. Invalidates every frosted window's panel cache.
2. Paints the desktop background across the whole backbuffer — either the
   wallpaper (`memcpy` rows when it matches the resolution exactly, otherwise a
   scaled blit) or a vertical gradient computed per-row by `desktop_bg_at`, which
   selects among four wallpaper presets (Aegis gradient, Midnight flat, Slate,
   Accent).
3. Runs `on_draw_desktop` (top bar).
4. Renders and blits every visible, presented window bottom-to-top.
5. Draws the desktop selection box, the overlay (`on_draw_overlay`, the menu),
   and the DnD ghost.
6. Flips the entire backbuffer to the framebuffer (warm-tinted per-pixel if
   Night Light is on).

### Partial (dirty-rect) redraw

When `full_redraw` is clear, the compositor processes the accumulated dirty
rects individually rather than unioning them into one bounding box (which would
needlessly repaint the span between two far-apart small rects). For each dirty
rect it repaints the desktop background within the rect; then it re-renders any
window whose `win_screen_rect` intersects any dirty rect; then it draws the
overlay and DnD ghost once; finally it flips only the dirty rects to the
framebuffer (`partial_flip`, applying the Night Light tint there if enabled).

Dirty rects are accumulated by `comp_add_dirty`, which clamps to the screen and,
on overflow past `MAX_DIRTY_RECTS` (32), unions into the last slot.

### Window rendering (`blit_window_to_back`)

A window is composited in one of several modes (`BLIT_FROST`, `BLIT_FAST_FROST`
used for non-dragged windows during a drag, `BLIT_OPAQUE` for the dragged
window):

- **Chromeless frosted** (dropdown terminal, fullscreen launcher): blur + a
  single dark tint, then a color-keyed blit of the surface so key-colored pixels
  show the frosted desktop through.
- **Chromed frosted** (normal app windows): the full glass treatment —
  1. blur the window footprint,
  2. tint the titlebar (red and near-opaque when `admin`, otherwise dark glass),
  3. tint the client region (darker for terminals, lighter for widget windows),
  4. draw subtle 1 px borders,
  5. draw the centered title text,
  6. draw the three traffic-light circles (full color when focused, grey when
     not),
  7. color-keyed blit of the client content,
  8. dim the whole window slightly if unfocused,
  9. round the four corners.
- **Opaque chromed** (the window being dragged): a plain blit with corners still
  rounded, frost skipped for drag smoothness.

A soft drop shadow (`draw_window_shadow`) is drawn under chromed windows on
full-quality frost passes. Its geometry is intentionally focus-independent so it
only changes on move/open/close — the exact events that dirty the
`SHADOW_PAD`-expanded footprint.

#### Rounded corners

Square compositing is made to read as rounded by `save_corner_block` /
`round_window_corner`: the four corner blocks of the background (desktop +
shadow) are saved before the window is composited, and afterward the pixels
outside each corner's rounded arc are restored from those saves.

#### The frosted-glass panel cache

The blur+tint+border "panel" of a frosted window depends only on the backdrop
(desktop + windows below it) plus constant tint colors. On a content-only frame
the backdrop is unchanged, so the panel can be cached and stamped instead of
recomputed — removing the per-frame box blur and the whole-client alpha blend,
the two biggest per-frame costs.

`comp_update_blur_validity` decides, before rendering, which frosted windows may
reuse their cache this frame. A window's cached panel is invalidated iff (a) an
*external* dirty rect (one added by an event handler before per-window content
damage — clock/volume repaint, a move, a removed window, the selection box, the
DnD ghost) overlaps its footprint, or (b) some window *below* it whose own
content changed this frame overlaps its footprint. A window's own content damage
never invalidates its own cache — that is the win. Windows *above* it are painted
over it afterward and are not part of its backdrop. `panel_try_stamp` /
`panel_capture` store the clamped footprint in a per-window buffer (origin/size
mismatch on a move automatically counts as a miss). `n_ext` — the dirty count at
composite entry — is the external/own boundary; on dirty-list overflow every
panel is conservatively invalidated.

---

## 5. Input handling

### Mouse (`comp_handle_mouse`, `src/compositor.c`)

The drained per-frame mouse delta is applied to the cursor position with the
user's pointer-speed multiplier (`glyph_theme_pointer_speed`, clamped 50–400%),
then clamped to the screen. Old and new cursor rects are added as dirty.

A precedence ladder then routes the event (each step returns if it consumed the
gesture):

1. **Wheel** → the window under the cursor (proxy `on_mouse_wheel` with local
   coords, or the dropdown terminal's position-less `on_scroll`). Natural
   scrolling inverts the direction.
2. **Drag-and-drop in progress** → the ghost follows the cursor; the proxy under
   the pointer receives `DRAG_OVER`/`DRAG_LEAVE`, and on release the one under
   the pointer receives `DROP` with the path payload.
3. **No-button hover move** → forwarded to the proxy window under the cursor (for
   dock/menu hover highlighting).
4. **Desktop selection box** → updated/committed while dragging on empty desktop.
5. **Content drag** (text selection in a client area) → `on_mouse_move` /
   `on_mouse_up` to the window that started it.
6. **Titlebar drag** → moves `drag_win`, marking old and new footprints dirty.
7. **Right-button press edge** → `on_mouse_rdown` for windows that opt in.
8. **Left-button press edge** → close-button hit test (→ `on_close` or destroy),
   titlebar hit test (→ start drag + focus/raise), focus/raise on a body click,
   then dispatch to the window; a press on empty desktop starts a selection box.

Top-bar interactions (the volume slider drag and the "Aegis" menu toggle) and
the context menu clicks are handled in `main.c` before `comp_handle_mouse` is
called.

### Keyboard (escape decoding, `src/main.c`)

A leading ESC byte triggers immediate sequence collection using `poll` with a
short timeout (15 ms, covering one PIT tick — USB HID bytes can arrive up to
~10 ms apart). The compositor recognizes:

- **Bare ESC** → focused window's `on_key`, or its PTY.
- **Ctrl+Alt+T** → toggle the dropdown terminal (with focus save/restore).
- **Ctrl+Alt+I** → launch the GUI installer; **Ctrl+Alt+R** → the run launcher;
  **Ctrl+Alt+L** → lock the screen (freeze input and signal Bastion).
- **Ctrl+Shift+C / Ctrl+Shift+V** (and Alt+C/Alt+V) → copy/paste against the
  internal 8 KB clipboard and the focused terminal's selection.
- **CSI sequences (ESC `[`)** → collected and written whole to the focused PTY;
  for proxy windows that take single bytes, arrow keys are translated to
  synthetic high-range codes (`0xF1`–`0xF4`).
- **Other Alt+key combos** → forwarded as ESC+byte in one write so the client
  sees them atomically.

### Cursor (`src/cursor.c`)

The cursor is a procedurally built 16×20 ARGB arrow sprite: a per-row width
table defines the arrow shape, drawn in three passes (translucent drop shadow,
black outline, white interior). `cursor_show` saves the framebuffer pixels under
the sprite into `s_save[]` and alpha-blends the sprite in; `cursor_hide`
restores the saved pixels. The compositor must bracket every framebuffer write
with `cursor_hide()` / `cursor_show()`. Pure pointer motion uses this save-under
to relocate the cursor without recompositing the scene.

---

## 6. The window protocol server (`src/lumen_server.c`)

### Socket and handshake

`lumen_server_init` creates `/run/lumen.sock` (`AF_UNIX`, `SOCK_STREAM`),
non-blocking, `listen` backlog 8. `lumen_server_tick` (called each loop
iteration) polls the listen fd for new connections and each client fd for
pending data, all with zero timeout so the compositor never blocks.

On accept (`lumen_server_accept_fd`), the client must send a `lumen_hello_t`
within 500 ms; the server checks `LUMEN_MAGIC` / `LUMEN_VERSION`, enforces the
`LUMEN_MAX_CLIENTS` (8) limit, replies with a `lumen_hello_reply_t`, and
allocates a `lumen_client_t`.

### Per-client state

```c
lumen_client_t  { fd, windows[8], nwindows, next_id }
proxy_window_t  { glyph_window_t *win, lumen_client_t *client,
                  uint32_t id, int memfd, void *shared }
```

Each client may own up to `LUMEN_MAX_WINDOWS_PER_CLIENT` (8) proxy windows.

### Message dispatch (`lumen_server_read`)

A message is a `lumen_msg_hdr_t { op, len }` followed by a body. For every
fixed-size opcode the server **requires `hdr.len` to equal the expected struct
size exactly** and drops the connection on mismatch — without this a bad `len`
would desync the stream against the wire framing. Unknown opcodes are skipped by
draining `hdr.len` bytes (forward extensibility).

Handled opcodes:

| Opcode | Handler | Effect |
|--------|---------|--------|
| `LUMEN_OP_CREATE_WINDOW` | `handle_create_window` | Create a normal or fullscreen window. |
| `LUMEN_OP_CREATE_PANEL` | `handle_create_panel` | Create a bottom-anchored, never-focused, chromeless panel (the dock). |
| `LUMEN_OP_DAMAGE` | `handle_damage` | Mark dirty + present; first DAMAGE reveals the window and starts its open animation. |
| `LUMEN_OP_DESTROY_WINDOW` | `handle_destroy_window` | Remove the window, unmap/close the memfd, free the proxy. |
| `LUMEN_OP_SET_TITLE` | (read, no-op) | Accepted but not acted on. |
| `LUMEN_OP_INVOKE` | `handle_invoke` | Ask the registered invoke handler to launch a named app. |
| `LUMEN_OP_DRAG_START` | `handle_drag_start` | Begin a compositor-brokered drag (validated: op ∈ {MOVE, COPY}, non-empty path). |
| `LUMEN_OP_SET_ADMIN` | `handle_set_admin` | Flag the window as an elevated session → red titlebar. |

### Window creation and the shared buffer (`handle_create_common`)

The core path for all three window styles (`WSTYLE_NORMAL`, `WSTYLE_PANEL`,
`WSTYLE_FULLSCREEN`):

1. `memfd_create` + `ftruncate` to `w*h*4`, then `mmap(PROT_READ, MAP_SHARED)` —
   the compositor's read-only view of the client's pixel buffer.
2. Build the `glyph_window_t`. Panels and fullscreen windows use
   `make_chromeless_window` with `want_surface=0`: they have no chrome to draw,
   so the compositor reads the client's shared buffer directly via `blit_src`,
   avoiding a second full-window buffer and the alloc churn that musl could not
   fully reuse. Normal windows use `glyph_window_create` (chrome-padded surface).
3. Wire up the proxy callbacks (`proxy_on_render`, `proxy_on_close`,
   `proxy_on_key`, the mouse callbacks). Panels get no `on_key` (they never take
   keyboard focus).
4. Position by style: normal = centered; panel = bottom-centered, 10 px margin,
   focus restored to the window beneath; fullscreen = (0,0), framebuffer-sized.
5. The window is created **unpresented** so its uninitialized (zero) buffer never
   flashes; the first `DAMAGE` presents it.
6. Reply with `lumen_window_created_t` (status, id, geometry) **plus the memfd
   itself** passed as an `SCM_RIGHTS` ancillary message via `sendmsg`. The client
   maps the same memfd and renders into it. On any failure an error reply
   (`status = EIO`) is sent.

### Event delivery to clients

The proxy callbacks translate compositor events into protocol messages written
back over the client socket: `proxy_on_key` → `LUMEN_EV_KEY`; the mouse
callbacks → `LUMEN_EV_MOUSE` (down/up/move/wheel, with window-local coords
converted to client-area coords by subtracting the border/titlebar insets);
`proxy_on_close` → `LUMEN_EV_CLOSE_REQUEST` (so the client runs its own teardown
rather than being killed); `lumen_proxy_notify_focus` → `LUMEN_EV_FOCUS`; and the
DnD helpers → `LUMEN_EV_DRAG_OVER` / `_LEAVE` / `_DROP`.

### Proxy rendering (`proxy_on_render`)

For chromeless proxy windows, `blit_src` aliases the shared buffer and rendering
is a no-op (the compositor reads the client's pixels directly). For chromed
proxy windows the client's shared buffer is copied into the inset client region
of the window's chrome-padded surface.

### Drag-and-drop brokering

DnD is owned by the compositor, not the clients. A source client sends
`LUMEN_OP_DRAG_START`; `comp_dnd_start` (only if the left button is still held)
captures the label and path, and the compositor then draws a ghost label at the
cursor (`draw_dnd_ghost`), sends `DRAG_OVER`/`DRAG_LEAVE` to the proxy under the
pointer, and on release delivers `DROP` with the path to the target. DnD targets
are restricted to proxy windows (`priv` set, `tag < 0`), excluding the dropdown
terminal. If the source window dies mid-drag the gesture is cancelled.

### Hangup

`lumen_server_hangup` (on EOF/error) removes all of a client's windows,
unmaps/closes their memfds, frees the proxies and the client record, and forces a
full redraw if anything was on screen.

---

## 7. The dropdown terminal (`src/terminal.c`)

A single quake-style dropdown terminal lives in-process. It is a thin wrapper
over glyph's shared terminal-emulator core (`glyph_term`): the wrapper owns a
`glyph_window_t` whose `priv` is the `glyph_term_t`, plus a PTY opened by
`glyph_pty_open_and_spawn`. Because `main.c` may pass *any* focused window
(including proxies, whose `priv` is a `proxy_window_t`, not a `glyph_term_t`) to
the terminal helpers, every entry point guards via `term_of`, which returns the
emulator only when the window is the registered dropdown.

`terminal_create_dropdown` sizes the window to the screen width minus margins and
45% of the screen height, computes the character grid from the monospace font
metrics, allocates the emulator, creates a chromeless frosted glyph window with
rounded bottom corners, wires key/mouse/scroll callbacks, opens the PTY, and
spawns the shell. The window starts hidden; Ctrl+Alt+T toggles it.

Output flows in via `terminal_write` (called from the main loop's PTY poll),
which feeds bytes to `glyph_term_feed` and mirrors the shell's admin-session
state onto `win->admin`. Selection/clipboard helpers
(`terminal_has_selection`, `terminal_copy_selection`, `terminal_clear_selection`)
back the global copy/paste shortcuts. `dropdown_render_content` renders the
emulator into the window surface and paints the background color over the
bottom-corner arcs to round them.

Regular terminal windows are *not* here — they are the standalone `/bin/terminal`
protocol client. Only the dropdown remains embedded.

---

## 8. The About window (`src/about.c`)

`about_create` builds a 400×500 frosted glyph window and installs `about_render`
as its renderer. It loads two RGBA assets (each a `[width, height]` header
followed by pixels): the Aegis logo (`/usr/share/logo.raw`) and the Claude mark
(`/usr/share/claude.raw`).

`about_render` fills the client area with the frost key color and lays out,
top to bottom: the scaled logo, the version string
(`"Version <AEGIS_VERSION> …"`, injected at build time via `-DAEGIS_VERSION`), a
tagline, the v1 software disclaimer (first public release, not production
hardened — the same warning shown once at every startup), system info read from
`/proc/meminfo` (CPU core count, total memory) and the actual display
resolution, credits, and a bottom-pinned "Built with Claude Code" mark.

The CPU/memory reads from `/proc` are the concrete use of lumen's `PROC_READ`
capability.

---

## Appendix: syscalls and capabilities

lumen uses a small set of Aegis syscalls directly:

| Syscall | Number | Use |
|---------|--------|-----|
| `sys_fb_map` | 513 | Map the linear framebuffer (`FB` capability). |
| `sys_present` | 515 | Present a frame (no-op on a direct framebuffer; hook for buffered paths). |
| `sys_spawn` | 514 | Spawn a client process for a launched app (`THREAD_CREATE`). |
| `sys_audio_volume` | 503 | Set the HDA output level from the top-bar slider. |
| `sys_reboot` | 169 | Fallback hard reboot when signaling init fails (`POWER`). |

Graceful power actions prefer signaling init (PID 1): `SIGINT` for reboot
(ctrl-alt-del convention, so vigil tears down services and syncs first) and
`SIGTERM` for poweroff, falling back to `sys_reboot`. Locking the screen signals
lumen's parent (Bastion) with `SIGUSR1`.

The capability set in `pkg/caps.d/lumen` — `service FB THREAD_CREATE PROC_READ
POWER` — is exactly what these uses require, and nothing more. lumen holds no
ambient authority beyond this declared set.
