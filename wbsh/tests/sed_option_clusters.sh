#!/bin/bash
# sed takes combined short options and GNU's long forms.

printf 'a1\nb2\na3\n' > _sed_clusters.txt
sed -nE 's/a([0-9])/A\1/p' _sed_clusters.txt
sed -ne '/b/p' _sed_clusters.txt
sed -En 's/(b)/[\1]/p' _sed_clusters.txt
sed -n -e 1p -e 3p _sed_clusters.txt
sed --quiet --expression=2p _sed_clusters.txt
sed -nr 's/^(.)(.)$/\2\1/p' _sed_clusters.txt
sed -s -n '$p' _sed_clusters.txt _sed_clusters.txt
printf 's/a/X/\n' > _sed_clusters.sed
sed -nf _sed_clusters.sed -e p _sed_clusters.txt | head -1
sed 1d -n -e p _sed_clusters.txt

echo "=== in place"
cp _sed_clusters.txt _sed_ip.txt
sed -Ei 's/([a-z])/<\1>/' _sed_ip.txt; cat _sed_ip.txt
sed -i.bak -n '1p' _sed_ip.txt; cat _sed_ip.txt _sed_ip.txt.bak
sed --in-place=.orig 's/</(/' _sed_ip.txt; cat _sed_ip.txt _sed_ip.txt.orig

echo "=== errors"
sed -nQ p _sed_clusters.txt; echo "status $?"
sed --bogus p _sed_clusters.txt; echo "status $?"
sed -n; echo "status $?"
rm -f _sed_clusters.txt _sed_clusters.sed _sed_ip.txt _sed_ip.txt.bak _sed_ip.txt.orig
