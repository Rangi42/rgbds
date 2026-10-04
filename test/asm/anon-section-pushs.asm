; Anonymous sections can be pushed onto the section stack, and can be anonymous themselves.

SECTION "Outer", ROM0[$0]
	println "outer @=", @
	db $01

	PUSHS ROM0[$4]
Pushed::
	println "pushed @=", @
	db $02
	POPS

	println "outer again @=", @
	db $03