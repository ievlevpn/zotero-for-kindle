# Writing with the Scribe's pen

On a Kindle Scribe the pen writes on PDF pages. Strokes become Zotero `ink` annotations and sync like
highlights. Everywhere else (lists, menus, EPUB, where Zotero has no ink) the pen is a finger: a stroke
the visible screen doesn't take (`Screen::onPen` returns false for its Down) is fed to the gestures.

- Strokes made within 1.5 s of each other are one annotation, up to 24 strokes, 2000 points or 10 s
  (`kMaxInk*`); a page turn, a jump, the menus, another screen or the background save at once, and
  closing the book waits (at most 3 s) until every stroke is an annotation (`flushInk`, `onClose`).
  Strokes are simplified (Douglas-Peucker, 1 px). Ink is black, 7 px wide on the screen
  (`ReaderScreen::kInkWidthPx`), stored in PDF points.
- A quick pen touch that hardly moves, with nothing written before it, is a tap (menus, page turns);
  a dot right after writing stays ink (`kTapSlopPx`, `kTapMs`).
- The pen writes in the white bands beside a letterboxed page too (room for notes in the margin).
  Those strokes lie outside the PDF page: they are saved whole and shown on the Kindle, but Zotero
  desktop draws ink only inside the page and cuts the rest off.
- The eraser end takes away the strokes it crosses: an annotation is rewritten without them, and
  deleted when none is left, unless it has a comment or tags (then a toast points to Notes).
- Display panel, PDF, once the pen was used: "Pen: Write | Tap" (kept in the library). Tap makes the
  pen a finger on PDF pages too (turn pages, select text); holding the side button (Premium Pen) does
  the same for one stroke: a long press and drag selects text, or on blank space draws an area (a
  Zotero `image` annotation), as with a finger. While the menus' "Mark an area" waits, the pen
  draws the area without the button.
- While the pen is near the screen (and 0.7 s after it leaves), touches are taken for the palm and
  ignored (`App::kPalmMs`). "Near" lapses after 2 s without a pen event (`kPenStaleMs`), and the
  background resets it, so a lost Leave can't block touch.
- The line under the pen is drawn by the firmware's own "wet ink" (below), as in the Scribe's
  notebook. Without it (other devices, `ZK_NO_WET_INK=1`) the app draws the line itself, one A2
  update per 25 ms frame (`App::kInkFrameMs`): usable, but visibly behind the pen.

## Pieces
- `platform/pen_decoder.*`: evdev stream -> `PenEvent`s in window pixels (range scaling, axis swap,
  flips, quarter turns, window origin). No Linux headers; tested in `tests/test_pen_decoder.cpp`.
- `platform/kindle/evdev_pen.*`: finds the digitizer, logs its ranges (`pen:` in the log). Overrides:
  `ZK_PEN_DEVICE`, `ZK_PEN_SWAP_XY`, `ZK_PEN_FLIP_X`, `ZK_PEN_FLIP_Y`, `ZK_PEN_TURNS`.
- `platform/kindle/wet_ink.*`: client of the firmware's WetInkHandler.
- `App`: `Screen::onPen`, palm rejection, `Nav::paintNow` (draw and show one region at once),
  `Nav::setSystemInk` (wet ink on only while the asking screen is visible).
- `DocumentEngine::inkPosition` (PDF: device points -> PDF user space, the inverse of the ink branch
  of `highlights()`), `RenderWorker::ink` (every request answered, before a layout change).
