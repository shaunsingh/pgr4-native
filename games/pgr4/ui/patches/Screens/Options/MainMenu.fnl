;; Options\MainMenu.lua, after the game's: the port's settings pages, Display and Developer,
;; before Game Credits. Each opens Pgr4\Settings.lua (Screens/Pgr4/Settings.fnl) with its page.

(local display 100)
(local developer 101)
(local pages {display :display developer :developer})
(local credits 7) ; the game's item id of Game Credits

(local game-init Init)
(local game-message-update MessageUpdate)

;; The game's Init with the items added before the credits' as its own are: an item, its help
;; line, its sticker.
(global Init
        (fn []
          (let [add-item UIButtons.AddItem]
            (set UIButtons.AddItem
                 (fn [menu id text stored]
                   (when (and (= menu GUI.menu_ID) (= id credits))
                     (add-item menu display (Pgr4.Text "DISPLAY") false)
                     (UIButtons.AddItemHelp menu
                                            (Pgr4.Text "Renderer, antialiasing, resolution and frame pacing"))
                     (set (. GUI.sticker_types display) UIEnums.Stickers.OP_GRAPHICS)
                     (add-item menu developer (Pgr4.Text "DEVELOPER") false)
                     (UIButtons.AddItemHelp menu (Pgr4.Text "Debug keys, views and logs"))
                     (set (. GUI.sticker_types developer) UIEnums.Stickers.OP_GENERAL))
                   (add-item menu id text stored)))
            (let [(ok message) (pcall game-init)]
              (set UIButtons.AddItem add-item)
              (when (not ok)
                (error message 0))))))

(global MessageUpdate
        (fn [message ...]
          (let [page (and (= message :BUTTON_NEXT) (not GUI.finished)
                          (. pages (UIButtons.GetSelection GUI.menu_ID)))]
            (if page
                (do
                  (set GUI.finished true)
                  (PlayMenuNext)
                  (StoreMenuSelection GUI.menu_ID)
                  (GothamSystem.SetMusicState 22)
                  (set Pgr4.page page)
                  (UIScreen.SetNextScreen "Pgr4\\Settings.lua")
                  false)
                (game-message-update message ...)))))
