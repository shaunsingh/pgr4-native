;; Screens\GUIBank.lua, after the game's: the HUD's viewports are the game's. GetViewportDetails
;; splits UIGlobals.screen, which UIGlobals.fnl makes as tall as the frame, into the players'
;; viewports; the HUD lays out in them (HudHelper's AdjustPositionAndSizeForViewports) and the race
;; loading sends them to the HUD's own widgets (SendViewportDetails), both by fractions of their
;; height, so a taller viewport would spread the HUD over it. Each viewport keeps to the game's
;; rows 0 to 480 instead, and HudHelper.fnl moves the items to the frame's edges.

(local game-viewport-details GetViewportDetails)

(global GetViewportDetails
        (fn [...]
          (let [viewport (game-viewport-details ...)
                top (math.max viewport.topLeftY 0)
                bottom (math.min (+ viewport.topLeftY viewport.sizeY) 480)]
            (set viewport.topLeftY top)
            (set viewport.sizeY (- bottom top))
            viewport)))
