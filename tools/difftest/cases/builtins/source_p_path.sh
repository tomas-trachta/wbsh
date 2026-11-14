# requires: bash>=5.3
mkdir lib
echo 'echo "from lib: $1"' > lib/part.sh
echo 'echo "from cwd"' > here.sh
source -p lib part.sh one
. -p "lib:." here.sh
. -p lib here.sh
echo "status $?"
source -p '' here.sh
