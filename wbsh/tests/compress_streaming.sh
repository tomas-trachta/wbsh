#!/bin/bash
# The codecs, tar, cpio and the pagers stream: data goes through in pieces,
# output starts before the input ends, and memory does not grow with size.

tmp=$(mktemp -d)
cd "$tmp" || exit 1

echo "=== round trips of more data than any buffer"
yes 'a line of compressible text' | head -c 20000000 > big.txt
gzip -c big.txt | zcat | cmp - big.txt && echo "gzip ok"
gzip -1 -c big.txt | zcat | wc -c
bzip2 -c big.txt | bzcat | cmp - big.txt && echo "bzip2 ok"
head -c 5000000 /dev/zero | bzip2 -c | bzcat | wc -c
head -c 5000000 /dev/zero | gzip -9 -c | zcat | wc -c

echo "=== output starts before the input ends"
yes | gzip -c | zcat | head -2
yes | bzip2 -c | bzcat | head -2
gzip -c big.txt > big.gz
zcat big.gz | head -1

echo "=== concatenated members and streams"
{ printf 'first\n' | gzip -c; printf 'second\n' | gzip -c; } | zcat
{ printf 'one\n' | bzip2 -c; printf 'two\n' | bzip2 -c; } | bzcat
{ printf 'kept\n' | gzip -c; printf 'garbage'; } | zcat

echo "=== errors"
head -c 3000 big.gz | zcat > /dev/null
echo "rc=$?"
printf 'not compressed' | zcat
echo "rc=$?"
gzip -t big.gz && echo "test ok"

