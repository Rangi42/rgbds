; Checks that a two-section cycle is rejected, whether the dependency is made
; via a section name or via a label.

SECTION "C", ROMX, BANK = BANK("Bk")
Rho::

SECTION "Bk", ROMX, BANK = BANK(Rho)
