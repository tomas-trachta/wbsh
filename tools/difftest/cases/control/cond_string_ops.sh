[[ abc == a* ]] && echo glob-eq
[[ abc == "a*" ]] || echo quoted-literal
[[ abc != b* ]] && echo ne
[[ a < b ]] && echo lt
[[ b > a ]] && echo gt
[[ -z "" && -n x ]] && echo z-n
