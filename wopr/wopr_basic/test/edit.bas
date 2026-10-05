' EDIT.BAS - Felix Edit, a full-screen editor for free-form BASIC files.
'
' Keyboard
'   Arrows, Home, End, PgUp, PgDn    move     Ins        insert/overwrite
'   Enter splits a line (keeps indent)        Tab        indent to next 4
'   Backspace / Del delete (or the selection)
'   F1 Help  F2 Save  F3 Find next  F4 Find  F5 Copy  F6 Cut  F7 Paste
'   F8 Go to line  F9 Open  F10 Save as  Esc clear selection / quit
'   Copy or Cut with nothing selected takes the whole current line.
'
' Mouse
'   Click to place the cursor, drag to select, double-click selects a word.
'   Wheel scrolls; the bar on the right scrolls, pages and can be dragged.
'   The menu on the top line does the same as the function keys.

CONST MAXL = 8000
CONST VROWS = 23
CONST VCOLS = 79

DIM L$(MAXL)
DIM CB$(MAXL)
DIM KQ$(2000)
DIM MX1(12), MX2(12), ML$(12)

SCREEN 0
WIDTH 80, 25
_TITLE "Felix Edit"
COLOR 7, 1
CLS

' ---- menu bar: label and the columns it covers -------------------------
FOR i = 1 TO 11
    READ ML$(i)
NEXT
DATA "Open","Save","Save As","Find","Next","Go To","Copy","Cut","Paste","Help","Quit"
menu$ = " "
FOR i = 1 TO 11
    MX1(i) = LEN(menu$) + 1
    menu$ = menu$ + ML$(i)
    MX2(i) = LEN(menu$)
    menu$ = menu$ + "  "
NEXT
menu$ = LEFT$(menu$ + SPACE$(80), 80 - 11) + " Felix Edit"

' ---- editor state ---------------------------------------------------------
n = 1: L$(1) = ""
cy = 1: cx = 1: wantx = 1
topl = 1: lcol = 1
insmode = 1: modified = 0: crlf = 0
fname$ = "": msg$ = "": find$ = ""
mark = 0: ay = 1: ax = 1
cbn = 0: cbline = 0
lastmb = 0: dragging = 0: sbdrag = 0
lastclick = 0: lastclx = 0: lastcly = 0
quitnow = 0: dirty = 1

_MOUSESHOW
GOSUB Render

pr$ = "File to edit (Enter for a new file):": in$ = ""
GOSUB GetInput
IF ok AND in$ <> "" THEN
    fname$ = in$
    GOSUB LoadFile
END IF
dirty = 1

' ---- main loop -------------------------------------------------------------
DO
    GOSUB PollMouse
    GOSUB PollKeys
    IF dirty THEN
        GOSUB Render
        dirty = 0
    END IF
    IF quitnow THEN EXIT DO
    _LIMIT 100
LOOP

_MOUSEHIDE
COLOR 7, 0
CLS
END

' ===========================================================================
' Keyboard
' ===========================================================================
PollKeys:
    kq = 0
    DO
        k$ = INKEY$
        IF k$ = "" THEN EXIT DO
        kq = kq + 1
        KQ$(kq) = k$
        IF kq >= 2000 THEN EXIT DO
    LOOP
    IF kq = 0 THEN RETURN
    msgkeep = 0
    ' Several keys at once means a Ctrl+V paste: don't auto-indent it.
    pasting = 0
    IF kq > 1 THEN pasting = 1
    FOR qi = 1 TO kq
        k$ = KQ$(qi)
        GOSUB HandleKey
        IF quitnow THEN EXIT FOR
    NEXT
    msgkeep = 0
    dirty = 1
    RETURN

HandleKey:
    IF msgkeep = 0 THEN msg$ = ""
    IF LEN(k$) = 2 THEN
        sc = ASC(MID$(k$, 2, 1))
        GOSUB ExtKey
        RETURN
    END IF
    kc = ASC(k$)
    IF kc = 27 THEN
        IF mark THEN
            mark = 0
        ELSE
            act = 11: GOSUB DoAction
        END IF
    ELSEIF kc = 13 OR kc = 10 THEN
        GOSUB KeyEnter
    ELSEIF kc = 8 OR kc = 127 THEN
        GOSUB KeyBackspace
    ELSEIF kc = 9 THEN
        sp = 4 - ((cx - 1) MOD 4)
        c$ = SPACE$(sp): GOSUB InsertText
    ELSEIF kc >= 32 AND kc <> 127 THEN
        c$ = k$: GOSUB InsertText
    END IF
    RETURN

