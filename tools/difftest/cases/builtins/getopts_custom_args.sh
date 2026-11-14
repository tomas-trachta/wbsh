OPTIND=1
while getopts "n:" o -n 5 -n 6; do echo "$o $OPTARG"; done
echo "OPTIND=$OPTIND"
