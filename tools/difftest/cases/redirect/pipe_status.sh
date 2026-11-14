true | false | true; echo "${PIPESTATUS[@]}"
false | true; echo "${PIPESTATUS[*]} $?"