ExtKey:
    IF sc = 72 THEN
        mark = 0
        IF cy > 1 THEN cy = cy - 1
        GOSUB VertCol
    ELSEIF sc = 80 THEN
        mark = 0
        IF cy < n THEN cy = cy + 1
        GOSUB VertCol
    ELSEIF sc = 75 THEN
        mark = 0
        IF cx > 1 THEN
            cx = cx - 1
        ELSEIF cy > 1 THEN
            cy = cy - 1: cx = LEN(L$(cy)) + 1
        END IF
        wantx = cx
    ELSEIF sc = 77 THEN
        mark = 0
        IF cx <= LEN(L$(cy)) THEN
            cx = cx + 1
        ELSEIF cy < n THEN
            cy = cy + 1: cx = 1
        END IF
        wantx = cx
    ELSEIF sc = 71 THEN
        mark = 0
        ' Home: first non-blank, then column 1
        t$ = L$(cy)
        fb = LEN(t$) - LEN(LTRIM$(t$)) + 1
        IF cx = fb THEN cx = 1 ELSE cx = fb
        wantx = cx
    ELSEIF sc = 79 THEN
        mark = 0
        cx = LEN(L$(cy)) + 1: wantx = cx
    ELSEIF sc = 73 THEN
        mark = 0
        cy = cy - (VROWS - 1): topl = topl - (VROWS - 1)
        IF cy < 1 THEN cy = 1
        IF topl < 1 THEN topl = 1
        GOSUB VertCol
    ELSEIF sc = 81 THEN
        mark = 0
        cy = cy + (VROWS - 1): topl = topl + (VROWS - 1)
        IF cy > n THEN cy = n
        GOSUB ClampTop
        GOSUB VertCol
    ELSEIF sc = 82 THEN
        insmode = 1 - insmode
    ELSEIF sc = 83 THEN
        GOSUB KeyDelete
    ELSEIF sc = 59 THEN
        act = 10: GOSUB DoAction
    ELSEIF sc = 60 THEN
        act = 2: GOSUB DoAction
    ELSEIF sc = 61 THEN
        act = 5: GOSUB DoAction
    ELSEIF sc = 62 THEN
        act = 4: GOSUB DoAction
    ELSEIF sc = 63 THEN
        act = 7: GOSUB DoAction
    ELSEIF sc = 64 THEN
        act = 8: GOSUB DoAction
    ELSEIF sc = 65 THEN
        act = 9: GOSUB DoAction
    ELSEIF sc = 66 THEN
        act = 6: GOSUB DoAction
    ELSEIF sc = 67 THEN
        act = 1: GOSUB DoAction
    ELSEIF sc = 68 THEN
        act = 3: GOSUB DoAction
    END IF
    GOSUB EnsureVisible
    RETURN

' Up/Down keep the column you were in, as far as the new line allows.
VertCol:
    cx = wantx
    IF cx > LEN(L$(cy)) + 1 THEN cx = LEN(L$(cy)) + 1
    RETURN

' ===========================================================================
' Editing
' ===========================================================================
InsertText:
    GOSUB HasSel
    IF hs THEN GOSUB DeleteSel
    mark = 0
    t$ = L$(cy)
    IF insmode OR cx > LEN(t$) THEN
        L$(cy) = LEFT$(t$, cx - 1) + c$ + MID$(t$, cx)
    ELSE
        L$(cy) = LEFT$(t$, cx - 1) + c$ + MID$(t$, cx + LEN(c$))
    END IF
    cx = cx + LEN(c$): wantx = cx
    modified = 1
    GOSUB EnsureVisible
    RETURN

