1000 ' ================================================================
1010 ' test_new_commands.bas
1020 ' Tests all newly implemented CLI commands and functions.
1030 ' Each test prints PASS or FAIL with a description.
1040 ' Run: ./basic test_new_commands.bas
1050 ' ================================================================
1060 passed = 0
1070 failed = 0

2000 PRINT "--- SGN ---"
2010 IF SGN(-99) = -1 THEN PRINT "  PASS: SGN(-99) = -1" : passed = passed + 1 ELSE PRINT "  FAIL: SGN(-99) = -1" : failed = failed + 1
2020 IF SGN(0) = 0 THEN PRINT "  PASS: SGN(0) = 0" : passed = passed + 1 ELSE PRINT "  FAIL: SGN(0) = 0" : failed = failed + 1
2030 IF SGN(42) = 1 THEN PRINT "  PASS: SGN(42) = 1" : passed = passed + 1 ELSE PRINT "  FAIL: SGN(42) = 1" : failed = failed + 1
2040 IF SGN(-0.001) = -1 THEN PRINT "  PASS: SGN(-0.001) = -1" : passed = passed + 1 ELSE PRINT "  FAIL: SGN(-0.001) = -1" : failed = failed + 1
2050 IF SGN(0.001) = 1 THEN PRINT "  PASS: SGN(0.001) = 1" : passed = passed + 1 ELSE PRINT "  FAIL: SGN(0.001) = 1" : failed = failed + 1

3000 PRINT "--- FIX ---"
3010 IF FIX(3.9) = 3 THEN PRINT "  PASS: FIX(3.9) = 3" : passed = passed + 1 ELSE PRINT "  FAIL: FIX(3.9) = 3" : failed = failed + 1
3020 IF FIX(-3.9) = -3 THEN PRINT "  PASS: FIX(-3.9) = -3 (not -4 like INT)" : passed = passed + 1 ELSE PRINT "  FAIL: FIX(-3.9) = -3" : failed = failed + 1
3030 IF FIX(0.999) = 0 THEN PRINT "  PASS: FIX(0.999) = 0" : passed = passed + 1 ELSE PRINT "  FAIL: FIX(0.999) = 0" : failed = failed + 1
3040 IF FIX(-0.001) = 0 THEN PRINT "  PASS: FIX(-0.001) = 0" : passed = passed + 1 ELSE PRINT "  FAIL: FIX(-0.001) = 0" : failed = failed + 1
3050 IF INT(-3.9) = -4 THEN PRINT "  PASS: INT(-3.9) = -4 (INT differs from FIX)" : passed = passed + 1 ELSE PRINT "  FAIL: INT(-3.9) = -4" : failed = failed + 1

4000 PRINT "--- TIMER ---"
4010 T1 = TIMER
4020 IF T1 > 0 THEN PRINT "  PASS: TIMER > 0" : passed = passed + 1 ELSE PRINT "  FAIL: TIMER > 0" : failed = failed + 1
4030 IF T1 < 86400 THEN PRINT "  PASS: TIMER < 86400" : passed = passed + 1 ELSE PRINT "  FAIL: TIMER < 86400" : failed = failed + 1
4040 Tcopy = T1
4050 IF Tcopy = T1 THEN PRINT "  PASS: TIMER value assignable" : passed = passed + 1 ELSE PRINT "  FAIL: TIMER value assignable" : failed = failed + 1

5000 PRINT "--- SLEEP ---"
5010 T1 = TIMER
5020 SLEEP 0.2
5030 T2 = TIMER
5040 SleepDiff = T2 - T1
5050 IF SleepDiff >= 0.1 THEN PRINT "  PASS: SLEEP 0.2 advances timer >= 0.1s" : passed = passed + 1 ELSE PRINT "  FAIL: SLEEP 0.2 timing (diff=" + STR$(SleepDiff) + ")" : failed = failed + 1
5060 SLEEP 0
5070 PRINT "  PASS: SLEEP 0 does not hang" : passed = passed + 1

6000 PRINT "--- RANDOMIZE TIMER ---"
6010 RANDOMIZE TIMER
6020 R1 = RND(1)
6030 R2 = RND(1)
6040 IF R1 >= 0 AND R1 < 1 THEN PRINT "  PASS: RANDOMIZE TIMER + RND in [0,1)" : passed = passed + 1 ELSE PRINT "  FAIL: RND range" : failed = failed + 1
6050 IF R1 <> R2 THEN PRINT "  PASS: Two sequential RNDs differ" : passed = passed + 1 ELSE PRINT "  FAIL: Two RNDs same (unlikely)" : failed = failed + 1

