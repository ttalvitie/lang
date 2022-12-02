#!/bin/bash

TMPDIR="$(mktemp -d)"
echo "BITS 32" > "${TMPDIR}/src.asm"

if [ "$#" == "0" ]
then
    cat >> "${TMPDIR}/src.asm"
else
    echo "$@" >> "${TMPDIR}/src.asm"
fi

if nasm -f bin -o "${TMPDIR}/binary" "${TMPDIR}/src.asm"
then
    xxd -u -p "${TMPDIR}/binary"
fi
rm -r -- "${TMPDIR}"