KeyEnter:
    GOSUB HasSel
    IF hs THEN GOSUB DeleteSel
    mark = 0
    IF n >= MAXL THEN
        msg$ = "File too long (" + LTRIM$(STR$(MAXL)) + " lines max)": msgkeep = 1
        RETURN
    END IF
    t$ = L$(cy)
    head$ = LEFT$(t$, cx - 1)
    tail$ = MID$(t$, cx)
    ind = 0
    IF pasting = 0 THEN
        ind = LEN(t$) - LEN(LTRIM$(t$))
        IF ind > cx - 1 THEN ind = cx - 1
        IF ind > 0 THEN tail$ = LTRIM$(tail$)
    END IF
    L$(cy) = head$
    gi = cy + 1: gc = 1: GOSUB OpenGap
    L$(cy + 1) = SPACE$(ind) + tail$
    cy = cy + 1: cx = ind + 1: wantx = cx
    modified = 1
    GOSUB EnsureVisible
    RETURN

KeyBackspace:
    GOSUB HasSel
    IF hs THEN
        GOSUB DeleteSel
    ELSEIF cx > 1 THEN
        t$ = L$(cy)
        L$(cy) = LEFT$(t$, cx - 2) + MID$(t$, cx)
        cx = cx - 1
        modified = 1
    ELSEIF cy > 1 THEN
        cx = LEN(L$(cy - 1)) + 1
        L$(cy - 1) = L$(cy - 1) + L$(cy)
        gi = cy: gc = 1: GOSUB CloseGap
        cy = cy - 1
        modified = 1
    END IF
    mark = 0: wantx = cx
    GOSUB EnsureVisible
    RETURN

KeyDelete:
    GOSUB HasSel
    IF hs THEN
        GOSUB DeleteSel
    ELSEIF cx <= LEN(L$(cy)) THEN
        t$ = L$(cy)
        L$(cy) = LEFT$(t$, cx - 1) + MID$(t$, cx + 1)
        modified = 1
    ELSEIF cy < n THEN
        L$(cy) = L$(cy) + L$(cy + 1)
        gi = cy + 1: gc = 1: GOSUB CloseGap
        modified = 1
    END IF
    mark = 0: wantx = cx
    RETURN

' Make room for gc lines at gi (lines from gi down move gc further on).
OpenGap:
    FOR j = n TO gi STEP -1
        L$(j + gc) = L$(j)
    NEXT
    FOR j = gi TO gi + gc - 1
        L$(j) = ""
    NEXT
    n = n + gc
    RETURN

' Remove gc lines starting at gi.
CloseGap:
    FOR j = gi TO n - gc
        L$(j) = L$(j + gc)
    NEXT
    FOR j = n - gc + 1 TO n
        L$(j) = ""
    NEXT
    n = n - gc
    IF n < 1 THEN n = 1: L$(1) = ""
    RETURN

' ===========================================================================
' Selection: from the anchor (ay, ax) to the cursor (cy, cx).
' NormSel gives it in order as (sy1, sx1)-(sy2, sx2), sx2 exclusive.
' ===========================================================================
HasSel:
    hs = 0
    IF mark THEN
        IF ay <> cy OR ax <> cx THEN hs = 1
    END IF
    IF hs THEN GOSUB NormSel
    RETURN

NormSel:
    IF ay < cy OR (ay = cy AND ax < cx) THEN
        sy1 = ay: sx1 = ax: sy2 = cy: sx2 = cx
    ELSE
        sy1 = cy: sx1 = cx: sy2 = ay: sx2 = ax
    END IF
    RETURN

DeleteSel:
    GOSUB NormSel
    IF sy1 = sy2 THEN
        t$ = L$(sy1)
        L$(sy1) = LEFT$(t$, sx1 - 1) + MID$(t$, sx2)
    ELSE
        L$(sy1) = LEFT$(L$(sy1), sx1 - 1) + MID$(L$(sy2), sx2)
        gi = sy1 + 1: gc = sy2 - sy1: GOSUB CloseGap
    END IF
    cy = sy1: cx = sx1: wantx = cx
    mark = 0: modified = 1
    RETURN

CopySel:
    GOSUB HasSel
    IF hs = 0 THEN
        CB$(1) = L$(cy): cbn = 1: cbline = 1
        RETURN
    END IF
    cbline = 0
    IF sy1 = sy2 THEN
        CB$(1) = MID$(L$(sy1), sx1, sx2 - sx1): cbn = 1
    ELSE
        cbn = sy2 - sy1 + 1
        CB$(1) = MID$(L$(sy1), sx1)
        FOR j = 2 TO cbn - 1
            CB$(j) = L$(sy1 + j - 1)
        NEXT
        CB$(cbn) = LEFT$(L$(sy2), sx2 - 1)
    END IF
    RETURN

