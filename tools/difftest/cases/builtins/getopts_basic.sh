f() {
    local OPTIND opt
    while getopts "ab:c" opt; do
        echo "opt=$opt arg=${OPTARG-}"
    done
    shift $((OPTIND - 1))
    echo "rest: $*"
}
f -a -b val -c file1 file2
f -ac -bjoined -- -notopt
