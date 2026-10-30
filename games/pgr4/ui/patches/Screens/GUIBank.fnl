;; Screens\GUIBank.lua, after the game's: the music player and the previous screen's image span the
;; frame (UIGlobals.frame, UIGlobals.fnl) rather than the game's screen.

;; The music player (SetupMusicPlayer), which the HUD and the replays show at the bottom, is
;; placed in rows 0 to 480 (GUIBank.music: shown at y 420, hidden at 560, clipped to the rows).
;; It moves down with the frame's bottom, as HudHelper.fnl's items do, and its clip spans the
;; frame.
(let [frame UIGlobals.frame
      music GUIBank.music
      added (- frame.bottom 480)]
  (set music.y (+ music.y added))
  (set music.to_y (+ music.to_y added))
  (set music.clipy frame.top)
  (set music.clipsizey frame.height))

;; The previous screen's image (GUIBank.fade_panel, SetupFadePanel: the frame's copy in
;; RENDERTARGET1, at its half-texel offset), which a screen fades from, spans rows 0 to 480 about
;; the middle. It covers the frame, and keeps the offset in the frame's rows.
(let [frame UIGlobals.frame
      panel GUIBank.fade_panel
      image (. panel.textures 1)]
  (set image.v (/ (* image.v panel.height) frame.height))
  (set panel.height frame.height))
