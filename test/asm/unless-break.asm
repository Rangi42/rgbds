; An `UNLESS_BREAK` block runs after the loop, unless the loop was broken out of
MACRO test
	FOR x, \1
		PRINTLN "{d:x}"
		IF x == 7
			PRINTLN "break early"
			BREAK
		ENDC
	UNLESS_BREAK
		PRINTLN "all done!"
	ENDR
ENDM
test 5
test 10

; A loop that never runs was not broken out of either, so its block runs
REPT 0
	PRINTLN "no iterations"
UNLESS_BREAK
	PRINTLN "rept 0"
ENDR
FOR x, 2, 2
	PRINTLN "no iterations"
UNLESS_BREAK
	PRINTLN "for empty"
ENDR

; Both the loop body and the block may be empty
REPT 2
UNLESS_BREAK
ENDR
PRINTLN "empty is ok"

; Nested loops each have their own block
REPT 2
	REPT 2
		PRINTLN "  inner"
	UNLESS_BREAK
		PRINTLN "  inner unless"
	ENDR
	PRINTLN "outer"
UNLESS_BREAK
	PRINTLN "outer unless"
ENDR

; Breaking out of the inner loop skips only the inner block
REPT 2
	FOR x, 5
		PRINTLN "  inner {d:x}"
		IF x == 2
			BREAK
		ENDC
	UNLESS_BREAK
		PRINTLN "  inner unless"
	ENDR
	PRINTLN "outer"
UNLESS_BREAK
	PRINTLN "outer unless"
ENDR

; The block may contain loops and conditionals of its own
REPT 2
	PRINTLN "A"
UNLESS_BREAK
	IF 1
		FOR y, 2
			PRINTLN "  A unless {d:y}"
		ENDR
	ELSE
		PRINTLN "  impossible"
	ENDC
ENDR

; Conditionals in the loop body do not confuse the block's placement
REPT 2
	IF 0
		PRINTLN "dead"
	ENDC
	PRINTLN "B"
UNLESS_BREAK
	PRINTLN "B unless"
ENDR

; Loops whose header ends in an expansion capture their block, too
DEF cnt EQUS "2"
REPT cnt
	PRINTLN "equs"
UNLESS_BREAK
	PRINTLN "equs unless"
ENDR
MACRO args
	REPT \1
		PRINTLN "arg"
	UNLESS_BREAK
		PRINTLN "arg unless"
	ENDR
	FOR x, \1
		PRINTLN "arg for {d:x}"
	UNLESS_BREAK
		PRINTLN "arg for unless"
	ENDR
ENDM
args 2

; A loop inside a macro, invoked from inside another loop
MACRO inner
	REPT 2
		PRINTLN "  macro inner"
	UNLESS_BREAK
		PRINTLN "  macro inner unless"
	ENDR
ENDM
REPT 2
	inner
UNLESS_BREAK
	PRINTLN "macro outer unless"
ENDR

; A block gets a unique ID of its own, just like a loop body
REPT 2
	PRINTLN "body id=\@"
UNLESS_BREAK
	PRINTLN "unless id=\@"
ENDR
