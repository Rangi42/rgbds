SECTION "Ldr", ROMX, BANK[1]
Leader::
	ds 16384

SECTION "Dep", ROMX, BANK = BANK(Leader)
	db 1
