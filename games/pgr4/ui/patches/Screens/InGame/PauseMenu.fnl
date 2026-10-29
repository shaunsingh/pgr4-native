;; Ingame\PauseMenu.lua, after the game's: the port's settings pages, Display, Driving and
;; Developer, after Sound in each of the menu's lists, as the Options menu has them
;; (patches/Screens/Options/MainMenu.fnl). Each opens Pgr4\Settings.lua, which comes back here.

(local pages [{:op 100 :page :display :text "DISPLAY"}
              {:op 102 :page :driving :text "DRIVING"}
              {:op 101 :page :developer :text "DEVELOPER"}])

(local by-op (collect [_ {: op : page} (ipairs pages)]
               (values op page)))

;; The lists the game's Init chooses from by race mode and build; the frame and the menu are
;; sized from the list chosen.
(each [_ name (ipairs [:timeAttackUI :TournamentQualifyUI :defaultUI :defaultUIwithoutPhotoMode
                       :careerUI :networkUI])]
  (let [items (. GUI name)
        sound (accumulate [found nil at item (ipairs items)]
                (if (= item.op GUI.OP_SOUND) at found))]
    (when sound
      (each [i {: op : text} (ipairs pages)]
        (table.insert items (+ sound i) {: op :text (Pgr4.Text text)})))
    (set GUI.right_menu.num_rows (math.max GUI.right_menu.num_rows (length items)))))

(local game-message-update MessageUpdate)

(global MessageUpdate
        (fn [message ...]
          (let [page (and (= message :BUTTON_NEXT) (not GUI.finished)
                          (. by-op (UIButtons.GetSelection GUI.pause_menu_ID)))]
            (if page
                (do
                  (set GUI.finished true)
                  (PlayMenuNext)
                  (set Pgr4.page page)
                  (set Pgr4.page_from :pause)
                  (UIScreen.SetNextScreen "Pgr4\\Settings.lua")
                  false)
                (game-message-update message ...)))))
