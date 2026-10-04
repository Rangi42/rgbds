; A named section, and one whose name is the empty string, which is *not* anonymous.
SECTION ROM0
B1::	db $01, $02
SECTION "", ROM0
B2::	db $01, $02, $03, $04