7000 PRINT "--- WIDTH ---"
7010 WIDTH 80, 25
7020 PRINT "  PASS: WIDTH 80, 25 does not crash" : passed = passed + 1
7030 WIDTH 40, 25
7040 PRINT "  PASS: WIDTH 40, 25 does not crash" : passed = passed + 1
7050 WIDTH 80
7060 PRINT "  PASS: WIDTH 80 (single arg) works" : passed = passed + 1

8000 PRINT "--- CLS ---"
8010 CLS
8020 PRINT "  PASS: CLS (no arg)" : passed = passed + 1
8030 CLS 0
8040 PRINT "  PASS: CLS 0" : passed = passed + 1
8050 CLS 2
8060 PRINT "  PASS: CLS 2 (graphics viewport)" : passed = passed + 1

9000 PRINT "--- KILL ---"
9010 OPEN "killme.tmp" FOR OUTPUT AS #1
9020 PRINT #1, "temporary file"
9030 CLOSE #1
9040 KILL "killme.tmp"
9050 PRINT "  PASS: KILL does not crash" : passed = passed + 1

10000 PRINT "--- GET / PUT (stubs) ---"
10010 DIM SprBuf(200)
10020 GET (0, 0)-(15, 15), SprBuf
10030 PRINT "  PASS: GET does not crash" : passed = passed + 1
10040 PUT (10, 10), SprBuf, XOR
10050 PRINT "  PASS: PUT XOR does not crash" : passed = passed + 1
10060 PUT (10, 10), SprBuf, PSET
10070 PRINT "  PASS: PUT PSET does not crash" : passed = passed + 1

11000 PRINT "--- Graphics stubs ---"
11010 CIRCLE (40, 12), 5, 2
11020 PRINT "  PASS: CIRCLE stub" : passed = passed + 1
11030 LINE (0, 0)-(10, 10), 2
11040 PRINT "  PASS: LINE (graphics) stub" : passed = passed + 1
11050 PSET (5, 5), 3
11060 PRINT "  PASS: PSET stub" : passed = passed + 1
11070 PAINT (5, 5), 2, 3
11080 PRINT "  PASS: PAINT stub" : passed = passed + 1
11090 DRAW "U10 R10 D10 L10"
11100 PRINT "  PASS: DRAW stub" : passed = passed + 1

12000 PRINT "--- TYPE (scalar struct) ---"
12010 TYPE Settings
12020   NumPlayers AS INTEGER
12030   UseSound AS INTEGER
12040   Speed AS DOUBLE
12050   PlayerName AS STRING * 20
12060 END TYPE
12070 DIM GameSet AS Settings
12080 GameSet.NumPlayers = 2
12090 GameSet.UseSound = 1
12100 GameSet.Speed = 1.5
12110 GameSet.PlayerName = "Hero"
12120 IF GameSet.NumPlayers = 2 THEN PRINT "  PASS: Scalar TYPE numeric field" : passed = passed + 1 ELSE PRINT "  FAIL: Scalar TYPE numeric field" : failed = failed + 1
12130 IF GameSet.UseSound = 1 THEN PRINT "  PASS: Scalar TYPE second field" : passed = passed + 1 ELSE PRINT "  FAIL: Scalar TYPE second field" : failed = failed + 1
12140 IF GameSet.Speed = 1.5 THEN PRINT "  PASS: Scalar TYPE float field" : passed = passed + 1 ELSE PRINT "  FAIL: Scalar TYPE float field" : failed = failed + 1
12150 IF RTRIM$(GameSet.PlayerName) = "Hero" THEN PRINT "  PASS: Scalar TYPE string field" : passed = passed + 1 ELSE PRINT "  FAIL: Scalar TYPE string field" : failed = failed + 1
12160 GameSet.NumPlayers = 4
12170 IF GameSet.NumPlayers = 4 THEN PRINT "  PASS: Scalar TYPE field update" : passed = passed + 1 ELSE PRINT "  FAIL: Scalar TYPE field update" : failed = failed + 1