Paste:
    IF cbn = 0 THEN
        msg$ = "Nothing to paste": msgkeep = 1
        RETURN
    END IF
    GOSUB HasSel
    IF hs THEN GOSUB DeleteSel
    mark = 0
    IF cbline THEN
        ' a whole line goes in above the current one
        IF n >= MAXL THEN RETURN
        gi = cy: gc = 1: GOSUB OpenGap
        L$(cy) = CB$(1)
        cy = cy + 1
    ELSEIF cbn = 1 THEN
        t$ = L$(cy)
        L$(cy) = LEFT$(t$, cx - 1) + CB$(1) + MID$(t$, cx)
        cx = cx + LEN(CB$(1))
    ELSE
        IF n + cbn - 1 > MAXL THEN
            msg$ = "Too many lines to paste": msgkeep = 1
            RETURN
        END IF
        t$ = L$(cy)
        head$ = LEFT$(t$, cx - 1): tail$ = MID$(t$, cx)
        gi = cy + 1: gc = cbn - 1: GOSUB OpenGap
        L$(cy) = head$ + CB$(1)
        FOR j = 2 TO cbn - 1
            L$(cy + j - 1) = CB$(j)
        NEXT
        L$(cy + cbn - 1) = CB$(cbn) + tail$
        cy = cy + cbn - 1
        cx = LEN(CB$(cbn)) + 1
    END IF
    wantx = cx: modified = 1
    GOSUB EnsureVisible
    RETURN

' ===========================================================================
' Actions (menu bar and function keys)
'   1 Open  2 Save  3 Save As  4 Find  5 Next  6 Go To
'   7 Copy  8 Cut  9 Paste  10 Help  11 Quit
' ===========================================================================
DoAction:
    IF act = 1 THEN
        GOSUB AskSave
        IF cancel = 0 THEN
            pr$ = "Open file:": in$ = ""
            GOSUB GetInput
            IF ok AND in$ <> "" THEN
                fname$ = in$
                GOSUB LoadFile
            END IF
        END IF
    ELSEIF act = 2 THEN
        GOSUB DoSave
    ELSEIF act = 3 THEN
        GOSUB DoSaveAs
    ELSEIF act = 4 THEN
        pr$ = "Find:": in$ = find$
        GOSUB GetInput
        IF ok AND in$ <> "" THEN
            find$ = in$
            GOSUB FindNext
        END IF
    ELSEIF act = 5 THEN
        IF find$ = "" THEN
            act = 4: GOSUB DoAction
        ELSE
            GOSUB FindNext
        END IF
    ELSEIF act = 6 THEN
        pr$ = "Go to line (1-" + LTRIM$(STR$(n)) + "):": in$ = ""
        GOSUB GetInput
        IF ok AND VAL(in$) > 0 THEN
            cy = INT(VAL(in$))
            IF cy > n THEN cy = n
            cx = 1: wantx = 1: mark = 0
            topl = cy - VROWS \ 2
            GOSUB ClampTop
        END IF
    ELSEIF act = 7 THEN
        GOSUB CopySel
        IF cbline THEN msg$ = "Line copied" ELSE msg$ = "Copied"
        msgkeep = 1
    ELSEIF act = 8 THEN
        GOSUB CopySel
        IF cbline THEN
            gi = cy: gc = 1: GOSUB CloseGap
            IF cy > n THEN cy = n
            cx = 1: wantx = 1: modified = 1
            msg$ = "Line cut"
        ELSE
            GOSUB DeleteSel
            msg$ = "Cut"
        END IF
        msgkeep = 1
    ELSEIF act = 9 THEN
        GOSUB Paste
    ELSEIF act = 10 THEN
        GOSUB ShowHelp
    ELSEIF act = 11 THEN
        GOSUB AskSave
        IF cancel = 0 THEN quitnow = 1
    END IF
    GOSUB EnsureVisible
    dirty = 1
    RETURN

