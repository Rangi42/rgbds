MACRO while
	rept 999_999_999
		if !(\1)
			break
		endc
		__BODY__
	endr
ENDM

DEF n = 1
while n < 10 [:
	println "n is {d:n}"
	DEF i = n
	while i > 0 [:
		DEF n += 1
		DEF i -= 1
	:]
:]
