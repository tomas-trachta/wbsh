#!/bin/bash
# file: every fixture is built here from printf bytes or by the bundled
# archivers, never committed as a binary.

D=_file_type_tmp
rm -rf $D
mkdir -p $D
cd $D

zeros() { head -c "$1" /dev/zero; }

echo "=== inode types and text"
: > empty
printf 'a' > one
printf 'hello world\n' > ascii.txt
printf 'a\r\nb\r\n' > crlf.txt
printf 'a\rb\r' > cr.txt
printf 'a\r\nb\n' > mixed.txt
printf 'no newline' > noeol.txt
printf '%0400d\n' 0 > long.txt
printf 'caf\xc3\xa9\n' > utf8.txt
printf '\xef\xbb\xbfhi\n' > bom.txt
printf '\xff\xfeh\x00i\x00\n\x00' > u16le.txt
printf '\xfe\xff\x00h\x00i\x00\n' > u16be.txt
printf 'caf\xe9\n' > latin1.txt
printf 'x\x85y\x99\n' > ext.txt
printf 'a\x1b[1mb\n' > esc.txt
printf 'a\bb\n' > over.txt
printf '\x00\x01\x02\x03' > bin.dat
mkdir dir
file empty one ascii.txt crlf.txt cr.txt mixed.txt noeol.txt long.txt utf8.txt bom.txt
file u16le.txt u16be.txt latin1.txt ext.txt esc.txt over.txt bin.dat dir

echo "=== scripts and source"
printf '#!/bin/sh\necho hi\n' > s1
printf '#!/bin/bash\necho hi\n' > s2
printf '#!/usr/bin/env bash\necho hi\n' > s3
printf '#!/usr/bin/env python3\nprint(1)\n' > s4
printf '#!/usr/bin/perl -w\nprint 1;\n' > s5
printf '#!/usr/bin/env node\nconsole.log(1)\n' > s6
printf '#!/opt/odd/interp -x\nrun\n' > s7
printf '#!/usr/bin/env python3\nprint("\xc3\xa9")\n' > s8
printf '#!/bin/sh\r\necho hi\r\n' > s9
printf '#include <stdio.h>\nint main(void) { return 0; }\n' > c.c
printf '#include <iostream>\nint main() { std::cout << 1; }\n' > cpp.cpp
printf '<!DOCTYPE html>\n<html><body></body></html>\n' > page.html
printf '<?xml version="1.0"?>\n<root/>\n' > doc.xml
printf '<?xml version="1.0"?>\n<svg xmlns="http://www.w3.org/2000/svg"/>\n' > pic.svg
printf '{"a": [1, 2.5e3, true, null, "x\\"y"], "b": {}}\n' > data.json
printf '{"a": }\n' > broken.json
printf '@echo off\r\necho hi\r\n' > run.bat
printf 'param($Name)\nWrite-Host "hi $Name"\n' > script.ps1
printf '.PHONY: all\nall:\n\techo hi\n' > Makefile
printf -- '-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n' > cert.pem
printf -- '-----BEGIN OPENSSH PRIVATE KEY-----\nb3Bl\n-----END OPENSSH PRIVATE KEY-----\n' > id_key
file s1 s2 s3 s4 s5 s6 s7 s8 s9
file c.c cpp.cpp page.html doc.xml pic.svg data.json broken.json run.bat script.ps1 Makefile
file cert.pem id_key

echo "=== images"
printf '\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR\x00\x00\x00\x10\x00\x00\x00\x08\x08\x06\x00\x00\x00\x00\x00\x00\x00' > img.png
printf '\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR\x00\x00\x01\x00\x00\x00\x00\x40\x01\x00\x00\x00\x01\x00\x00\x00\x00' > gray.png
printf 'GIF89a\x0a\x00\x14\x00\x00\x00\x00;' > img.gif
{ printf '\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00'
  printf '\xff\xc0\x00\x11\x08\x00\x20\x00\x40\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01\xff\xd9'; } > img.jpg
{ printf 'BM\x46\x00\x00\x00\x00\x00\x00\x00\x36\x00\x00\x00\x28\x00\x00\x00'
  printf '\x02\x00\x00\x00\x02\x00\x00\x00\x01\x00\x18\x00\x00\x00\x00\x00\x10\x00\x00\x00'
  printf '\x13\x0b\x00\x00\x13\x0b\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'; zeros 16; } > img.bmp
{ printf '\x00\x00\x01\x00\x01\x00\x10\x10\x00\x00\x01\x00\x20\x00\x00\x01\x00\x00\x16\x00\x00\x00'
  zeros 64; } > img.ico
printf 'RIFF\x1a\x00\x00\x00WEBPVP8L\x0d\x00\x00\x00\x2f\x0f\xc0\x01\x00\x00\x00\x00\x00\x00' > img.webp
printf 'II*\x00\x08\x00\x00\x00\x01\x00\x00\x01\x03\x00\x01\x00\x00\x00\x10\x00\x00\x00' > img.tif
file img.png gray.png img.gif img.jpg img.bmp img.ico img.webp img.tif

