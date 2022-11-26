#!/bin/bash

TMPDIR="$(mktemp -d)"
echo "BITS 32" > "${TMPDIR}/src.asm"
echo "$@" >> "${TMPDIR}/src.asm"
if nasm -f bin -o "${TMPDIR}/binary" "${TMPDIR}/src.asm"
then
    xxd -p "${TMPDIR}/binary"
fi
rm -r -- "${TMPDIR}"
