trap 'echo ERR fired' ERR
if false; then :; fi
false || true
false && true
! false
echo end