- `ReaderScreen::onPen`: strokes, commit, eraser; wet ink is paused for menus, selection, the note
  editor and while the eraser end is near (the handler's own eraser would draw over the page).

## Scribe wet ink
The Scribe's instant line comes from `/app/bin/WetInkHandler` (upstart `/etc/upstart/wi.conf`): it reads
the raw digitizer (`/dev/input/event3`, "WacomDigitizer"), draws strokes straight into `/dev/fb0` over one
registered X window, and hands finished strokes to its client over SysV shared memory. The firmware
notebook (`kfxreader`, `StrokeIPCDryInk`) is its usual client. The protocol below was observed on a
Scribe (5.16.8) with an LD_PRELOAD logger on WetInkHandler (`tools/wetink/ipcshim.c`) while the notebook
was used; `tools/wetink/wetink-ctl.c` sends single messages for experiments. It is not documented by
Amazon and may change with firmware updates; the app falls back to drawing the line itself when the
handler doesn't answer.

Only on the 1st-gen Scribe (FBInk's device name "Scribe"), where the protocol was checked;
`ZK_WET_INK=1` forces it elsewhere, `ZK_NO_WET_INK=1` turns it off. In landscape the app draws the line
itself for now (the handler's canvas is registered portrait).

What the app does:
- Goes full screen (`PC:N` in the window title): the handler only draws over the top window, and the
  status bar would be it.
- Connects at start, registers its window, keeps the handler paused except while a PDF page takes
  handwriting, acknowledges every stroke and stroke context at once (the strokes themselves aren't
  used: the app reads the digitizer too), drains the handler's replies, and disconnects on exit.
  A session left open with unacknowledged strokes once took the Kindle framework down (a reboot fixed it):
  fatal signals and the stall watchdog acknowledge waiting strokes, turn the handler off and end the
  session (`WetInk::emergencyOff`). After an exit that skips all that (SIGKILL, the OOM killer) the
  launcher runs `zotero --reset-system-ink`, which acts only if the app's session marker
  (`/tmp/zk-wet-ink-session`) is still there: the firmware reader keeps a session of its own, and a
  blind disconnect would end it. A connect that isn't answered within 0.5 s takes back everything it
  posted (SysV semaphores outlive the app) and falls back to the app's own line.
- Pauses wet ink while the app is in the background (a system dialog over it) and ignores the pen there.
- Repaints where an ignored stroke (outside the page, or before wet ink paused) may have been inked.
- Reads the raw digitizer: the handler's re-sent copy (`stylus-custom`, event5) goes quiet while wet
  ink is on. Raw coordinates follow the panel, not the picture: the turn comes from
  `/sys/class/graphics/fb0/rotate` (3 = upright on the Scribe, raw = screen; 1 = upside down), read
  about once a second because a half turn doesn't resize the window.

## Transport
- Message queue key `0x537a6f` (client -> handler messages, handler -> client type 15 status).
- Semaphore set key `0x537a70`, 13 semaphores. The handler sleeps on sem 0; a client wakes it with sem 0 +1.
  - Not connected: the handler checks only sem 2 (connect request) and sem 4.
  - Connect: client sem 2 +1, sem 0 +1 -> handler "ipcProcessEstablishConnectionRequest", answers sem 3 +1,
    creates shm `0x537a74` (40 bytes, stroke context), posts sem 8, sem 1.
  - Connected: after each wake the handler checks sems 7, 9, 10, 12 and reads queued messages of each type.
  - Strokes: shm `0x537a71` (136 B header), `0x537a72` (points, ~1 KB), `0x537a75` (40 B); handler logs
    "ipcSendStroke: N" (sem 5) and waits for "ipcReceiveStrokeProcessed" (client ack: sem 6; the stroke
    context, sem 8, is acknowledged with sem 9).
  - Close: "ipcReceiveTerminateConnectionRequest" (probably sem 4).
- Handler log goes to syslog (`/var/log/messages`, tmpfs) with `KNSR::` tags; it names each message.

## Messages (queue 0x537a6f, mtype = message, payload little-endian)
| type | handler log name | payload |
|---|---|---|
| 1 | SetStylusMode | u8: 0x00 pen draws, 0x0a disabled (during setup), 0x0b off |
| 5 | exclusion regions | u8 count + floats (rects / circle x,y,r) |
| 6 | SetCanvasRect | 4 x u16: x, y, w, h (0,0,1860,2480) |
| 7 | SetActiveXWindowID | u32 X window id |
| 8 | SetCurrentThicknessForMode etc. | u8 index + float (index 0 = thickness, 3.5) |
| 9 | SetPanZoomScale | 2 x float (1.0, 1.0) |
| 12 | brush parameters request | 2 bytes |
| 15 (handler -> client) | brush parameters | 30 bytes |

Notebook open sequence: 1:0a, 7:window, 1:0a, 8:thickness, 5:exclusions, 1:00, 8, 6:canvas, 9:scale.

## Not verified on a device yet
- Landscape reading with wet ink (the quarter-turn direction is derived from `fb0/rotate`).
- Strokes made less than 1.5 s before the reader closes other than through the menus are lost.
- Wet ink drawn while the page is being repainted (just after a save) can vanish from the screen; the
  stroke itself is saved and comes back with the annotation.
- Kindles with a pen other than the Scribe (1st generation, firmware 5.16.8 tested).