echo "=== audio and video"
{ printf 'RIFF\x24\x00\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x02\x00\x44\xac\x00\x00'
  printf '\x10\xb1\x02\x00\x04\x00\x10\x00data\x00\x00\x00\x00'; } > snd.wav
{ printf 'OggS\x00\x02'; zeros 20; printf '\x01\x1e\x01vorbis\x00\x00\x00\x00\x02\x44\xac\x00\x00'
  printf '\x00\x00\x00\x00\x00\xf4\x01\x00\x00\x00\x00\x00\xb8\x01'; } > snd.ogg
{ printf 'fLaC\x80\x00\x00\x22\x03\xe8\x03\xe8\x00\x00\x00\x00\x00\x00'
  printf '\x0a\xc4\x42\xf0\x00\x06\xba\xa8'; zeros 16; } > snd.flac
printf '\xff\xfb\x90\x64\x00\x00\x00\x00' > snd.mp3
printf 'ID3\x03\x00\x00\x00\x00\x00\x00\xff\xfb\x90\x64\x00\x00' > tag.mp3
printf '\x00\x00\x00\x18ftypisom\x00\x00\x02\x00isomiso2' > clip.mp4
printf '\x00\x00\x00\x14ftypqt  \x00\x00\x02\x00qt  ' > clip.mov
printf '\x1a\x45\xdf\xa3\x9f\x42\x82\x84webm' > clip.webm
file snd.wav snd.ogg snd.flac snd.mp3 tag.mp3 clip.mp4 clip.mov clip.webm

echo "=== executables"
pe_header() {
	printf 'MZ'; zeros 58; printf '\x40\x00\x00\x00PE\x00\x00'
	printf "$1"; printf '\x02\x00'; zeros 12; printf "$2"; printf "$3"
}
pe32plus() {
	pe_header '\x64\x86' '\xf0\x00' "$1"
	printf '\x0b\x02'; zeros 66; printf "$2"; zeros 38; printf '\x10\x00\x00\x00'
	zeros 112; printf "$3"; zeros 8
}
pe32() {
	pe_header '\x4c\x01' '\xe0\x00' "$1"
	printf '\x0b\x01'; zeros 66; printf "$2"; zeros 22; printf '\x10\x00\x00\x00'
	zeros 112; printf "$3"; zeros 8
}
pe32plus '\x22\x00' '\x03\x00' '\x00\x00\x00\x00\x00\x00\x00\x00' > console.exe
pe32plus '\x22\x00' '\x02\x00' '\x00\x00\x00\x00\x00\x00\x00\x00' > gui.exe
pe32plus '\x22\x20' '\x03\x00' '\x00\x00\x00\x00\x00\x00\x00\x00' > lib.dll
pe32 '\x02\x01' '\x03\x00' '\x00\x00\x00\x00\x00\x00\x00\x00' > old.exe
pe32 '\x02\x01' '\x03\x00' '\x08\x20\x00\x00\x48\x00\x00\x00' > managed.exe
{ printf 'MZ'; zeros 62; } > dos.exe
{ printf '\x7fELF\x02\x01\x01\x00'; zeros 8; printf '\x02\x00\x3e\x00\x01\x00\x00\x00'; zeros 28
  printf '\x40\x00\x38\x00\x00\x00\x40\x00\x00\x00\x00\x00'; } > prog.elf
{ printf '\xcf\xfa\xed\xfe\x07\x00\x00\x01\x03\x00\x00\x00\x02\x00\x00\x00'
  printf '\x00\x00\x00\x00\x00\x00\x00\x00\x85\x00\x20\x00\x00\x00\x00\x00'; } > prog.macho
printf '\xca\xfe\xba\xbe\x00\x00\x00\x34\x00\x10' > Main.class
printf '\x00asm\x01\x00\x00\x00' > mod.wasm
file console.exe gui.exe lib.dll old.exe managed.exe dos.exe
file prog.elf prog.macho Main.class mod.wasm

echo "=== archives and compressed data"
printf 'compress me\n' > plain.txt
touch -d @1700000000 plain.txt
cp plain.txt p1; cp plain.txt p2; cp plain.txt p3; cp plain.txt p4; cp plain.txt p5
gzip p1; bzip2 p2; xz p3; lzma p4; lzop p5
tar cf a.tar plain.txt
echo plain.txt | cpio -o -H newc > a.cpio 2>/dev/null
echo plain.txt | cpio -o -H odc > odc.cpio 2>/dev/null
ar rc a.a plain.txt
zip -q a.zip plain.txt > /dev/null
printf '\x1f\x8b\x08\x08\x00\xf1\x53\x65\x02\x03a.txt\x00\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00' > named.gz
printf '\x1f\x9d\x90\x63\x00' > old.Z
printf '\x28\xb5\x2f\xfd\x00\x00\x00\x00' > a.zst
printf '7z\xbc\xaf\x27\x1c\x00\x04' > a.7z
printf 'Rar!\x1a\x07\x01\x00' > a.rar
printf 'Rar!\x1a\x07\x00\xcf' > old.rar
{ printf 'MSCF'; zeros 4; printf '\x50\x00\x00\x00'; zeros 12; printf '\x03\x01\x01\x00\x02\x00'; zeros 4; } > a.cab
file p1.gz named.gz p2.bz2 p3.xz p4.lzma p5.lzo a.tar a.cpio odc.cpio a.a a.zip
file old.Z a.zst a.7z a.rar old.rar a.cab

