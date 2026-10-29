;; Pgr4\Settings.lua: a page of the port's settings (Pgr4.Page, games/pgr4/src/pgr4_settings.cpp),
;; made as the game's own option screens are (Options\General.lua, and the scrolling of
;; ContentServer\ReplaySearchVars.lua): a spinner per setting in a frame, up and down to choose
;; one, left and right to change it, A to apply and save the page, B to leave it unchanged. The
;; page is Pgr4.page, set by the menu that opens the screen (patches/Screens/Options/MainMenu.fnl).
;; A row with a condition (shown_with) is shown only while the row it depends on shows one of
;; its choices, and the rows below it close up.

(local titles {:display "DISPLAY" :driving "DRIVING" :developer "DEVELOPER"})

;; Where a hidden row is put: inactive, a spinner still draws its label, but the frame clips it.
(local parked-y 10000)

(global GUI {:finished false
             :started false
             :optionChosen 0
             :offset 0
             :target_offset 0
             :max_offset 0
             :scroll_group {:type UIEnums.ButtonTypes.GROUPSTART
                            :group_type UIEnums.GroupType.OFFSET
                            :x 0
                            :y 0}})

(fn shown? [option]
  (let [condition option.shown_with]
    (or (= nil condition)
        (let [by (. GUI.by_id condition.id)]
          (and (shown? by) (= true (. condition.choices (UIButtons.GetSelection by.ID))))))))

;; Marks the chosen row and scrolls it into the frame's upper half where the page allows. Every
;; row is unmarked before the chosen one is marked, as General.lua's ActivateOptions does.
(fn activate-options []
  (each [_ option (ipairs GUI.options)]
    (UIButtons.SetSelected option.ID false))
  (UIButtons.SetSelected (. GUI.rows (+ GUI.optionChosen 1) :ID) true)
  (let [offset (- 100 (* UIGlobals.options_cr GUI.optionChosen))]
    (set GUI.target_offset (math.max (- GUI.max_offset) (math.min 0 offset)))))

;; Shows the rows whose conditions hold, one below the other, and keeps the chosen one chosen.
(fn lay-out []
  (let [chosen (. GUI.rows (+ GUI.optionChosen 1))]
    (set GUI.rows [])
    (each [_ option (ipairs GUI.options)]
      (let [shown (shown? option)]
        (UIButtons.SetActive option.ID shown)
        (UIButtons.ChangePosition option.ID GUI.x
                                  (if shown
                                      (+ GUI.top (* (length GUI.rows) UIGlobals.options_cr))
                                      parked-y)
                                  0)
        (when shown
          (set GUI.optionChosen (if (= option chosen) (length GUI.rows) GUI.optionChosen))
          (set (. GUI.rows (+ (length GUI.rows) 1)) option))))
    (set GUI.optionChosen (math.min GUI.optionChosen (- (length GUI.rows) 1)))
    (set GUI.max_offset
         (math.max 0 (- (* UIGlobals.options_cr (length GUI.rows)) GUI.visible)))
    (each [_ id (ipairs GUI.scroll_bar)]
      (UIButtons.SetActive id (> GUI.max_offset 0)))
    (each [_ option (ipairs GUI.options)]
      (set option.shown_selection (UIButtons.GetSelection option.ID))))
  (activate-options))

;; Whether a row another depends on shows another choice than at the last lay-out.
(fn conditions-changed? []
  (accumulate [changed false _ option (ipairs GUI.options)]
    (or changed
        (and option.controls
             (not= option.shown_selection (UIButtons.GetSelection option.ID))))))

(fn leave []
  (UIScreen.SetNextScreen "Options\\MainMenu.lua"))

