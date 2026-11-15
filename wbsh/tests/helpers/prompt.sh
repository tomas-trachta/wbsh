# An elevated shell prints `#` for the prompt escape \$, as bash does for
# root. Expected outputs are recorded unelevated, and CI runners are always
# elevated, so prompt output goes through this filter before it is compared.
as_unelevated() {
    local sigil='\$'
    if [ "${sigil@P}" = '#' ]; then
        sed 's/#/$/g'
    else
        cat
    fi
}