FindNext:
    uf$ = UCASE$(find$)
    fy = cy: fx = cx + 1
    GOSUB HasSel
    IF hs = 0 THEN fx = cx
    found = 0
    FOR pass = 0 TO n
        i = fy + pass
        IF i > n THEN i = i - n
        IF pass = 0 THEN st = fx ELSE st = 1
        IF pass = n THEN
            ' back on the starting line: look before where we began
            p = INSTR(1, UCASE$(L$(i)), uf$)
            IF p >= fx THEN p = 0
        ELSEIF st <= LEN(L$(i)) THEN
            p = INSTR(st, UCASE$(L$(i)), uf$)
        ELSE
            p = 0
        END IF
        IF p > 0 THEN
            found = 1
            EXIT FOR
        END IF
    NEXT
    IF found THEN
        mark = 1: ay = i: ax = p
        cy = i: cx = p + LEN(find$): wantx = cx
        topl = cy - VROWS \ 2
        GOSUB ClampTop
        IF i < fy OR (i = fy AND p < fx) THEN msg$ = "Search wrapped": msgkeep = 1
    ELSE
        msg$ = "Not found: " + find$: msgkeep = 1
    END IF
    RETURN

' ===========================================================================
' Files
' ===========================================================================
LoadFile:
    loadfail = 0
    ON ERROR GOTO LoadErr
    OPEN fname$ FOR INPUT AS #1
    ON ERROR GOTO 0
    oldn = n
    IF loadfail THEN
        n = 1: L$(1) = ""
        crlf = 0
        msg$ = "New file: " + fname$
    ELSE
        n = 0: crlf = 0: trunc = 0
        DO WHILE NOT EOF(1)
            LINE INPUT #1, a$
            IF n >= MAXL THEN
                trunc = 1
                EXIT DO
            END IF
            IF RIGHT$(a$, 1) = CHR$(13) THEN
                crlf = 1
                a$ = LEFT$(a$, LEN(a$) - 1)
            END IF
            n = n + 1
            L$(n) = a$
        LOOP
        CLOSE #1
        IF n = 0 THEN n = 1: L$(1) = ""
        msg$ = "Loaded " + fname$ + " (" + LTRIM$(STR$(n)) + " lines)"
        IF trunc THEN msg$ = "Only the first " + LTRIM$(STR$(MAXL)) + " lines loaded!"
    END IF
    FOR j = n + 1 TO oldn
        L$(j) = ""
    NEXT
    msgkeep = 1
    cy = 1: cx = 1: wantx = 1: topl = 1: lcol = 1
    mark = 0: modified = 0
    dirty = 1
    RETURN

LoadErr:
    loadfail = 1
    RESUME NEXT

DoSave:
    IF fname$ = "" THEN
        GOSUB DoSaveAs
    ELSE
        GOSUB WriteFile
    END IF
    RETURN

DoSaveAs:
    pr$ = "Save as:": in$ = fname$
    GOSUB GetInput
    IF ok AND in$ <> "" THEN
        fname$ = in$
        GOSUB WriteFile
    ELSE
        saveok = 0
    END IF
    RETURN

WriteFile:
    saveok = 0: savefail = 0
    ON ERROR GOTO SaveErr
    OPEN fname$ FOR OUTPUT AS #2
    ON ERROR GOTO 0
    IF savefail THEN
        msg$ = "Can't write " + fname$: msgkeep = 1
        RETURN
    END IF
    FOR j = 1 TO n
        IF crlf THEN
            PRINT #2, L$(j); CHR$(13)
        ELSE
            PRINT #2, L$(j)
        END IF
    NEXT
    CLOSE #2
    modified = 0: saveok = 1
    msg$ = "Saved " + fname$ + " (" + LTRIM$(STR$(n)) + " lines)": msgkeep = 1
    RETURN

SaveErr:
    savefail = 1
    RESUME NEXT