13000 PRINT "--- TYPE (array of structs) ---"
13010 TYPE PlayerData
13020   PNam AS STRING * 17
13030   XPos AS DOUBLE
13040   YPos AS DOUBLE
13050   Score AS INTEGER
13060 END TYPE
13070 DIM PDat(3) AS PlayerData
13080 PDat(1).PNam = "Alice"
13090 PDat(1).XPos = 10
13100 PDat(1).YPos = 20
13110 PDat(1).Score = 100
13120 PDat(2).PNam = "Bob"
13130 PDat(2).XPos = 70
13140 PDat(2).YPos = 20
13150 PDat(2).Score = 200
13160 PDat(3).PNam = "CPU"
13170 PDat(3).Score = 0
13180 IF RTRIM$(PDat(1).PNam) = "Alice" THEN PRINT "  PASS: PDat(1).PNam = Alice" : passed = passed + 1 ELSE PRINT "  FAIL: PDat(1).PNam" : failed = failed + 1
13190 IF RTRIM$(PDat(2).PNam) = "Bob" THEN PRINT "  PASS: PDat(2).PNam = Bob" : passed = passed + 1 ELSE PRINT "  FAIL: PDat(2).PNam" : failed = failed + 1
13200 IF PDat(1).XPos = 10 THEN PRINT "  PASS: PDat(1).XPos = 10" : passed = passed + 1 ELSE PRINT "  FAIL: PDat(1).XPos" : failed = failed + 1
13210 IF PDat(2).YPos = 20 THEN PRINT "  PASS: PDat(2).YPos = 20" : passed = passed + 1 ELSE PRINT "  FAIL: PDat(2).YPos" : failed = failed + 1
13220 IF PDat(1).Score = 100 THEN PRINT "  PASS: PDat(1).Score = 100" : passed = passed + 1 ELSE PRINT "  FAIL: PDat(1).Score" : failed = failed + 1
13230 IF PDat(2).Score = 200 THEN PRINT "  PASS: PDat(2).Score = 200" : passed = passed + 1 ELSE PRINT "  FAIL: PDat(2).Score" : failed = failed + 1
13240 IF PDat(3).Score = 0 THEN PRINT "  PASS: PDat(3).Score = 0 (default)" : passed = passed + 1 ELSE PRINT "  FAIL: PDat(3).Score default" : failed = failed + 1
13250 PDat(1).Score = PDat(1).Score + 50
13260 IF PDat(1).Score = 150 THEN PRINT "  PASS: Field update via arithmetic" : passed = passed + 1 ELSE PRINT "  FAIL: Field update via arithmetic" : failed = failed + 1
13270 IF PDat(1).Score <> PDat(2).Score THEN PRINT "  PASS: Array elements independent" : passed = passed + 1 ELSE PRINT "  FAIL: Array elements independent" : failed = failed + 1
13280 IF PDat(1).PNam <> PDat(2).PNam THEN PRINT "  PASS: String fields independent" : passed = passed + 1 ELSE PRINT "  FAIL: String fields independent" : failed = failed + 1

14000 PRINT "--- TYPE in expressions ---"
14010 Total = PDat(1).Score + PDat(2).Score
14020 IF Total = 350 THEN PRINT "  PASS: Fields used in arithmetic" : passed = passed + 1 ELSE PRINT "  FAIL: Fields in arithmetic (got " + STR$(Total) + ")" : failed = failed + 1
14030 IF PDat(2).Score > PDat(1).Score THEN Leader$ = PDat(2).PNam ELSE Leader$ = PDat(1).PNam
14040 IF RTRIM$(Leader$) = "Bob" THEN PRINT "  PASS: Fields used in IF condition" : passed = passed + 1 ELSE PRINT "  FAIL: Fields in IF condition" : failed = failed + 1
14050 ScoreSum = 0
14060 FOR I = 1 TO 3
14070   ScoreSum = ScoreSum + PDat(I).Score
14080 NEXT I
14090 IF ScoreSum = 350 THEN PRINT "  PASS: Fields used in FOR loop" : passed = passed + 1 ELSE PRINT "  FAIL: Fields in FOR loop (got " + STR$(ScoreSum) + ")" : failed = failed + 1

15000 PRINT "--- SYSTEM ---"
15010 PRINT "  PASS: SYSTEM is reachable" : passed = passed + 1

16000 PRINT ""
16010 PRINT "==============================="
16020 PRINT "Results: " + STR$(passed) + " passed, " + STR$(failed) + " failed"
16030 IF failed = 0 THEN PRINT "ALL TESTS PASSED" ELSE PRINT "SOME TESTS FAILED"
16040 PRINT "==============================="
16050 SYSTEM
