;; UIGlobals.lua, after the game's: the screen as tall as the frame. The UI lays out in a 640x480
;; space whose 480 the HUD's projection maps to the game's 720 rows, centered in a taller frame
;; (src/native/d3d_state.cpp). The game widens that space to the frame's sides at 16:9 (UIEnums'
;; SCREEN_LEFT and SCREEN_WIDTH) but keeps it 0 to 480 tall, so what the UI anchors to the
;; screen's edges (UIGlobals.screen) kept to the central 720 rows of a 16:10 frame; with the
;; screen as tall as the frame it reaches the top and bottom as it does the sides. The safe area
;; keeps the game's 6 %. RedoScreenGlobals, which the game calls as a race loads, sets the game's
;; height again, and the frame's after it.

;; The rows the frame's safe top is above the game's, by which the UI's 3D models move (below).
(var safe-top-raise 0)

(fn frame-height []
  (let [screen UIGlobals.screen
        height (Pgr4.ScreenHeight)
        top (/ (- 480 height) 2)
        bottom (+ top height)
        margin (* height 0.06)]
    (set safe-top-raise (- screen.safe_top (+ top margin)))
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

;; The UI's 3D models (true_3d buttons: the arcade's medals) are placed in the UI view's space, x
;; and y at depth z, to sit in frames the game places from the safe top (GetDefaultFrame at
;; safe_top + 75 in PGRArcade\ChooseChapter.lua, ChooseEvent.lua and ChooseDifficulty.lua), and
;; move up with the safe top as the frames do. A row of the game's 480 spans z * tan(fov / 2) / 240
;; at depth z in the UI view, whose field of view is the view's default of 1 rad (sub_822C0078) and
;; whose rows Pgr4ViewProjectionHook keeps at any frame height.
(let [game-add UIButtons.AddButton
      row-at-unit-depth (/ (math.tan 0.5) 240)]
  (set UIButtons.AddButton
       (fn [button ...]
         (if (and button.true_3d button.z)
             (let [moved (collect [k v (pairs button)]
                           (values k v))]
               (set moved.y (+ (or button.y 0) (* safe-top-raise button.z row-at-unit-depth)))
               (game-add moved ...))
             (game-add button ...)))))
