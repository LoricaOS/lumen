# lumen

**lumen** is the compositor and display server for [AspisOS](https://github.com/AspisOS),
a capability-based, no-ambient-authority x86-64 operating system built on the
from-scratch [Aegis](https://github.com/AspisOS/Aegis) kernel.

lumen owns the screen. It maps the framebuffer, draws the desktop, and is the
single process every graphical application talks to in order to get a window.
A GUI app does not touch the framebuffer or the input devices itself — it
connects to lumen over an `AF_UNIX` socket, asks for a window, and receives a
shared-memory buffer it draws into plus a stream of input events. lumen
composites every client's surface, its own built-in surfaces (the top bar, the
"Aegis" menu, the dropdown terminal, the About window), and the cursor into the
framebuffer.

This repository is a **standalone AspisOS component**. It does not vendor the
GUI toolkit; it fetches a pinned build artifact of the
[glyph](https://github.com/AspisOS/glyph) toolkit (`GLYPH_VERSION`), compiles
against it, and packs a signed [herald](https://github.com/AspisOS) system
package, `lumen.hpkg`.

---

## Where lumen fits

AspisOS is decomposed into independent repositories:

| Repo | Role |
|------|------|
| `AspisOS/Aegis` | The kernel. Provides the framebuffer mapping, PTYs, `AF_UNIX` sockets, `memfd`, the capability model, and the syscalls lumen uses. |
| `AspisOS/AspisOS` | The OS: userland, rootfs, ISO/installer. Assembles the components into a bootable system. |
| `AspisOS/glyph` | The GUI toolkit. Provides window/surface primitives, the software renderer (`draw_*`, `font_*`), the terminal emulator core (`glyph_term`), theme/preferences, the `/apps` bundle registry, **and the client side of lumen's window protocol** (`lumen_proto.h`, `lumen_client.h`). |
| `AspisOS/lumen` | **This repo.** The compositor/display server. The foundational graphical component. |

Everything graphical depends on lumen. The desktop session manager (Bastion)
launches lumen; the dock, application launcher, file manager, editor,
calculator, settings, the standalone terminal, and the GUI installer are all
lumen clients that connect to `/run/lumen.sock`. In herald packaging terms,
those components declare `depends=lumen`.

Because lumen ships the desktop fonts (`Inter`, `JetBrains Mono`) and logo
assets inside its package, every component that depends on lumen gets the fonts
transitively — there is no separate font package to install.

---

## Architecture overview

lumen is a single-threaded, poll-driven compositor. `main()`
(`src/main.c`) maps the framebuffer, sets up the compositor, opens the input
devices and the client socket, then runs one event loop at roughly 60 fps:
service clients, read keyboard and mouse, read terminal PTYs, update the clock,
composite the dirty regions, present.

The source is organized into focused modules:

| File | Responsibility |
|------|----------------|
| `src/main.c` | Entry point and event loop. Framebuffer mapping (`sys_fb_map`), backbuffer allocation, raw-mode keyboard, mouse batching, the top-bar/clock/volume UI, the "Aegis" context menu, keyboard shortcuts, the boot crossfade, wallpaper/logo loading, client process spawning, and zombie reaping. |
| `src/compositor.c` / `.h` | Window management and dirty-rect compositing: the window stack, focus/raise, drag, drag-and-drop brokering, the desktop background/gradient, frosted-glass window chrome, soft shadows, rounded corners, the per-window blur cache, and the backbuffer→framebuffer flip. |
| `src/lumen_server.c` / `.h` | The `AF_UNIX` window-protocol server: accept/handshake, per-client state, `CREATE_WINDOW`/`CREATE_PANEL`/`DAMAGE`/`DESTROY`/`INVOKE`/`DRAG_START`/`SET_ADMIN`, the `memfd` shared-buffer handoff via `SCM_RIGHTS`, and input/focus/DnD event delivery back to clients. |
| `src/cursor.c` / `.h` | The software cursor: a procedurally built 16×20 ARGB arrow sprite with outline and drop shadow, drawn with save-under so the compositor can move it without recompositing. |
| `src/terminal.c` / `.h` | The in-process quake-style **dropdown terminal** (Ctrl+Alt+T). A thin wrapper over glyph's shared terminal-emulator core (`glyph_term`) plus a PTY. Regular terminals are a separate `/bin/terminal` client. |
| `src/about.c` / `.h` | The "About Aegis" window: version string, CPU/memory/display info read from `/proc`, logo, and the v1 software disclaimer (shown once at every startup). |

### The compositor model

The compositor keeps a Z-ordered array of up to `MAX_WINDOWS` (16) windows
(`compositor_t.windows`, top of stack is last). Each window is a glyph
`glyph_window_t`. Windows come in two flavours:

- **In-process windows** owned by lumen itself: the dropdown terminal and the
  About window. lumen renders their content directly.
- **Proxy windows** owned by external clients. The client renders into a shared
  `memfd` buffer; the compositor reads that buffer when it composites. A
  `proxy_window_t` (`src/lumen_server.c`) ties the glyph window to the client
  connection and the shared mapping.

Rendering is **damage-driven**. Event handlers accumulate dirty rectangles
(`comp_add_dirty`), and `comp_composite` repaints only those regions — redrawing
the desktop background under each rect, re-blitting any window that overlaps it,
then copying just those rects to the framebuffer (`partial_flip`). A
`full_redraw` flag forces a whole-frame composite for events that change
everything (window raise, theme change, wallpaper change, the boot fade). See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full compositing pipeline,
including the frosted-glass panel cache and rounded-corner handling.

### Client connection and the window protocol

lumen listens on `/run/lumen.sock` (`AF_UNIX`, `SOCK_STREAM`). The wire protocol
is defined in the glyph toolkit (`lumen_proto.h` / `lumen_client.h`), so clients
and the server share one definition. The exchange is:

1. **Handshake.** On connect the client sends a `lumen_hello_t` (`magic` +
   `version`); the server validates it against `LUMEN_MAGIC` / `LUMEN_VERSION`
   and replies with a `lumen_hello_reply_t` status.
2. **Create a window.** The client sends `LUMEN_OP_CREATE_WINDOW` (or
   `LUMEN_OP_CREATE_PANEL`). The server allocates a `memfd`, `ftruncate`s it to
   `width*height*4`, maps it, and replies with a `lumen_window_created_t`
   carrying the geometry — and the `memfd` itself passed as an `SCM_RIGHTS`
   ancillary message. The client maps the same `memfd` and draws into it.
3. **Damage.** When the client has drawn a frame it sends `LUMEN_OP_DAMAGE`; the
   compositor marks the window dirty and presents it. The first `DAMAGE` is also
   what reveals the window (it is created unpresented so its uninitialized
   buffer never flashes) and triggers its open animation.
4. **Events.** The compositor delivers `LUMEN_EV_KEY`, `LUMEN_EV_MOUSE`
   (down/up/move/wheel), `LUMEN_EV_FOCUS`, the drag-and-drop events
   (`LUMEN_EV_DRAG_OVER` / `_LEAVE` / `LUMEN_EV_DROP`), and `LUMEN_EV_CLOSE_REQUEST`
   back over the same socket.

Two additional opcodes are notable. `LUMEN_OP_INVOKE` lets a client (the dock)
ask the compositor to launch a named application — resolved through glyph's
`/apps` bundle registry and spawned as a new client process. `LUMEN_OP_SET_ADMIN`
lets a client flag a window as an elevated/admin session, which the compositor
renders with an unmistakable red titlebar tint.

The server is hardened against malformed clients: every fixed-size opcode
requires the client-declared header length to match the expected struct size
exactly, and the connection is dropped on any mismatch rather than guessing
(`lumen_server_read`). Client and per-client window counts are bounded
(`LUMEN_MAX_CLIENTS` = 8, `LUMEN_MAX_WINDOWS_PER_CLIENT` = 8).

### Input and the cursor

lumen reads the raw keyboard from `stdin` (put into raw, non-canonical mode) and
the mouse from `/dev/mouse` (non-blocking). Mouse events are batched per frame —
all pending deltas are drained and coalesced before hit-testing — to keep the
pointer responsive. Keyboard handling decodes escape sequences inline (bare ESC,
CSI arrow keys, and the compositor's global chords) and otherwise forwards keys
to the focused window's `on_key` callback or to its PTY.

The cursor (`src/cursor.c`) is drawn in software with **save-under**: before the
sprite is blitted, the framebuffer pixels beneath it are copied aside, so
`cursor_hide()` can restore them without a recomposite. Pure pointer motion (no
content change) takes a fast path that only hides and re-shows the cursor.

### The built-in dropdown terminal

A single quake-style dropdown terminal lives in-process (`src/terminal.c`),
toggled with Ctrl+Alt+T. It is a thin wrapper over glyph's shared
`glyph_term` emulator core plus a PTY opened by glyph's
`glyph_pty_open_and_spawn`. The main loop polls every window that carries a PTY
(`win->tag` ≥ 0) and feeds output into the emulator. Regular terminal windows
are a separate standalone client (`/bin/terminal`); only the dropdown remains
embedded in the compositor.

### Framebuffer handling

lumen maps the linear framebuffer once via `sys_fb_map` (syscall 513), which
returns its address, dimensions, pitch, and bpp. It composites into a
heap-allocated **backbuffer** and flips finished regions to the real framebuffer,
so clients never see partial frames. A `sys_present` syscall (515) is issued
after each flip (a no-op on a direct framebuffer, a hook for buffered display
paths). Night Light, when enabled, is applied as a warm tint during the flip.

---

## The capability model

AspisOS has **no ambient authority**. A process can do nothing to the system
except through capabilities granted to it by kernel policy at exec time; there is
no implicit "root can do anything". lumen's capability set is declared in
`pkg/caps.d/lumen` and installed to `/etc/aegis/caps.d/lumen`:

```
service FB THREAD_CREATE PROC_READ POWER
```

| Capability | Why lumen needs it |
|------------|--------------------|
| `FB` | Direct framebuffer access. lumen maps the linear framebuffer with `sys_fb_map` and writes pixels to it — it *is* the display server, so this is its defining capability. |
| `THREAD_CREATE` | Used to spawn client processes. lumen launches GUI applications on demand (`sys_spawn`, e.g. from the "Aegis" menu and `LUMEN_OP_INVOKE`); without authority to create new execution contexts it could not start the desktop's apps. |
| `PROC_READ` | Read process/system introspection. The About window reads `/proc/meminfo` for CPU and memory info; lumen also reaps the child clients it spawns. |
| `POWER` | Power control. The menu's "Restart" and "Power Off" items signal init (PID 1) for a graceful shutdown, with a raw `sys_reboot` as fallback — which requires power authority. This is why the package is `class=system`. |

`service` marks this as a system service profile. lumen runs with **only** these
baseline capabilities; it does not hold elevated privilege. Capability
delegation for a session is handled by lumen's parent (Bastion), not by lumen
itself.

---

## Building

lumen builds with a musl cross-compiler against the prebuilt glyph toolkit.

Requirements:

- `MUSL_CC` — a musl `gcc` (defaults to `musl-gcc` on `PATH`). AspisOS userland
  is built against musl.
- `HERALD_KEY` — the package signing key (ECDSA P-256 private key). Required to
  pack `lumen.hpkg`.
- Network access (or a pre-populated cache) to fetch the glyph toolkit artifact.

```sh
make                       # fetch toolkit + build lumen.elf + pack lumen.hpkg
HERALD_KEY=/path/to/key make
MUSL_CC=/opt/musl/bin/musl-gcc make
make clean                 # remove build outputs + the fetched toolkit
```

The build (`Makefile`) does three things:

1. **Fetch the toolkit.** `tools/fetch-glyph.sh $(GLYPH_VERSION) toolkit`
   downloads `glyph-<ver>.tar.gz` from the glyph repo's releases (or uses the
   cached copy under `vendor/`) and unpacks its `include/` and `lib/` into
   `toolkit/`. This mirrors how AspisOS fetches the kernel artifact.
2. **Compile.** `$(MUSL_CC)` builds the `src/*.c` sources against
   `toolkit/include` and links `toolkit/lib`'s `-lcitadel -lglyph` into
   `lumen.elf`. The version string is injected via `-DAEGIS_VERSION`.
3. **Pack.** `tools/pack.sh` stages the payload, builds `lumen.hpkg`, and signs
   it.

The pinned toolkit version lives in `GLYPH_VERSION`; lumen's own version lives in
`VERSION`.

---

## Packaging

`lumen.hpkg` is a **herald system package**: a manifest-first, uncompressed POSIX
`ustar` archive accompanied by a detached `lumen.hpkg.sig` ECDSA-P256/SHA-256
signature (`tools/pack.sh`). Because lumen is `class=system` (it holds the
`POWER` capability), herald installs its whole payload tree verbatim:

```
manifest                          id, name, version, class=system
bin/lumen                         the compositor (stripped)
etc/aegis/caps.d/lumen            its capability policy
usr/share/fonts/Inter-Regular.ttf
usr/share/fonts/JetBrainsMono-Regular.ttf
usr/share/logo.raw                Aegis desktop logo
usr/share/claude.raw              "Built with Claude Code" mark
```

The fonts ship here so that every component depending on lumen inherits them.
The logo assets back the desktop centerpiece and the About window.

Install it directly:

```sh
herald install lumen
```

or pull it in as part of the `desktop` meta-package. lumen is the base of the
graphical stack — everything else in the desktop declares `depends=lumen`.

---

## Repository layout

```
.
├── Makefile               fetch toolkit → build lumen.elf → pack lumen.hpkg
├── VERSION                lumen's version
├── GLYPH_VERSION          pinned glyph toolkit artifact version
├── src/
│   ├── main.c             entry point + event loop + top bar/menu/shortcuts
│   ├── compositor.c/.h    window management + dirty-rect compositing
│   ├── lumen_server.c/.h  AF_UNIX window-protocol server
│   ├── cursor.c/.h        software cursor with save-under
│   ├── terminal.c/.h      in-process dropdown terminal (glyph_term wrapper)
│   └── about.c/.h         "About Aegis" window
├── pkg/
│   └── caps.d/lumen       capability policy (installed to /etc/aegis/caps.d)
├── assets/
│   ├── Inter-Regular.ttf          desktop UI font
│   ├── JetBrainsMono-Regular.ttf  monospace/terminal font
│   ├── logo.raw                   Aegis logo (width/height header + RGBA)
│   └── claude.raw                 "Built with Claude Code" mark
├── tools/
│   ├── fetch-glyph.sh     fetch + unpack the pinned glyph toolkit artifact
│   └── pack.sh            build + sign lumen.hpkg
└── docs/
    └── ARCHITECTURE.md    compositor internals in depth
```

Build outputs (`lumen.elf`, `lumen.hpkg`, `lumen.hpkg.sig`), the fetched
`toolkit/`, and the `vendor/` cache are git-ignored.

---

## See also

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — the compositing pipeline,
  the protocol exchange, input/cursor handling, the dropdown terminal, and the
  About window, in detail.
- [`AspisOS/glyph`](https://github.com/AspisOS/glyph) — the toolkit lumen builds
  against and the home of the window protocol.
- [`AspisOS/Aegis`](https://github.com/AspisOS/Aegis) — the kernel.
