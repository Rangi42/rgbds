; A macro invoked from a loop body cannot provide that loop's `UNLESS_BREAK` block
MACRO m
UNLESS_BREAK
	PRINTLN "never"
ENDR
ENDM
REPT 2
	m
ENDR
