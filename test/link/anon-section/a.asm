; Two anonymous sections with identical constraints: they must not be merged with each
; other, nor with the anonymous section in `b.asm`. Sizes differ, so that the linker's
; placement order (decreasing size) is easy to predict.
SECTION ROM0
A1::	db $01
SECTION ROM0
A2::	db $01, $02, $03