echo "=== xz filter chains, undone in reverse order"
echo '/Td6WFoAAATm1rRGAgEEACEBEACLIW9J4Bj/Aw5dADGYSduBDUgQgyk7tK18ASa67RXw4AB2w7hO0y/r5DqdeceOXkXTcL7ki30JazdrFJlQaUslCKgajf5eK3s1OTxj0+w+tUMMUSx2Rly0ZV0SL7ydEK0QAcG0RbyEUt00lifADCvhqVz9RUgXWcYIPE/6pNLsAtv4Tos20jyWwddDOXKxxLXottPELteIBRmTtZuTY8UpEy3tm3DTk/vXLnbGiaZChnXzp8JBFWX5KsT7gDSsEyvBfNJNlbnTe7vRkOWZlXYXuajTD8gwjs7QlNo7hpPK4KeOaj/Rs1GZYRsZQM0Z5whb+DTQdmU5xOzjftPfcc3HT5/6gvHJpZI9OAJRQurAIIiGWboqFGOmSFWmsRcCS9k3d4ujGhye/pbnzJuxfq/6OhwOU2eyvUKfzbS7C0eYEBeIWr0FhpjQIKxq0qJmpXqm1npIoo3OHvd52kc8lUwLPUqAl1Iwys2Qn8jc9v4K0Lm5Z6pKJbP2kmTu93hjhAn3WzHLMDd4kZbV8obXiIz5hEJ1CDE8W2BbQ8ZsdHDyeNWmKc4S9uedWafHLyG2qD2j3Xz3VehTgy0pzo3IkpQqQTd44HxaCPl6kKwuzNuATSfzoRaQccEJ3lPWWLdwNe87uQJilF+2XjtsA9nzT1KpN/kYoF8B99kXoMzcietxW8MxKrmtFRgp7silVAVvapxcUsQ4hRdPjrDTIg//N4ZSAOdUgq05Pf2xEw/3pQPZtwmqm+Tq/We6wRWGudbQD0etj+4DnKeh8meXhMurrX+s3UfJe+Qp8xxCD7fSN+zqiRx3wOCBNGmLtOwQq0Fb+q/pzSsLQy2jZIvT7YNeTp3DcOrSmNVvfPbMV0k4M3wYSA9DrQV9OyMPfRrLhNKjTU15TLIsksk5kPy6Ufsp1J+3HnTwRFMkQS9ZGhpFoMaX3t0G/bOxcGus4Amt2WZ7J33ImPgnljlmKMtUg6zz3Z+5dP6i7ZR8miMDtK8L26JJNPDE625utR6uG2GC1BEL97zbNQdDE+ubsZkugbrFE4AkMX+y8m/nhy0AAAAAyTBm5k+XlM8AAaoGgDIAADCUJD6xxGf7AgAAAAAEWVo=' | base64 -d | xzcat | md5sum
echo '/Td6WFoAAATm1rRGAwIDAQIEACEBEAAA0eM6RuAY/wLOXQAxmEnUbDi/XTHyNDlgQfUd70oeT22VAaK7LHASuF6ubgstWMTzg17sD4xPdhsfZ+d3iqXppfXK/HPEjkC8dKgsiuZtVnjGU+R577Qd41iPXXYt/LzFXuSb4OXzNKT0By0wrTQad72Axs1l2/fke3eK/OWPi5QDz6BbiT9vniQ3vsNdfsIGgM0jBV6DW1TfJz380vhFaZS+XLMXCe3t63GlY4uuYJkUkGSV1XRL/jNYvcJgRzDZ9Q0tvX03hY/63rM+PUkvGkcAg9fDNRkCWpmqr8ObMPhWh1jq5yJ17w2+ib0ojriAHAEKxEuVDYyFo9PFUjtH/1j+VO08NLQCRnI0C2UQflGhOCIZ61emUJdflJmfN6IiTgAikZN8nffSeiafRPkTHwcUiO/EkcpqZ3eX7RfKymiQ1uXefyLeFez6iNKpQo2Zo0XUXT7I114IPzLyzv1LvXPi4PnUSZYhzSxoVlAVQIX0Sx96fjgMkry1JtX+tDDC2TaxbOG1sKrzWCvOusHg1Ki0xcV08NbdNN79gHcfSJtPAx8guc2Nulpq55ELlawF58/rarUIx9IH9ZQP8HuDP+tWHUmKs29zU+lnDUWFlSKoVUPEKZDk/hyNR/woAinRq1jcvG0cw0CsU9lFw6YsDyXtw9ffIOeTWUYxnMIiotZfTuErkW3F1eXkCi36YWfZCTgHkYf27+eNclMaqC66v117NeIR3tgSZ5x+Yiys9qsZmHe/IYTZ8izR9ZVeNl1mjEQIs/fmJwsggtWsnE793TulmNKvR0MJrn+uhI6DaR8HJagillEpReH3Y5ZDfBuxVrgrESQ8oqaSVJ1n9S49GWAtsh5g352FgI7po5WfNVeXE+K/Sme6Dr2zenm+7Al24UZJfNuYldKTwq7bzTS/XlJ24ovrqeTV6SUu2MsFCbClvMjJ08N8/OTH2ZZFu8qGno06HxcRlDbjAAAAyTBm5k+XlM8AAe4FgDIAAJqmTe2xxGf7AgAAAAAEWVo=' | base64 -d | xzcat | md5sum
echo '/Td6WFoAAATm1rRGAwIEAAMBAiEBEAAA2WLHYOAY/wS2XQAxmEnUbDi/XTHyNDlhlvMpKpmZWYsl26ch6BwFtuRMacKwQn88eONsVDtUQMQIaANkhdDOGkvKqmC/+1W5C4ypRBRgjESrDlUoJyWNF/0QG2kOCGU+1nHAmOXY2Q8brd6mGCteQZeqIjMDPlv2619NmJ99Hv98NWwSYRmjiX634BWsomvnxgv/RuvTGuLieIUqZPQivmuhOdpIrDynopWXboIzgZITWrBnMQZBvPTlmS4cbhm9Cg7xW/8DbmUcw0xI97OExfwbnoYTifSSY494wAwChvBQF82nDRzsWjNsH0V2CgFAMpdYZPTHPdnMsrB7BXuzsKChOl99/n5YwmHDy7d8oNbBiC05n9LUDoBDjHGQ2Di6YE5GsKAOeVvVsRauRJTLbT/wVrkYKzdPrttfFElxDint9Ank0iUfaIdScaQ8OauFA7AosOMkkfpqpsW5xrXr5QpJuwOg4lOfYPTH98k/MRYX+r4UwtpDc/k9lC8byXfT4RzeAKpZQ2zjEZqPQUFfK9+Yoy8wbGoOrBw9h7U343Twm2+0Sb1QjDNA/jMB203Tc/1N9icc+Be88/7GW3S4RE5qoBeWWhCiufPW86ceqRglH1laxCBwvj9Z2BjAUFedF7P3McxVg2at5bDQTJdpYhu5uPImvL0WcIwcjyjonkA+2dCPtjFDfssE3gVFycoZukKkWnGJZgUoUagGnvbfCI8ASI8tg3K4lNyqymnA/jYvwT2jYJUG8Pc2cYWGJWXuOyGD3UB4qCOshkLRggUKFeUQjD5LurIWj/muG/OeeS6KYGVPeY+CC96nGAB7nfgcd8LQWtuBhqNYSH4dmD/4Nyb5OG05X9x7LuTy2l8pqzUFkm6XbLRi5VmKWkxqPtZnI8ZLnBPFkrR9r4CrNQm5UZqsqFNqdB4LVcUW1WMNDlesJzzhBIEQIkM7AdvxBAgGUBa+TQVMebt8evmUVzcfGDkfsbuNXxhbWyYtaywjrBzSCybglu99IxNaNL6MlBVMzwaq1vjo6bASbg++ugRtvnA4uUh8Xo7VP8wO6/1JJDoJRtqlQuISqRoMs9fChW9kiQiGDUSnVphF31FfPa3VjBxA5OC3hzR0w3NzjyyhXcOIwF5vhcXP/1n10wVCtirmRw8LdeHGZjVTHnljpHjf5MY3S3dVeIRniRKwr+FYpSXiXKQ2lQNmlVMzQGswdjgTO7UvRbFTl61agsCeODPiPUF2mSzh8wCW3SzYCnpX94crRn6mLyp6GmFIGtoCx8OAATtdzKQeEDwZNtoDuxadMx76paW2mR/5ggar4FrZP50bpsgbkeA8IouW1Jg/C3I+YSkCSd2NwY4R65SPKpiuqaFrrCcMJioKrKvr/omKvcdX0XfMKxeTroXSgYq1yb/0QHIWhOJ9WGevkPm/ZB6DF9SfXEWtKoDQlO5k2UPG816vy+pd7HOaeI4aIIQWh6jl0IH1qx/VeVyBA1Z2BDX9bqoQyjE0o35kcuT5u3vyI3xCC5QSpMj0m0T3qIGRj7iCO09PGTFWyvfvaC0m+3yn/UHkITxy+z+imjNwBhky2jutOk2lNwLWJ3mfISf6dQ3Q3Lce7GuKGtksSXcQIkQFBgAAAADJMGbmT5eUzwAB1gmAMgAAW86UwLHEZ/sCAAAAAARZWg==' | base64 -d | xzcat | md5sum

echo "=== tar, member by member"
mkdir -p src/sub
cp big.txt src/sub/big.txt
printf 'small\n' > src/small.txt
tar czf a.tgz src
tar tzf a.tgz
mkdir out
tar xzf a.tgz -C out
cmp out/src/sub/big.txt big.txt && echo "tgz extract ok"
tar cjf a.tbz src
tar xjf a.tbz -O src/small.txt
tar cf - src | tar tf - | wc -l
head -c 2000 a.tgz | tar tzf - > /dev/null
echo "rc=$?"

echo "=== cpio"
find src -type f | sort | cpio -o > a.cpio 2> /dev/null
cpio -t < a.cpio 2> /dev/null
mkdir cpout
(cd cpout && cpio -id < ../a.cpio 2> /dev/null)
cmp cpout/src/sub/big.txt big.txt && echo "cpio extract ok"

echo "=== pagers on an endless pipe"
yes | less | head -2
yes | more | head -2
printf 'x\n' | less

cd / && rm -rf "$tmp"
