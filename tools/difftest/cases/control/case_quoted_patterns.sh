v='*'
case star in "$v") echo literal ;; $v) echo glob ;; esac
case '*' in "$v") echo literal-match ;; esac
