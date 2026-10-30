;; Screens\HudHelper.lua, after the game's: the HUD's items move with the nearer of the frame's top
;; and bottom. The HUD lays out in the players' viewports, within the game's screen (rows 0 to
;; 480), which a 16:10 frame extends above and below (UIGlobals.frame, UIGlobals.fnl). An item
;; above its viewport's middle moves up by the rows the frame adds above the screen, one below it
;; down by those added below, so each keeps its distance to that edge of the frame. The HUD's own
;; widgets, which place themselves, move by the same rule (Pgr4HudWidgetHook).

(local game-adjust AdjustPositionAndSizeForViewports)

(fn anchored [viewport fraction y]
  (let [frame UIGlobals.frame
        top viewport.topLeftY
        bottom (+ top viewport.sizeY)]
    (if (and (< fraction 0.5) (<= top 0)) (+ y frame.top)
        (and (> fraction 0.5) (>= bottom 480)) (+ y (- frame.bottom 480))
        y)))

(global AdjustPositionAndSizeForViewports
        (fn [viewport item]
          (game-adjust viewport item)
          (if (= item.type UIEnums.ButtonTypes.TACHOMETER)
              (do
                (set item.centreLayoutY
                     (anchored viewport item.x_centreLayoutY item.centreLayoutY))
                (set item.sideLayoutY (anchored viewport item.x_sideLayoutY item.sideLayoutY))
                (set item.horizSplitLayoutY
                     (anchored viewport item.x_horizSplitLayoutY item.horizSplitLayoutY)))
              (let [fraction (if (and (= numberOfViewports 2) item.mp_y_percent)
                                 item.mp_y_percent
                                 item.y_percent)]
                (set item.y (anchored viewport fraction item.y))))))
