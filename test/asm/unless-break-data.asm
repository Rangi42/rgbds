SECTION "test", ROM0
REPT 2
	db 1
UNLESS_BREAK
	; The block is emitted after all of the loop's iterations, and gets its own file stack node
Label:
	db 2, 3
	dw Label
ENDR
	db 4
