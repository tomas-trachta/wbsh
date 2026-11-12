# A case item that stops on a token it cannot use (fuzz-parse m00246):
# the parser added empty items until memory ran out.
case x in
a || b
case x in
$(true) || exit 1
case x in (a) :;; b && c
esac