' Unsaved changes? Ask first. cancel = 1 means stay put.
AskSave:
    cancel = 0
    IF modified = 0 THEN RETURN
    GOSUB Render
    LOCATE 25, 1: COLOR 15, 4
    PRINT LEFT$(" Save changes to " + fname$ + "?  (Y)es  (N)o  (Esc) cancel" + SPACE$(79), 79);
    DO
        k$ = UCASE$(INKEY$)
        DO WHILE _MOUSEINPUT
        LOOP
        _LIMIT 100
    LOOP UNTIL k$ = "Y" OR k$ = "N" OR k$ = CHR$(27)
    IF k$ = CHR$(27) THEN
        cancel = 1
    ELSEIF k$ = "Y" THEN
        GOSUB DoSave
        IF saveok = 0 THEN cancel = 1
    END IF
    dirty = 1
    RETURN

' ===========================================================================
' A one-line prompt on the bottom row: pr$ and the starting text in$.
' Returns in$ and ok (1 Enter, 0 Esc).
' ===========================================================================
GetInput:
    ok = 0
    DO
        LOCATE 25, 1: COLOR 15, 4
        s$ = " " + pr$ + " " + in$
        IF LEN(s$) > 78 THEN s$ = " " + pr$ + " ..." + RIGHT$(in$, 78 - LEN(pr$) - 5)
        PRINT LEFT$(s$ + SPACE$(79), 79);
        LOCATE 25, LEN(s$) + 1, 1
        DO
            k$ = INKEY$
            DO WHILE _MOUSEINPUT
            LOOP
            IF k$ = "" THEN _LIMIT 100
        LOOP WHILE k$ = ""
        IF k$ = CHR$(13) OR k$ = CHR$(10) THEN
            ok = 1
            EXIT DO
        ELSEIF k$ = CHR$(27) THEN
            EXIT DO
        ELSEIF k$ = CHR$(8) OR k$ = CHR$(127) THEN
            IF LEN(in$) > 0 THEN in$ = LEFT$(in$, LEN(in$) - 1)
        ELSEIF LEN(k$) = 1 THEN
            IF ASC(k$) >= 32 AND ASC(k$) <> 127 THEN in$ = in$ + k$
        END IF
    LOOP
    dirty = 1
    RETURN

ShowHelp:
    COLOR 0, 7
    LOCATE 3, 8: PRINT CHR$(218); STRING$(62, 196); CHR$(191);
    FOR r = 4 TO 21
        LOCATE r, 8: PRINT CHR$(179); SPACE$(62); CHR$(179);
    NEXT
    LOCATE 22, 8: PRINT CHR$(192); STRING$(62, 196); CHR$(217);
    LOCATE 4, 30: PRINT " Felix Edit Help ";
    LOCATE 6, 11: PRINT "Arrows Home End PgUp PgDn   move the cursor";
    LOCATE 7, 11: PRINT "Ins                         insert / overwrite";
    LOCATE 8, 11: PRINT "Enter  Tab  Backspace  Del  as usual";
    LOCATE 10, 11: PRINT "F1 Help       F2 Save       F10 Save as";
    LOCATE 11, 11: PRINT "F9 Open       F4 Find       F3 Find next";
    LOCATE 12, 11: PRINT "F5 Copy       F6 Cut        F7 Paste";
    LOCATE 13, 11: PRINT "F8 Go to line               Esc  quit";
    LOCATE 14, 11: PRINT "Copy/Cut with no selection take the whole line.";
    LOCATE 16, 11: PRINT "Mouse: click to place the cursor, drag to select,";
    LOCATE 17, 11: PRINT "double-click for a word, wheel or right bar to scroll.";
    LOCATE 18, 11: PRINT "Ctrl+V pastes text from other programs.";
    LOCATE 20, 11: PRINT "Press any key or click to close.";
    LOCATE 1, 1, 0
    DO
        k$ = INKEY$
        clicked = 0
        DO WHILE _MOUSEINPUT
            IF _MOUSEBUTTON(1) THEN clicked = 1
        LOOP
        _LIMIT 100
    LOOP WHILE k$ = "" AND clicked = 0
    lastmb = -1
    dirty = 1
    RETURN