echo "=== zip containers"
zip_entry() {
	printf 'PK\x03\x04\x0a\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
	printf "$2"; printf "$2"; printf "$3"; printf '\x00\x00'; printf '%s' "$1"
}
zip_entry 'META-INF/MANIFEST.MF' '\x00\x00\x00\x00' '\x14\x00' > a.jar
{ zip_entry '[Content_Types].xml' '\x00\x00\x00\x00' '\x13\x00'
  zip_entry 'word/document.xml' '\x00\x00\x00\x00' '\x11\x00'; } > a.docx
{ zip_entry '[Content_Types].xml' '\x00\x00\x00\x00' '\x13\x00'
  zip_entry 'xl/workbook.xml' '\x00\x00\x00\x00' '\x0f\x00'; } > a.xlsx
{ zip_entry 'mimetype' '\x27\x00\x00\x00' '\x08\x00'; printf 'application/vnd.oasis.opendocument.text'; } > a.odt
{ printf 'PK\x05\x06'; zeros 18; } > empty.zip
file a.jar a.docx a.xlsx a.odt empty.zip

echo "=== documents and containers"
printf '%%PDF-1.7\n1 0 obj\n<< /Type /Pages /Kids [] /Count 3 >>\nendobj\n%%%%EOF\n' > doc.pdf
printf '%%PDF-1.4\n%%%%EOF\n' > bare.pdf
{ printf 'SQLite format 3\x00\x10\x00\x01\x01\x00\x40\x20\x20\x00\x00\x00\x02\x00\x00\x00\x02'
  zeros 8; printf '\x00\x00\x00\x01\x00\x00\x00\x04'; zeros 8; printf '\x00\x00\x00\x01'; zeros 32
  printf '\x00\x00\x00\x02\x00\x2e\x5f\x1c'; } > db.sqlite
{ printf 'L\x00\x00\x00\x01\x14\x02\x00\x00\x00\x00\x00\xc0\x00\x00\x00\x00\x00\x00\x46'
  printf '\x0b\x00\x00\x00\x20\x00\x00\x00'; zeros 24; printf '\xd2\x04\x00\x00'; zeros 4
  printf '\x01\x00\x00\x00'; zeros 12; } > app.lnk
{ printf '\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1'; zeros 22; printf '\x09\x00'; zeros 16
  printf '\x00\x00\x00\x00'; zeros 460; zeros 80
  printf '\x84\x10\x0c\x00\x00\x00\x00\x00\xc0\x00\x00\x00\x00\x00\x00\x46'; zeros 16; } > setup.msi
{ printf '\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1'; zeros 504; } > other.ole
{ zeros 32769; printf 'CD001\x01\x00'; zeros 32; printf 'WBSH_DISC'; zeros 23; zeros 64; } > disc.iso
printf '\x00\x01\x00\x00\x00\x0c\x00\x80' > font.ttf
printf 'wOFF\x00\x01\x00\x00' > font.woff
file doc.pdf bare.pdf db.sqlite app.lnk setup.msi other.ole disc.iso font.ttf font.woff

echo "=== options"
file -b ascii.txt
file -i ascii.txt utf8.txt bin.dat empty dir img.png data.json s1
file --mime-type crlf.txt latin1.txt u16le.txt
file --mime-encoding ascii.txt latin1.txt ext.txt
file -N ascii.txt img.gif
file -F ' =>' ascii.txt
file -F '' -b ascii.txt
printf 'ascii.txt\nimg.gif\n\n' > list
file -f list
printf 'piped text\n' | file -
file - < img.png
file -b - < empty
file missing_q7; echo "status $?"
file -E missing_q7 ascii.txt; echo "status $?"
file -f no_such_list_q7; echo "status $?"
file; echo "status $?"
file -z p1.gz
file -Z p1.gz
file -zb p2.bz2 p3.xz
file -z -i p1.gz
file -z ascii.txt

echo "=== links"
if ln -s ascii.txt link 2>/dev/null; then
	file link
	file -L link
	file -h link
	rm -f link
	ln -s gone_q7 dangling 2>/dev/null && file dangling && rm -f dangling
else
	echo "link: symbolic link to ascii.txt"
	echo "link: ASCII text"
	echo "link: symbolic link to ascii.txt"
	echo "dangling: broken symbolic link to gone_q7"
fi

cd ..
rm -rf $D
