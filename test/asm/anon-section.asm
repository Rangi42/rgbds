; Anonymous sections are declared without a name, and cannot be referred to by one.

SECTION ROM0[$0]
Anonymous1:
	println "anon1 @=", @
	db $01, $02

; A second anonymous section, with the same type but a different address:
; it must not be merged with the first one.
SECTION ROM0[$4]
Anonymous2:
	println "anon2 @=", @
	db $03

SECTION "named", ROM0[$8]
	println "named @=", @
	db $04

; A section whose name is the empty string is *not* anonymous.
SECTION "", ROM0[$C]
	println "empty @=", @
	db $05