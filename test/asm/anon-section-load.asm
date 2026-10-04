; Anonymous sections can open `LOAD` blocks.

SECTION "Main", ROM0[$0]
	println "main @=", @
	db $AA
	LOAD WRAM0
RamCode::	ld a, [$C000]
	ENDL
	LOAD WRAMX[$D010], BANK[1]
RamVars::	ds 4
	ENDL
	println "main again @=", @
	db $BB