' ===========================================================================
' Mouse
' ===========================================================================
PollMouse:
    DO WHILE _MOUSEINPUT
        mx = _MOUSEX: my = _MOUSEY
        mb = _MOUSEBUTTON(1)
        mw = _MOUSEWHEEL
        IF mw <> 0 THEN
            topl = topl + mw * 3
            GOSUB ClampTop
            IF dragging THEN GOSUB MouseDrag
            dirty = 1
        END IF
        IF mb AND lastmb = 0 THEN
            GOSUB MousePress
        ELSEIF mb AND lastmb THEN
            IF dragging THEN GOSUB MouseDrag
            IF sbdrag THEN GOSUB ScrollDrag
        END IF
        IF mb = 0 THEN
            dragging = 0: sbdrag = 0
        END IF
        lastmb = mb
    LOOP
    ' Holding the button above or below the text scrolls the selection on.
    IF dragging AND (my < 2 OR my > 24) THEN
        IF TIMER - lastauto > .05 THEN
            lastauto = TIMER
            IF my < 2 THEN topl = topl - 1 ELSE topl = topl + 1
            GOSUB ClampTop
            GOSUB MouseDrag
        END IF
    END IF
    RETURN

MousePress:
    msg$ = ""
    IF my = 1 THEN
        FOR b = 1 TO 11
            IF mx >= MX1(b) AND mx <= MX2(b) THEN
                act = b: GOSUB DoAction
                EXIT FOR
            END IF
        NEXT
    ELSEIF my = 25 THEN
        IF mx <= 10 THEN
            act = 10: GOSUB DoAction
        END IF
    ELSEIF mx = 80 THEN
        IF my = 2 THEN
            topl = topl - 1
        ELSEIF my = 24 THEN
            topl = topl + 1
        ELSE
            sbdrag = 1
            GOSUB ScrollDrag
        END IF
        GOSUB ClampTop
    ELSE
        GOSUB MouseToText
        IF TIMER - lastclick < .4 AND ty = lastcly AND tx = lastclx THEN
            GOSUB SelectWord
            lastclick = 0
        ELSE
            cy = ty: cx = tx: wantx = cx
            mark = 1: ay = cy: ax = cx
            dragging = 1
            lastclick = TIMER: lastclx = tx: lastcly = ty
        END IF
    END IF
    dirty = 1
    RETURN

MouseDrag:
    GOSUB MouseToText
    cy = ty: cx = tx: wantx = cx
    GOSUB EnsureVisible
    dirty = 1
    RETURN

' Thumb follows the mouse along the bar (rows 3-23).
ScrollDrag:
    maxtop = n - VROWS + 1
    IF maxtop < 1 THEN maxtop = 1
    rr = my
    IF rr < 3 THEN rr = 3
    IF rr > 23 THEN rr = 23
    topl = 1 + INT((rr - 3) * (maxtop - 1) / 20 + .5)
    GOSUB ClampTop
    dirty = 1
    RETURN

' Mouse position -> text position (ty, tx), kept inside the text.
MouseToText:
    rr = my
    IF rr < 2 THEN rr = 2
    IF rr > 24 THEN rr = 24
    ty = topl + rr - 2
    IF ty > n THEN ty = n
    cc = mx
    IF cc > VCOLS THEN cc = VCOLS
    tx = lcol + cc - 1
    IF tx > LEN(L$(ty)) + 1 THEN tx = LEN(L$(ty)) + 1
    IF tx < 1 THEN tx = 1
    RETURN

SelectWord:
    t$ = L$(ty)
    wc$ = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$%!#&"
    IF tx > LEN(t$) THEN RETURN
    IF INSTR(wc$, UCASE$(MID$(t$, tx, 1))) = 0 THEN RETURN
    a = tx
    DO WHILE a > 1
        IF INSTR(wc$, UCASE$(MID$(t$, a - 1, 1))) = 0 THEN EXIT DO
        a = a - 1
    LOOP
    b = tx
    DO WHILE b <= LEN(t$)
        IF INSTR(wc$, UCASE$(MID$(t$, b, 1))) = 0 THEN EXIT DO
        b = b + 1
    LOOP
    mark = 1: ay = ty: ax = a
    cy = ty: cx = b: wantx = cx
    dragging = 0
    RETURN

' ===========================================================================
' View
' ===========================================================================
ClampTop:
    IF topl > n - VROWS + 1 THEN topl = n - VROWS + 1
    IF topl < 1 THEN topl = 1
    RETURN

EnsureVisible:
    IF cy < topl THEN topl = cy
    IF cy > topl + VROWS - 1 THEN topl = cy - VROWS + 1
    IF cx < lcol THEN lcol = cx
    IF cx > lcol + VCOLS - 1 THEN lcol = cx - VCOLS + 1
    IF lcol < 1 THEN lcol = 1
    GOSUB ClampTop
    RETURN

