;; UIGlobals.lua, after the game's: the screen as tall as the frame. The UI lays out in a 640x480
;; space whose 480 the HUD's projection maps to the game's 720 rows, centered in a taller frame
;; (src/native/d3d_state.cpp). The game widens that space to the frame's sides at 16:9 (UIEnums'
;; SCREEN_LEFT and SCREEN_WIDTH) but keeps it 0 to 480 tall, so what the UI anchors to the
;; screen's edges (UIGlobals.screen) kept to the central 720 rows of a 16:10 frame; with the
;; screen as tall as the frame it reaches the top and bottom as it does the sides. The safe area
;; keeps the game's 6 %. RedoScreenGlobals, which the game calls as a race loads, sets the game's
;; height again, and the frame's after it.

(fn frame-height []
  (let [screen UIGlobals.screen
        height (Pgr4.ScreenHeight)
        top (/ (- 480 height) 2)
        bottom (+ top height)
        margin (* height 0.06)]
    (set screen.top top)
    (set screen.bottom bottom)
    (set screen.height height)
    (set screen.safe_top (+ top margin))
    (set screen.safe_bottom (- bottom margin))))

(frame-height)

(let [game-redo RedoScreenGlobals]
  (global RedoScreenGlobals
          (fn [...]
            (game-redo ...)
            (frame-height))))
