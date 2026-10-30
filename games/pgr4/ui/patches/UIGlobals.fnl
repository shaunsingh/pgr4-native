;; UIGlobals.lua, after the game's: the frame's rows in the UI's space. The UI lays out in a
;; 640x480 space whose 480 rows the HUD's projection maps to the game's 720, centered in a taller
;; frame (src/native/d3d_state.cpp). The menus and loading screens keep the game's screen
;; (UIGlobals.screen, rows 0 to 480) and their layout with it. What belongs to the frame rather
;; than to a screen's layout uses UIGlobals.frame, its rows top to bottom (-26.7 to 506.7 for an
;; 800-row frame): the race HUD (Screens/HudHelper.fnl), the music player and the previous
;; screen's image (Screens/GUIBank.fnl), and the screen's washes below.

(let [height (Pgr4.ScreenHeight)
      top (/ (- 480 height) 2)]
  (set UIGlobals.frame {: top :bottom (+ top height) : height}))

;; A wash, a graphic of one colour (the null texture) over the whole game screen, covers the frame:
;; the fades to a colour (GUIBank.fade_gfx) and the backdrops that dim what is behind a menu
;; (GUIBank.fade_background0, behind the pause menu and the popups). A textured graphic keeps its
;; shape and rows.
(fn wash? [button]
  (let [texture (?. button.textures 1)]
    (and (= button.type UIEnums.ButtonTypes.GRAPHIC) button.y button.height texture
         (= texture.name UITexture.Textures.null) (<= button.y 0)
         (>= (+ button.y button.height) 480))))

(let [game-add UIButtons.AddButton]
  (set UIButtons.AddButton
       (fn [button ...]
         (when (wash? button)
           (set button.y UIGlobals.frame.top)
           (set button.height UIGlobals.frame.height))
         (game-add button ...))))
