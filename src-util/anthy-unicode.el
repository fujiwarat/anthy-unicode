;;; anthy-unicode.el -- Anthy

;; Copyright (C) 2001 - 2007 KMC(Kyoto University Micro Computer Club)
;; Copyright (C) 2021 Takao Fujiwara <takao.fujiwara1@gmail.com>
;; Copyright (c) 2021 Red Hat, Inc.

;; Author: Yusuke Tabata<yusuke@kmc.gr.jp>
;;         Tomoharu Ugawa
;;         Norio Suzuki <suzuki@sanpobu.net>
;; Keywords: japanese

;; This file is part of Anthy

;;; Commentary:
;;
;; かな漢字変換エンジン Anthy を emacs から使うためのプログラム
;; Anthy ライブラリを使うためのコマンド anthy-agent-unicode を起動して、
;; anthy-agent-unicode とパイプで通信をすることによって変換の動作を行う
;;
;;
;; Funded by IPA未踏ソフトウェア創造事業 2001 10/10
;;
;; 開発は emacs21.2 上で行っていて minor-mode
;; もしくは leim としても使用できる
;; (set-input-method 'japanese-anthy)
;;
;; emacs19(mule),20,21,xemacs で動作する
;;
;;
;; 2003-08-24 XEmacs の候補選択モードバグに対応 (suzuki)
;;
;; 2001-11-16 EUC-JP -> ISO-2022-JP
;; 2021-10-26 ISO-2022-JP -> UTF-8
;;
;; TODO
;;  候補選択モードで候補をいっきに次のページにいかないようにする (2chスレ78)
;;  minibufffer の扱い
;;  isearch 対応
;;
;; 用語
;;  commit 文字列を確定すること
;;  preedit(プリエディット) 確定前の文字列アンダーラインや強調の属性も含む
;;  segment(文節) 文法的な文節ではなく，同じ属性の文字列のかたまり
;;

;;; Code:
;(setq debug-on-error t)

(defvar anthy-default-enable-enum-candidate-p t
  "これを設定すると次候補を数回押した際に候補の一覧から選択するモードになります．")

(defvar anthy-personality ""
  "パーソナリティ")

(defvar anthy-preedit-begin-mark "|"
  "変換時の先頭に付く文字列")

(defvar anthy-preedit-delim-mark "|"
 "変換時の文節の区切りに使われる文字列")

(defvar anthy-accept-timeout 50)
(if (>= emacs-major-version 22)
    (setq anthy-accept-timeout 1))

(defconst anthy-working-buffer " *anthy*")
(defvar anthy-agent-unicode-process nil
  "anthy-agent-unicodeのプロセス")
(defvar anthy-use-hankaku-kana t)
;;
(defvar anthy-agent-unicode-command-list '("anthy-agent-unicode")
  "anthy-agent-unicodeのPATH 名")

;; utf-8-unix が実際に使えるか。名前が在るだけでは足りない。Emacs 21.4 は
;; utf-8 という coding system を持っているが、CJK を入れる先が無いので三
;; byte の漢字が byte のまま残る。一文字復号して長さを見る。
;; XEmacs の coding-system-p は symbol ではなく coding system の object を
;; 取るので、symbol で訊くと常に nil になる。XEmacs 側の綴りは
;; find-coding-system で、GNU Emacs には無い。
(defun anthy-utf-8-unix-p ()
  (and (if (fboundp 'find-coding-system)
	   (find-coding-system 'utf-8-unix)
	 (coding-system-p 'utf-8-unix))
       (= (length (decode-coding-string "\346\227\245" 'utf-8)) 1)))

;; mapc は Emacs 21 から。無い処理系では anthy 9100h と同じ mapcar を
;; 使う。この行は返り値を捨てるので、どちらでも同じことになる。
(defvar anthy-mapc-function
  (if (fboundp 'mapc)
      'mapc
    'mapcar))

;; face
(defvar anthy-highlight-face nil)
(defvar anthy-underline-face nil)
(copy-face 'highlight 'anthy-highlight-face)
(if (not (featurep 'xemacs))
    (set-face-underline 'anthy-highlight-face t))
(copy-face 'underline 'anthy-underline-face)

;;
(defvar anthy-xemacs
  (if (featurep 'xemacs)
      t nil))
(if anthy-xemacs
    (require 'overlay))
;;
(defvar anthy-mode-map nil
  "AnthyのASCIIモードのキーマップ")
(or anthy-mode-map
    (let ((map (make-keymap))
	  (i 32))
      (define-key map (char-to-string 10) 'anthy-insert)
      (define-key map (char-to-string 17) 'anthy-insert)
      (while (< i 127)
	(define-key map (char-to-string i) 'anthy-insert)
	(setq i (+ 1 i)))
      (setq anthy-mode-map map)))
;;
(defvar anthy-preedit-keymap nil
  "Anthyのpreeditのキーマップ")
(or anthy-preedit-keymap
    (let ((map (make-keymap))
	  (i 0))
      ;; 通常の文字に対して
      (while (< i 128)
	(define-key map (char-to-string i) 'anthy-insert)
	(setq i (+ 1 i)))
      ;; 文節の伸縮
      (define-key map [(shift left)] 'anthy-insert)
      (define-key map [(shift right)] 'anthy-insert)
      ;; 文節の移動
      (define-key map [left] 'anthy-insert)
      (define-key map [right] 'anthy-insert)
      (define-key map [backspace] 'anthy-insert)
      (setq anthy-preedit-keymap map)))

;; anthy-agent-unicode に送る際にキーをエンコードするためのテーブル
(defvar anthy-keyencode-alist
  '((1 . "(ctrl A)") ;; \C-a
    (2 . "(left)") ;; \C-b
    (4 . "(ctrl D)") ;; \C-d
    (5 . "(ctrl E)") ;; \C-e
    (6 . "(right)") ;; \C-f
    (7 . "(esc)") ;; \C-g
    (8 . "(ctrl H)") ;; \C-h
    (9 . "(shift left)") ;; \C-i
    (10 . "(ctrl J)")
    (11 . "(ctrl K)")
    (13 . "(enter)") ;; \C-m
    (14 . "(space)") ;; \C-n
    (15 . "(shift right)") ;; \C-o
    (16 . "(up)") ;; \C-p
    (32 . "(space)")
    (40 . "(opar)") ;; '('
    (41 . "(cpar)") ;; ')'
    (127 . "(ctrl H)")
    ;; emacs map
    (S-right . "(shift right)")
    (S-left . "(shift left)")
    (right . "(right)")
    (left . "(left)")
    (up . "(up)")
    (backspace . "(ctrl H)")
    ;; xemacs
    ((shift right) . "(shift right)")
    ((shift left) . "(shift left)")
    ((right) . "(right)")
    ((left) . "(left)")
    ((up) . "(up)"))
  "キーのイベントをanthy-agent-unicodeに送るための対応表")

;; モードラインの文字列
(defvar anthy-mode-line-string-alist
  '(("hiragana" . " あ")
    ("katakana" . " ア")
    ("alphabet" . " A")
    ("walphabet" . " Ａ")
    ("hankaku_kana" . " (I1")
    )
  "モード名とモードラインの文字列の対応表")

;; 最後に割り当てたcontext id
(defvar anthy-last-context-id 1)

;; From skk-macs.el From viper-util.el.  Welcome!
;; Fixed emacs batch-byte-compile of anthy.el about the below warning:
;; "anthy.el:163:1:Warning: !! The file uses old-style backquotes !!"
;; https://stackoverflow.com/questions/8109665/how-fix-emacs-error-old-style-backquotes-detected
(defmacro anthy-deflocalvar (var default-value &optional documentation)
  `(progn
       (defvar ,var ,default-value
	 ,(format "%s\n\(buffer local\)" documentation))
       (make-variable-buffer-local ',var)
       ))

;; buffer local variables
(anthy-deflocalvar anthy-context-id nil "コンテキストのid")
; モードの管理
(anthy-deflocalvar anthy-minor-mode nil)
(anthy-deflocalvar anthy-mode nil)
(anthy-deflocalvar anthy-leim-active-p nil)
(anthy-deflocalvar anthy-saved-mode nil)
; プリエディット
(anthy-deflocalvar anthy-preedit "")
(anthy-deflocalvar anthy-preedit-start 0)
(anthy-deflocalvar anthy-preedit-overlays '())
(anthy-deflocalvar anthy-mode-line-string " A")
; 候補列挙
(anthy-deflocalvar anthy-enum-candidate-p nil)
(anthy-deflocalvar anthy-enum-rcandidate-p nil)
(anthy-deflocalvar anthy-candidate-minibuffer "")
(anthy-deflocalvar anthy-enum-candidate-list '()
		   "今列挙している候補の情報((画面内のindex 候補のindex . 候補文字列) ..)")
(anthy-deflocalvar anthy-enable-enum-candidate-p 
  (cons anthy-default-enable-enum-candidate-p nil)
  "このバッファで候補の列挙を行うかどうか")
(anthy-deflocalvar anthy-current-candidate-index 0)
(anthy-deflocalvar anthy-current-candidate-layout-begin-index 0)
(anthy-deflocalvar anthy-current-candidate-layout-end-index 0)
; 入力状態
(anthy-deflocalvar anthy-current-rkmap "hiragana")
; undo
(anthy-deflocalvar anthy-buffer-undo-list-saved nil)

;;
(defvar anthy-wide-space "　" "スペースを押した時に出て来る文字")

;;; setup minor-mode
;; minor-mode-alist
(if (not
     (assq 'anthy-minor-mode minor-mode-alist))
    (setq minor-mode-alist
       (cons
	(cons 'anthy-minor-mode '(anthy-mode-line-string))
	minor-mode-alist)))
;; minor-mode-map-alist
(if (not
     (assq 'anthy-minor-mode minor-mode-map-alist))
    (setq minor-mode-map-alist
       (cons
	(cons 'anthy-minor-mode anthy-mode-map)
	minor-mode-map-alist)))

;;
(defun anthy-process-sentinel (proc stat)
  "プロセスの状態が変化したら参照を消して，次に再起動できるようにする"
  (message "%s" stat)
  (anthy-mode-off)
  (setq anthy-agent-unicode-process nil))

;;; status
(defun anthy-update-mode-line ()
  "モードラインを更新する"
  (let ((a (assoc anthy-current-rkmap anthy-mode-line-string-alist)))
    (if a
	(progn
	 (setq anthy-mode-line-string (cdr a))
	 (setq current-input-method-title
	       (concat "<Anthy:" (cdr a) ">")))))
  (force-mode-line-update))

;;; preedit control
(defun anthy-erase-preedit ()
  "プリエディットを全部消す"
  (if (> (string-width anthy-preedit) 0)
      (let* ((str anthy-preedit)
	     (len (length str))
	     (start anthy-preedit-start))
	(delete-region start (+ start len))
	(goto-char start)))
  (setq anthy-preedit "")
  (funcall anthy-mapc-function 'delete-overlay anthy-preedit-overlays)
  (setq anthy-preedit-overlays nil))

(defun anthy-select-face-by-attr (attr)
  "文節の属性に応じたfaceを返す"
  (if (memq 'RV attr)
      'anthy-highlight-face
    'anthy-underline-face))

(defun anthy-enable-preedit-keymap ()
  "キーマップをプリエディットの存在する時のものに切替える"
;  (setq anthy-saved-buffer-undo-list buffer-undo-list)
;  (buffer-disable-undo)
  (setcdr
   (assq 'anthy-minor-mode minor-mode-map-alist)
   anthy-preedit-keymap))

(defun anthy-disable-preedit-keymap ()
  "キーマップをプリエディットの存在しない時のものに切替える"
;  (buffer-enable-undo)
;  (setq buffer-undo-list anthy-saved-buffer-undo-list)
  (setcdr
   (assq 'anthy-minor-mode minor-mode-map-alist)
   anthy-mode-map)
  (anthy-update-mode-line))

(defun anthy-insert-preedit-segment (str attr)
  "プリエディットを一文節文追加する"
  (let ((start (point))
	(end) (ol))
    (cond ((or (memq 'ENUM attr) (memq 'ENUMR attr))
	   (setq str (concat "<" str ">")))
	  ((memq 'RV attr) 
	   (setq str (concat "[" str "]"))))
    ; プリエディットの文字列を追加する
    (insert-and-inherit str)
    (setq end (point))
    ;; overlayによって属性を設定する
    (setq ol (make-overlay start end))
    (overlay-put ol 'face (anthy-select-face-by-attr attr))
    (setq anthy-preedit-overlays
	  (cons ol anthy-preedit-overlays))
    str))

(defvar anthy-select-candidate-keybind
  '((0 . "a")
    (1 . "s")
    (2 . "d")
    (3 . "f")
    (4 . "g")
    (5 . "h")
    (6 . "j")
    (7 . "k")
    (8 . "l")
    (9 . ";")))

;;;
;;; auto fill controll
;;; from egg.el

(defun anthy-do-auto-fill ()
  (if (and auto-fill-function (> (current-column) fill-column))
      (let ((ocolumn (current-column)))
	(funcall auto-fill-function)
	(while (and (< fill-column (current-column))
		    (< (current-column) ocolumn))
	  (setq ocolumn (current-column))
	  (funcall auto-fill-function)))))

;;
(defun anthy-check-context-id ()
  "バッファにコンテキストidが割り振られているかをチェックする"
  (if (null anthy-context-id)
      (progn
	(setq anthy-context-id anthy-last-context-id)
	(setq anthy-last-context-id
	      (+ anthy-last-context-id 1)))))

(defun anthy-get-candidate (idx)
  "agentから候補を一つ取得する"
  (anthy-send-recv-command 
   (concat " GET_CANDIDATE "
	   (number-to-string idx) "\n")))

;; 候補リストからミニバッファに表示する文字列を構成する
(defun anthy-make-candidate-minibuffer-string ()
  (let ((cand-list anthy-enum-candidate-list)
	(cur-elm)
	(str))
    (while cand-list
      (setq cur-elm (car cand-list))
      (let ((cand-str (cdr (cdr cur-elm)))
	    (cand-idx (car (cdr cur-elm)))
	    (sel-idx (car cur-elm)))
	(setq str (format (if (= anthy-current-candidate-index cand-idx)
			      "%s:[%s] "
			      "%s: %s  ")
			  (cdr (assoc sel-idx anthy-select-candidate-keybind))
			  cand-str)))
      (setq anthy-candidate-minibuffer
	    (concat str
		    anthy-candidate-minibuffer))
      (setq cand-list (cdr cand-list)))))

;; 表示する候補のリストに画面内でのインデックスを付ける
(defun anthy-add-candidate-index (lst)
  (let ((i 0)
	(res nil))
    (while lst
      (setq res
	    (cons
	     (cons i (car lst))
	     res))
      (setq i (1+ i))
      (setq lst (cdr lst)))
    res))


;; 文字の幅を計算して、表示する候補のリストを作る
(defun anthy-make-candidate-index-list (base nr l2r)
  (let ((width (frame-width))
	(errorp nil)
	(i 0)
	(repl)
	(cand-idx)
	(lst))
    ;; loop
    (while (and
	    (if l2r
		(< (+ base i) nr)
	      (<= 0 (- base i)))
	    (> width 0)
	    (< i (length anthy-select-candidate-keybind))
	    (not errorp))
      (if l2r
	  (setq cand-idx (+ base i))
	(setq cand-idx (- base i)))
      (setq repl (anthy-get-candidate cand-idx))
      (if (listp repl)
	  ;; append candidate
	  (let ((cand-str (car repl)))
	    (setq width (- width (string-width cand-str) 5))
	    (if (or (> width 0) (null lst))
		(setq lst
		      (cons
		       (cons cand-idx cand-str)
		       lst))))
	;; erroneous candidate
	(setq errorp t))
      (setq i (1+ i)))
    (if errorp
	nil
      lst)))
  

;; 表示する候補のリストを作る
(defun anthy-calc-candidate-layout (base nr l2r)
  (let
      ((lst (anthy-make-candidate-index-list base nr l2r)))
    ;;カレントの候補番号を設定する
    (if l2r
	(progn
	  ;; 左から右の場合
	  ;; indexを一番右の候補に設定する
	  (anthy-get-candidate (car (car lst)))
	  (setq lst (reverse lst))
	  (setq anthy-current-candidate-index (car (car lst))))
      (progn
	;; 右から左の場合
	(setq anthy-current-candidate-index (car (car (reverse lst))))))
    ;;結果をセット
    (setq anthy-enum-candidate-list
	  (if lst
	      (anthy-add-candidate-index lst)
	    nil))))

;;
(defun anthy-layout-candidate (idx nr)
  "候補リストをminibufferへレイアウトする"
  (setq anthy-candidate-minibuffer "")
  (setq anthy-enum-candidate-list '())
  ;; 左->右 or 右->左にレイアウトする
  (if anthy-enum-candidate-p
      (anthy-calc-candidate-layout idx nr 't)
    (anthy-calc-candidate-layout idx nr nil))
  (anthy-make-candidate-minibuffer-string)
  ;; 結果を表示する
  (if anthy-enum-candidate-list
      (progn
	(message "%s" anthy-candidate-minibuffer)
	(setq anthy-current-candidate-layout-begin-index
	      (car (cdr (car (reverse anthy-enum-candidate-list)))))
	(setq anthy-current-candidate-layout-end-index
	      (car (cdr (car anthy-enum-candidate-list)))))

    nil))

(defun anthy-update-preedit (stat ps)
  "プリエディットを更新する"
  (let ((cursor-pos nil)
	(num-candidate 0)
	(idx-candidate 0)
	(enum-candidate
	 (or anthy-enum-candidate-p
	     anthy-enum-rcandidate-p)))
    ;; erase old preedit
    (anthy-erase-preedit)

    ;; 入力キャンセル時にundoリストを繋げる
    (if (and (= (length ps) 0)  anthy-buffer-undo-list-saved )
	(progn
;	  (message "enable")
	  (buffer-enable-undo)
	  (setq buffer-undo-list anthy-buffer-undo-list)
	  (setq anthy-buffer-undo-list-saved nil)
	  ))

    (anthy-disable-preedit-keymap)
    ;; insert new preedit
    (setq anthy-preedit-start (point))
    (setq anthy-enum-candidate-p nil)
    (setq anthy-enum-rcandidate-p nil)
    (if (member stat '(2 3 4))
	(progn
	  (setq anthy-preedit
		(concat anthy-preedit anthy-preedit-begin-mark))
	  (anthy-insert-preedit-segment anthy-preedit-begin-mark '())

	  ;; 入力開始と同時にundoリストを無効化
	  (if (not anthy-buffer-undo-list-saved)
	      (progn
		;(message "disable")
		(setq anthy-buffer-undo-list (cdr buffer-undo-list))
		(buffer-disable-undo)
		(setq anthy-buffer-undo-list-saved 't)
		)
	    ;(message "not saved")
	    )

	  ))

    ;; 各文節に対して
    (while ps
      (let ((cur (car ps)))
	(setq ps (cdr ps))
	(cond
	 ((eq cur 'cursor)
	  (setq cursor-pos (point)))
	 ((string-equal (car (cdr cur)) "")
	  nil)
	 (t
	  (let ((nr (car (cdr (cdr (cdr cur)))))
		(idx (car (cdr (cdr cur))))
		(str (car (cdr cur)))
		(attr (car cur)))
	    (setq str (anthy-insert-preedit-segment str attr))
	    (cond ((and (car anthy-enable-enum-candidate-p) (memq 'ENUM attr))
		   ;; 順方向の候補列挙
		   (setq anthy-enum-candidate-p t)
		   (setq idx-candidate idx)
		   (setq num-candidate nr))
		  ((and (car anthy-enable-enum-candidate-p) (memq 'ENUMR attr))
		   ;; 逆方向の候補列挙
		   (setq anthy-enum-rcandidate-p t)
		   (setq idx-candidate idx)
		   (setq num-candidate nr)))
	    (setq anthy-preedit
		  (concat anthy-preedit str))
	    (if (and (member stat '(3 4)) (not (eq ps '())))
		(progn
		  (setq anthy-preedit
			(concat anthy-preedit anthy-preedit-delim-mark))
		  (anthy-insert-preedit-segment anthy-preedit-delim-mark '()))))))))
    ;; 候補一覧の表示開始チェック
    (if (and (not enum-candidate)
	     (or anthy-enum-candidate-p anthy-enum-rcandidate-p))
	(setq anthy-current-candidate-layout-begin-index 0))
    ;; 候補の列挙を行う
    (if (or anthy-enum-candidate-p anthy-enum-rcandidate-p)
	(anthy-layout-candidate idx-candidate num-candidate))
    ;; preeditのkeymapを更新する
    (if (member stat '(2 3 4))
	(anthy-enable-preedit-keymap))
    (if cursor-pos (goto-char cursor-pos))))

; suzuki : Emacs / XEmacs で共通の関数定義
(defun anthy-encode-key (ch)
  (let ((c (assoc ch anthy-keyencode-alist)))
    (if c
	(cdr c)
      (if (and
	   (integerp ch)
	   (> ch 32))
	  (char-to-string ch)
	nil))))

(defun anthy-restore-undo-list (commit-str)
  (let* ((len (length commit-str))
	 (beginning (point))
	 (end (+ beginning len)))
    (setq buffer-undo-list
	  (cons (cons beginning end)
		(cons nil anthy-saved-buffer-undo-list)))
	 ))

(defun anthy-proc-agent-reply (repl)
  (let*
      ((stat (car repl))
       (body (cdr repl))
       (commit "")
       (commitlen nil)
       (preedit nil))
    ;; 各文節を処理する
    (while body
      (let* ((cur (car body))
	     (pe nil))
	(setq body (cdr body))
	(if (and
	     (listp cur)
	     (listp (car cur)))
	    (cond
	     ((eq (car (car cur)) 'COMMIT)
	      (setq commit (concat commit (car (cdr cur)))))
	     ((eq (car (car cur)) 'CUTBUF)
	      (let ((len (length (car (cdr cur)))))
		(copy-region-as-kill (point) (+ (point) len))))
	     ((memq 'UL (car cur))
	      (setq pe (list cur))))
	  (setq pe (list cur)))
	(if pe
	    (setq preedit (append preedit pe)))))
    ;; コミットされた文節を処理する
;    (anthy-restore-undo-list commit)
    (if (> (string-width commit) 0)
	(progn
	  (setq commitlen (length commit))
	  (anthy-erase-preedit)
	  (anthy-disable-preedit-keymap)
	  ; 先にコミットさせておく
	  (insert-and-inherit commit)
	  (anthy-do-auto-fill)

	  ;; コミット時に繋げる
	  (if anthy-buffer-undo-list-saved 
	      (progn
		;(message "enable")
		; 復帰させる前に，今commitした内容をリストに追加
		(setq anthy-buffer-undo-list
		      (cons (cons anthy-preedit-start
				  (+ anthy-preedit-start commitlen))
			    anthy-buffer-undo-list))
		(setq anthy-buffer-undo-list (cons nil anthy-buffer-undo-list))

		(buffer-enable-undo)

		(setq buffer-undo-list anthy-buffer-undo-list)

		(setq anthy-buffer-undo-list-saved nil)
		))

	  (run-hooks 'anthy-commit-hook)
	  ))
    (anthy-update-preedit stat preedit)
    (anthy-update-mode-line)))

(defun anthy-insert-select-candidate (ch)
  (let* ((key-idx (car (rassoc (char-to-string ch)
			       anthy-select-candidate-keybind)))
	 (idx (car (cdr (assq key-idx
			      anthy-enum-candidate-list)))))
    (if idx
	(progn
	  (let ((repl (anthy-send-recv-command
		       (format " SELECT_CANDIDATE %d\n" idx))))
	    (anthy-proc-agent-reply repl))
	  (setq anthy-enum-candidate-p nil)
	  (setq anthy-enum-rcandidate-p nil))
      (message "%s" anthy-candidate-minibuffer))))

(defvar anthy-default-rkmap-keybind
  '(
    ;; q
    (("hiragana" . 113) . "katakana")
    (("katakana" . 113) . "hiragana")
    ;; l
    (("hiragana" . 108) . "alphabet")
    (("katakana" . 108) . "alphabet")
    ;; L
    (("hiragana" . 76) . "walphabet")
    (("katakana" . 76) . "walphabet")
    ;; \C-j
    (("alphabet" . 10) . "hiragana")
    (("walphabet" . 10) . "hiragana")
    ;; \C-q
    (("hiragana" . 17) . "hankaku_kana")
    (("hankaku_kana" . 17) . "hiragana")
    ))


(defvar anthy-rkmap-keybind anthy-default-rkmap-keybind)


(defun anthy-find-rkmap-keybind (ch)
  (let ((res
	 (assoc (cons anthy-current-rkmap ch) anthy-rkmap-keybind)))
    (if (and res (string-equal (cdr res) "hankaku_kana"))
	(if anthy-use-hankaku-kana res nil)
      res)))

(defun anthy-handle-normal-key (chenc)
  (let* ((repl
	  (if chenc (anthy-send-recv-command 
		     (concat chenc "\n"))
	    nil)))
    (if repl
	(anthy-proc-agent-reply repl))))

(defun anthy-handle-enum-candidate-mode (chenc)
  (anthy-handle-normal-key chenc))

;;
(defun anthy-insert (&optional arg)
  "Anthyのキーハンドラ"
  (interactive "*p")
  ;; suzuki : last-command-char を (anthy-last-command-char) に変更
  (let* ((ch (anthy-last-command-char))
	 (chenc (anthy-encode-key ch)))
    (anthy-handle-key ch chenc)))

(defun anthy-handle-key (ch chenc)
  (cond
   ;; 候補選択モードから候補を選ぶ
   ((and (or anthy-enum-candidate-p anthy-enum-rcandidate-p)
	 (integerp ch)
	 (assq (car (rassoc (char-to-string ch)
			    anthy-select-candidate-keybind))
	       anthy-enum-candidate-list))
    (anthy-insert-select-candidate ch))
   ;; キーマップを変更するコマンドを処理する
   ((and (anthy-find-rkmap-keybind ch)
	 (string-equal anthy-preedit ""))
    (let ((mapname (cdr (anthy-find-rkmap-keybind ch))))
      (let ((repl (anthy-send-recv-command
		   (concat " MAP_SELECT " mapname "\n"))))
	(if (eq repl 'OK)
	    (progn
	      (setq anthy-current-rkmap
		    (cdr (assoc (cons anthy-current-rkmap ch)
				anthy-rkmap-keybind)))
	      (anthy-update-mode-line))))))
   ;; アルファベットモードの場合は直接入力
   ((and (string-equal anthy-current-rkmap "alphabet")
	 (string-equal anthy-preedit ""))
    (self-insert-command 1))
   ;; プリエディットがなくてスペースが押された
   ((and
     (string-equal anthy-preedit "")
     (= ch 32)
     (not
      (string-equal anthy-current-rkmap "alphabet")))
    (progn
      (insert-and-inherit anthy-wide-space)
      (anthy-do-auto-fill)))
   ((or anthy-enum-candidate-p anthy-enum-rcandidate-p)
    (anthy-handle-enum-candidate-mode chenc))
   ;; 普通の入力
   (t
    (anthy-handle-normal-key chenc))))

;;
(defun anthy-do-invoke-agent (cmd)
  (if (and (stringp anthy-personality)
	   (> (length anthy-personality) 0))
      (start-process "anthy-agent-unicode"
		     anthy-working-buffer
		     cmd
		     (concat " --personality=" anthy-personality))
    (start-process "anthy-agent-unicode"
		   anthy-working-buffer
		   cmd)))
;;
(defun anthy-invoke-agent ()
  (let ((list anthy-agent-unicode-command-list)
	(proc nil))
    (while (and list (not proc))
      (setq proc 
	    (anthy-do-invoke-agent (car list)))
      (if (not (boundp 'proc))
	  (setq proc nil))
      (setq list (cdr list)))
    proc))
;;
;;
;;
(defun anthy-check-agent ()
  ;; check and do invoke
  (if (not anthy-agent-unicode-process)
      (let
	  ((proc (anthy-invoke-agent)))
	(if anthy-agent-unicode-process
	    (kill-process anthy-agent-unicode-process))
	(setq anthy-agent-unicode-process proc)
	(set-process-query-on-exit-flag proc nil)
	;; utf-8 を扱えない処理系では Mule-UCS が在れば読む。実測で扱えな
	;; かったのは XEmacs 21.4 と Emacs 21.4 と Emacs 20.7 の三つ。
	;; XEmacs 21.5 と Emacs 22 以降は最初から扱えるので何もしない。
	;; 無ければ黙って諦める。
	(if (not (anthy-utf-8-unix-p))
	    (condition-case nil
		(require 'un-define)
	      (error nil)))
	(if (anthy-utf-8-unix-p)
	    (set-process-coding-system proc 'utf-8-unix 'utf-8-unix))
;;	(if anthy-xemacs
;;	    (if (coding-system-p (find-coding-system 'euc-japan))
;;		(set-process-coding-system proc 'euc-japan 'euc-japan))
;;	  (cond ((coding-system-p 'euc-japan)
;;		 (set-process-coding-system proc 'euc-japan 'euc-japan))
;;		((coding-system-p '*euc-japan*)
;;		 (set-process-coding-system proc '*euc-japan* '*euc-japan*))))
	(set-process-sentinel proc 'anthy-process-sentinel))))
;;
(defun anthy-do-send-recv-command (cmd)
  (if (not anthy-agent-unicode-process)
      (anthy-check-agent))
  (let ((old-buffer (current-buffer)))
    (unwind-protect
	(progn
	  (set-buffer anthy-working-buffer)
	  (erase-buffer)
	  (process-send-string anthy-agent-unicode-process cmd)
	  (while (= (buffer-size) 0)
	    (accept-process-output nil 0 anthy-accept-timeout))
	  (read (buffer-string)))
      (set-buffer old-buffer))))
;;
(defun anthy-send-recv-command (cmd)
  (if anthy-context-id
      (anthy-do-send-recv-command
       (concat " SELECT_CONTEXT "
	       (number-to-string anthy-context-id)
	       "\n")))
  (anthy-do-send-recv-command cmd))
;;
(defun anthy-minibuffer-enter ()
  (setq anthy-saved-mode anthy-mode)
  (setq anthy-mode nil)
  (setq anthy-enable-enum-candidate-p 
	(cons nil anthy-enable-enum-candidate-p))
  (anthy-update-mode))
;;
(defun anthy-minibuffer-exit ()
  (setq anthy-mode anthy-saved-mode)
  (setq anthy-enable-enum-candidate-p 
	(cdr anthy-enable-enum-candidate-p))
  (anthy-update-mode))
;;
(defun anthy-kill-buffer ()
  (if anthy-context-id
      (anthy-send-recv-command
       " RELEASE_CONTEXT\n")))
;;
(defun anthy-mode-on ()
  (add-hook 'minibuffer-setup-hook 'anthy-minibuffer-enter)
  (add-hook 'minibuffer-exit-hook 'anthy-minibuffer-exit)
  (add-hook 'kill-buffer-hook 'anthy-kill-buffer)
  (anthy-check-context-id)
  (setq anthy-minor-mode t)
  (anthy-update-mode-line))
;;
(defun anthy-mode-off ()
  (setq anthy-minor-mode nil)
  (anthy-update-mode-line))
;;
(defun anthy-update-mode ()
  (if (or anthy-mode anthy-leim-active-p)
      (progn
	(anthy-check-agent)
	(anthy-mode-on))
    (anthy-mode-off))
  (run-hooks 'anthy-mode-hook))

(defun anthy-mode (&optional arg)
  "Start Anthy conversion system."
  (interactive "P")
  (setq anthy-mode
        (if (null arg)
            (not anthy-mode)
          (> (prefix-numeric-value arg) 0)))
  (anthy-update-mode))

(defun anthy-select-map (map)
  (anthy-send-recv-command (concat " MAP_SELECT " map "\n"))
  (setq anthy-current-rkmap map)
  (anthy-update-mode-line))
;;
(defun anthy-hiragana-map (&optional arg)
  "Hiragana mode"
  (interactive "P")
  (anthy-select-map "hiragana"))
;;
(defun anthy-katakana-map (&optional arg)
  "Katakana mode"
  (interactive "P")
  (anthy-select-map "katakana"))
;;
(defun anthy-alpha-map (arg)
  "Alphabet mode"
  (interactive "P")
  (anthy-select-map "alphabet"))
;;
(defun anthy-wide-alpha-map (arg)
  "Wide Alphabet mode"
  (interactive "P")
  (anthy-select-map "walphabet"))
;;
(defun anthy-hankaku-kana-map (arg)
  "Hankaku Katakana mode"
  (interactive "P")
  (anthy-select-map "hankaku_kana"))
;;
;;
;; leim の inactivate
;;
(defun anthy-unicode-leim-inactivate ()
  (setq anthy-leim-active-p nil)
  (anthy-update-mode))
;;
;; leim の activate
;;
(defun anthy-unicode-leim-activate (&optional name)
  (setq deactivate-current-input-method-function 'anthy-unicode-leim-inactivate)
  (setq anthy-leim-active-p t)
  (anthy-update-mode)
  (when (eq (selected-window) (minibuffer-window))
    (add-hook 'minibuffer-exit-hook 'anthy-unicode-leim-exit-from-minibuffer)))

;;
;; emacsのバグ避けらしいです
;;
(defun anthy-unicode-leim-exit-from-minibuffer ()
  (deactivate-input-method)
  (when (<= (minibuffer-depth) 1)
    (remove-hook 'minibuffer-exit-hook 'anthy-unicode-leim-exit-from-minibuffer)))

;;
;; Emacs / XEmacs コンパチブルな last-command-char
;; suzuki : 新設
;;
(defun anthy-last-command-char ()
  "最後の入力イベントを返す。XEmacs では int に変換する"
  (if anthy-xemacs
      (let ((event last-command-event))
	(cond
	 ((event-matches-key-specifier-p event 'left)      2)
	 ((event-matches-key-specifier-p event 'right)     6)
	 ((event-matches-key-specifier-p event 'backspace) 8)
	 (t
	  (char-to-int (event-to-character event)))))
    last-command-event))

;;
;;
;;
;(global-set-key [(meta escape)] 'anthy-mode)
(provide 'anthy-unicode)

(require 'anthy-unicode-dic)
(require 'anthy-unicode-conf)

;; is it ok for i18n?
(set-language-info "Japanese" 'input-method "japanese-anthy")
(if (equal current-language-environment "Japanese")
    (progn
      (if (boundp 'default-input-method)
	  (setq-default default-input-method "japanese-anthy"))
      (setq default-input-method "japanese-anthy")))

(defun anthy-default-mode ()
  (interactive)
  (setq anthy-rkmap-keybind anthy-default-rkmap-keybind)
  (anthy-send-recv-command " MAP_CLEAR 1\n")
  (anthy-send-recv-command " SET_PREEDIT_MODE 0\n")
  (anthy-hiragana-map))

(defun anthy-insert-and-convert (ch)
  (interactive "P")
  (if (< 0 (length anthy-preedit))
      (progn
        (anthy-insert ch)
        (anthy-handle-normal-key "(space)"))
    (anthy-insert ch)))

;;;
;;; anthy.el ends here
