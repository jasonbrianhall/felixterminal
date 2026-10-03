' MOUSE.BAS - draw with the mouse, using QB64's _MOUSE functions.
' Left button draws, right button picks the next color, the wheel
' changes the brush size, C clears the screen, Esc quits.
SCREEN 12: CLS
c = 14: sz = 2: lastr = 0: drawing = 0
GOSUB Status
_MOUSESHOW
DO
    DO WHILE _MOUSEINPUT
        x = _MOUSEX: y = _MOUSEY
        w = _MOUSEWHEEL
        IF w <> 0 THEN
            sz = sz - w
            IF sz < 1 THEN sz = 1
            IF sz > 20 THEN sz = 20
            GOSUB Status
        END IF
        IF _MOUSEBUTTON(1) AND y > 20 THEN
            IF drawing THEN
                FOR i = -sz TO sz
                    LINE (lx + i, ly)-(x + i, y), c
                    LINE (lx, ly + i)-(x, y + i), c
                NEXT
            ELSE
                LINE (x - sz, y - sz)-(x + sz, y + sz), c, BF
            END IF
            drawing = -1: lx = x: ly = y
        ELSE
            drawing = 0
        END IF
        r = _MOUSEBUTTON(2)
        IF r AND NOT lastr THEN c = c MOD 15 + 1: GOSUB Status
        lastr = r
    LOOP
    k$ = INKEY$
    IF UCASE$(k$) = "C" THEN CLS: GOSUB Status
LOOP UNTIL k$ = CHR$(27)
SCREEN 0
END

Status:
    LINE (0, 0)-(639, 18), 0, BF
    LOCATE 1, 1
    PRINT "Left: draw  Right: color  Wheel: size"; sz; " C: clear  Esc: quit";
    LINE (610, 2)-(636, 15), c, BF
    RETURN
