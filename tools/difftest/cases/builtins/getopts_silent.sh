f() {
    local OPTIND opt
    while getopts ":ab:" opt; do echo "opt=$opt OPTARG=${OPTARG-}"; done
}
f -x -b
