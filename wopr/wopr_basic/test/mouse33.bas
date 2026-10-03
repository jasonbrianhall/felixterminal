' MOUSE33.BAS - the mouse the QuickBASIC 4.5 way: the DOS mouse driver,
' INT 33h, through CALL INTERRUPT. Click a color to paint the screen
' with it; Esc quits.
TYPE RegType
    ax AS INTEGER
    bx AS INTEGER
    cx AS INTEGER
    dx AS INTEGER
    bp AS INTEGER
    si AS INTEGER
    di AS INTEGER
    flags AS INTEGER
END TYPE
DIM regs AS RegType

regs.ax = 0                                  ' reset: is there a mouse?
CALL INTERRUPT(&H33, regs, regs)
IF regs.ax = 0 THEN PRINT "No mouse driver.": END
nb = regs.bx

DIM nm$(7)
FOR i = 0 TO 7: READ nm$(i): NEXT
DATA Black,Blue,Green,Cyan,Red,Magenta,Brown,White
bg = 1
GOSUB Draw
regs.ax = 1                                  ' show the pointer
CALL INTERRUPT(&H33, regs, regs)

DO
    regs.ax = 3                              ' where is it, which buttons?
    CALL INTERRUPT(&H33, regs, regs)
    col = regs.cx \ 8 + 1: row = regs.dx \ 8 + 1
    LOCATE 22, 3: COLOR 15, bg
    PRINT "Row"; row; " Column"; col; " Buttons"; regs.bx; "  ";

    regs.ax = 5: regs.bx = 0                 ' left-button presses since last time
    CALL INTERRUPT(&H33, regs, regs)
    IF regs.bx > 0 THEN
        r = regs.dx \ 8 + 1: c = regs.cx \ 8 + 1
        IF r >= 8 AND r <= 15 AND c >= 30 AND c <= 49 THEN
            bg = r - 8: GOSUB Draw
        END IF
    END IF
LOOP UNTIL INKEY$ = CHR$(27)

regs.ax = 2                                  ' hide it again
CALL INTERRUPT(&H33, regs, regs)
COLOR 7, 0: CLS
END

Draw:
    regs.ax = 2: CALL INTERRUPT(&H33, regs, regs)   ' hide while drawing
    COLOR 15, bg: CLS
    LOCATE 3, 3: PRINT "INT 33h mouse driver found,"; nb; "buttons."
    LOCATE 5, 3: PRINT "Click a color:"
    FOR i = 0 TO 7
        fg = 0: IF i = 0 THEN fg = 15
        LOCATE 8 + i, 30: COLOR fg, i
        PRINT " "; LEFT$(nm$(i) + SPACE$(18), 18); " ";
    NEXT
    COLOR 15, bg
    LOCATE 24, 3: PRINT "Esc quits";
    regs.ax = 1: CALL INTERRUPT(&H33, regs, regs)
    RETURN