(global Init
        (fn []
          (SetupFadePanel)
          (SetupScreenTimers)
          (SetupInfoText UIText.IDS_CANCEL_B_SAVE_A 1 false)
          (SetupSlideMenuInfo)
          (set GUI.page (or Pgr4.page :display))
          (set GUI.options (Pgr4.Page GUI.page))
          (set GUI.by_id (collect [_ option (ipairs GUI.options)]
                           (values option.id option)))
          (each [_ option (ipairs GUI.options)]
            (when option.shown_with
              (set (. GUI.by_id option.shown_with.id :controls) true)))
          (set GUI.rows [])
          (let [frame (GetDefaultFrame 113.33333333333334 UIGlobals.screen.safe_top
                                       453.3333333333333 (- UIGlobals.screen.safe_bottom 30))
                width 320
                rows (* UIGlobals.options_cr (length GUI.options))]
            (set frame.panel 1)
            (UIButtons.AddButton frame)
            (SetupFrameTitle frame (Pgr4.Text (or (. titles GUI.page) GUI.page)))
            (set GUI.x (- 350 (* width 0.5) 16))
            (set GUI.top (+ frame.y UIGlobals.frame_mainbox_offset_y 12))
            (set GUI.visible (- frame.height UIGlobals.frame_mainbox_offset_y 28))
            ;; Sized for every row; hidden while the rows shown fit the frame.
            (set GUI.scroll_bar
                 (if (> rows GUI.visible)
                     (SetupFrameScrollBar frame true (* (/ GUI.visible rows) GUI.visible) 0
                                          UIEnums.Layer.LAYER_2)
                     []))
            (SetClipToFrame GUIBank.variable_spinner frame)
            (set GUI.scroll_group.ID (UIButtons.AddButton GUI.scroll_group))
            (set GUIBank.spinner_y (+ UIGlobals.screen.safe_top 12))
            (set GUIBank.spinner_cr 40)
            (each [row option (ipairs GUI.options)]
              (set option.ID
                   (SetupVariableSpinner option.label
                                         (+ GUI.top (* (- row 1) UIGlobals.options_cr)) width
                                         GUI.x 1))
              ;; The spinner lists its items last added first.
              (for [index (length option.choices) 1 -1]
                (UIButtons.AddItem option.ID (- index 1) (. option.choices index) false))
              (UIButtons.SetSelection option.ID option.value)))
          (SetupGroupEnd)
          (let [spinner GUIBank.variable_spinner]
            (set spinner.clipx nil)
            (set spinner.clipsizex nil)
            (set spinner.clipy nil)
            (set spinner.clipsizey nil))
          (lay-out)))

(global StartLoop (fn [_delta] true))

;; A move between rows is the screen's own (true), as in ReplaySearchVars.lua.
(global MessageUpdate
        (fn [message]
          (when (not GUI.finished)
            (case message
              :BUTTON_UP
              (do
                (PlayMenuMove)
                (set GUI.optionChosen (if (> GUI.optionChosen 0)
                                          (- GUI.optionChosen 1)
                                          (- (length GUI.rows) 1)))
                (activate-options)
                true)
              :BUTTON_DOWN
              (do
                (PlayMenuMove)
                (set GUI.optionChosen (if (< GUI.optionChosen (- (length GUI.rows) 1))
                                          (+ GUI.optionChosen 1)
                                          0))
                (activate-options)
                true)
              :BUTTON_NEXT
              (do
                (set GUI.finished true)
                (PlayMenuNext)
                (Pgr4.Apply GUI.page
                            (collect [_ option (ipairs GUI.options)]
                              (let [selection (UIButtons.GetSelection option.ID)]
                                (when (not= selection option.value)
                                  (values option.id selection)))))
                (leave)
                false)
              :BUTTON_BACK
              (do
                (set GUI.finished true)
                (PlayMenuBack)
                (leave)
                false)
              _
              false))))

;; The UI marks a row of its own once Init has run, so the chosen one is marked again on the
;; first frame. The rows follow the choices they depend on, and the frame eases to the chosen
;; row's offset, as ReplaySearchVars' does.
(global FrameUpdate
        (fn [delta]
          (when (not GUI.started)
            (set GUI.started true)
            (activate-options))
          (when (conditions-changed?)
            (lay-out))
          (when (not= GUI.offset GUI.target_offset)
            (set GUI.offset (- GUI.offset (* (- GUI.offset GUI.target_offset)
                                             (math.min 1 (* delta 4)))))
            (when (< (math.abs (- GUI.offset GUI.target_offset)) 0.5)
              (set GUI.offset GUI.target_offset))
            (UIButtons.ChangePosition GUI.scroll_group.ID 0 GUI.offset)
            (when (> GUI.max_offset 0)
              (MoveScrollBarV (/ (- GUI.offset) GUI.max_offset))))))

(global Render (fn []))

(global End (fn []))

(global EndLoop (fn [_delta] true))
