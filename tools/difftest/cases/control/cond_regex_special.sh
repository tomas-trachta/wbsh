[[ "a.b" =~ a\.b ]] && echo escaped-dot
[[ "foo bar" =~ foo\ bar ]] && echo escaped-space
[[ "x(y)" =~ \(y\) ]] && echo parens
re='^[[:alpha:]]+$'
[[ hello =~ $re ]] && echo class
