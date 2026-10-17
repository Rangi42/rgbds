; Checks that sections can reference each other's banks, and that code can use
; the values through ordinary `BANK()` expressions.

SECTION "Ldr", ROMX, BANK[1]
Leader::
	db $01, $02, $03
	dw BANK(Dep1)
	dw BANK(Dep2)
	dw BANK(Leader)
	dw BANK("Dep2")

SECTION "Dep1", ROMX, BANK = BANK(Leader)
Dep1::
	db $04

SECTION "Dep2", ROMX, BANK = BANK("Ldr") + 1
Dep2::
	db $05
