; A loop may only have one `UNLESS_BREAK` block
REPT 2
	PRINTLN "body"
UNLESS_BREAK
	PRINTLN "one"
UNLESS_BREAK
	PRINTLN "two"
ENDR
