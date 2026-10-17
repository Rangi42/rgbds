; Checks that a leader's bank can be followed across object files, when it is
; exported.

SECTION "Ldr", ROMX
Ldr::
	db $01
	dw BANK(Dep)

SECTION "Ldr2", ROMX, BANK[1]
	ds 100