' Where a comment starts in t$ (cpos, 0 if none): an apostrophe outside a
' string, or a line that is a REM.
FindComment:
    cpos = 0
    ut$ = LTRIM$(t$)
    IF UCASE$(LEFT$(ut$, 3)) = "REM" THEN
        IF LEN(ut$) = 3 OR MID$(ut$, 4, 1) = " " THEN
            cpos = LEN(t$) - LEN(ut$) + 1
            RETURN
        END IF
    END IF
    p = 1
    DO
        q1 = INSTR(p, t$, "'")
        IF q1 = 0 THEN EXIT DO
        q2 = INSTR(p, t$, CHR$(34))
        IF q2 = 0 OR q2 > q1 THEN
            cpos = q1
            EXIT DO
        END IF
        q3 = INSTR(q2 + 1, t$, CHR$(34))
        IF q3 = 0 THEN EXIT DO
        p = q3 + 1
    LOOP
    RETURN

Render:
    LOCATE 1, 1, 0: COLOR 0, 7: PRINT menu$;
    GOSUB HasSel
    ' scroll bar thumb
    maxtop = n - VROWS + 1
    IF maxtop <= 1 THEN
        thumb = 3
    ELSE
        thumb = 3 + INT((topl - 1) * 20 / (maxtop - 1) + .5)
    END IF
    FOR r = 2 TO 24
        i = topl + r - 2
        LOCATE r, 1
        IF i > n THEN
            COLOR 7, 1: PRINT SPACE$(VCOLS);
        ELSE
            t$ = L$(i)
            GOSUB FindComment
            v$ = LEFT$(MID$(t$, lcol) + SPACE$(VCOLS), VCOLS)
            IF cpos = 0 THEN
                COLOR 7, 1: PRINT v$;
            ELSE
                cc = cpos - lcol + 1
                IF cc <= 1 THEN
                    COLOR 11, 1: PRINT v$;
                ELSEIF cc > VCOLS THEN
                    COLOR 7, 1: PRINT v$;
                ELSE
                    COLOR 7, 1: PRINT LEFT$(v$, cc - 1);
                    COLOR 11, 1: PRINT MID$(v$, cc);
                END IF
            END IF
            IF hs THEN
                IF i >= sy1 AND i <= sy2 THEN
                    IF i = sy1 THEN a = sx1 ELSE a = 1
                    IF i = sy2 THEN b = sx2 - 1 ELSE b = LEN(t$) + 1
                    sa = a - lcol + 1: sb = b - lcol + 1
                    IF sa < 1 THEN sa = 1
                    IF sb > VCOLS THEN sb = VCOLS
                    IF sb >= sa THEN
                        LOCATE r, sa: COLOR 1, 7
                        PRINT MID$(v$, sa, sb - sa + 1);
                    END IF
                END IF
            END IF
        END IF
        LOCATE r, 80: COLOR 0, 7
        IF r = 2 THEN
            PRINT CHR$(24);
        ELSEIF r = 24 THEN
            PRINT CHR$(25);
        ELSEIF r = thumb THEN
            PRINT CHR$(219);
        ELSE
            PRINT CHR$(176);
        END IF
    NEXT
    ' status line
    IF msg$ <> "" THEN
        s$ = msg$
    ELSEIF fname$ = "" THEN
        s$ = "(untitled)"
    ELSE
        s$ = fname$
    END IF
    IF modified AND msg$ = "" THEN s$ = s$ + " *"
    IF insmode THEN m$ = "INS" ELSE m$ = "OVR"
    rt$ = "Ln " + LTRIM$(STR$(cy)) + "/" + LTRIM$(STR$(n)) + "  Col " + LTRIM$(STR$(cx)) + "  " + m$ + " "
    s$ = LEFT$(" F1=Help  " + s$ + SPACE$(79), 77 - LEN(rt$)) + "  " + rt$
    LOCATE 25, 1: COLOR 15, 3: PRINT s$;
    COLOR 7, 1
    LOCATE cy - topl + 2, cx - lcol + 1, 1
    RETURN
