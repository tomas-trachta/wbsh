#!/bin/bash
# awk sub/gsub: & and backslash rules in the replacement, anchors, empty matches.

echo "=== ampersand and backslashes"
echo foo | awk '{gsub(/o/,"<&>")}1'
echo aaa | awk '{gsub(/a/,"$$")}1'
echo abc | awk '{gsub(/b/,"[\\&]")}1'
echo abc | awk '{gsub(/b/,"[\\\\&]")}1'
echo abc | awk '{gsub(/b/,"[\\\\\\&]")}1'
echo abc | awk '{gsub(/b/,"[\\\\]")}1'
echo abc | awk '{gsub(/b/,"[\\q]")}1'
echo 'aaa bbb' | awk '{sub(/a+/,"[&&]"); print; print NF}'
echo 'path/to/file' | awk '{gsub("/","\\\\"); print}'

echo "=== anchors"
echo aaa | awk '{gsub(/^a/,"X")}1'
echo aaa | awk '{sub(/a$/,"X")}1'
echo abab | awk '{n=gsub(/$/,"X"); print n, $0}'
echo 'foo bar' | awk '{gsub(/^/,">")}1'
echo 'abc' | awk '{gsub(/$/,"!"); gsub(/^/,"?")}1'

echo "=== empty matches"
echo abc | awk '{gsub(/b*/,"-")}1'
echo abc | awk '{gsub(/x*/,"-")}1'
echo 'foo bar' | awk '{sub(/o*/,"X")}1'
echo 'ab' | awk '{gsub(//,"-")}1'

echo "=== targets and counts"
echo 'a.b.c' | awk '{gsub(".","-")}1'
echo 'a.b.c' | awk '{gsub(/\./,"-")}1'
echo 'x y z' | awk '{n=gsub(/ /,"_",$2); print n, $0}'
echo 'abcabc' | awk '{gsub(/b|c/,"#")}1'
echo 'one two' | awk '{s=$0; n=sub(/two/,"[&]",s); print n, s, $0}'
echo 'aaa' | awk '{print gsub(/a/,"b"), $0}'
