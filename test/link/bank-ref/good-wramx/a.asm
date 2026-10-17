; Checks that `BANK = BANK()` also works for RAM types; those don't produce
; output data, so only the map/symbol files can verify their placement.

SECTION "LdrW", WRAMX, BANK[1]
LeaderW::
	ds 100

SECTION "DepW", WRAMX, BANK = BANK(LeaderW) + 1
DepW::
	ds 10
