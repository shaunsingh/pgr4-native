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

;; The music player (SetupMusicPlayer), which the HUD and the replays show at the bottom, is
;; placed in rows 0 to 480 (GUIBank.music: shown at y 420, hidden at 560, clipped to the rows).
;; It moves down with the frame's bottom, as HudHelper.fnl's items do, and its clip spans the
;; screen.
(let [music GUIBank.music
      added (- UIGlobals.screen.bottom 480)]
  (set music.y (+ music.y added))
  (set music.to_y (+ music.to_y added))
  (set music.clipy UIGlobals.screen.top)
  (set music.clipsizey UIGlobals.screen.height))

;; The screen's fades span rows 0 to 480: the fade to a colour (GUIBank.fade_gfx, SetupScreenFade) from
;; the top, and the previous screen's image (GUIBank.fade_panel, SetupFadePanel: the frame's copy in
;; RENDERTARGET1, at its half-texel offset) about the middle. Both cover the screen, and the image
;; keeps the offset in the frame's rows.
(let [screen UIGlobals.screen
      fade GUIBank.fade_gfx
      panel GUIBank.fade_panel
      image (. panel.textures 1)]
  (set fade.y screen.top)
  (set fade.height screen.height)
  (set image.v (/ (* image.v panel.height) screen.height))
  (set panel.height screen.height))
