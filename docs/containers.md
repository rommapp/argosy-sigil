# Containers

A `.zip` holding any format sigil reads is read in place, with no
extraction step: sigil opens the archive's member, resolves the platform
from it, and runs the normal extractor against it. `.wua` and `.zar` are
both ZArchive and are read the same way.

## ZArchive (`.wua`, `.zar`): cheap random access

One container under
two names: Cemu writes it for Wii U, Xenia writes it for Xbox 360, and
both vendor the same library, so the metadata parsing is shared. Contents
sit in fixed 64 KiB blocks with an offset record for every sixteen, where
each record holds a full 64-bit base offset and the compressed size of
each block minus one. Reaching an arbitrary byte therefore costs one
record read, one block read and one zstd call regardless of how deep it
sits. Identifying a title out of a multi-gigabyte archive takes a couple
of blocks, not a decompression pass.

## Zip: streaming, with an emulated seek

`sigil_io` is a random-access
contract and deflate is a forward-only stream, so the shim inflates and
discards to reach a forward offset and restarts the decoder to reach a
backward one. The disc walkers touch a handful of ascending offsets, so a
restart is rare and never more than one per extraction. Cost scales with
how far into the member the identifier sits rather than with the member's
size, which is why a trimmed image in a zip is far cheaper than a full
redump in one. Stored (uncompressed) members skip all of it and read
through directly.

ZIP64 is handled, and required rather than optional: a redump exceeds
every 32-bit field in the classic records, so the real sizes and offsets
live only in the ZIP64 extra field.

Members are chosen one of two ways. By default the largest non-directory
member wins, which picks the disc image out of an archive that also holds
a readme. A caller can instead ask for a member by path suffix, which is
how a metadata file under a directory named for the title is reached; the
shallowest match wins there, so a nested copy of the same filename cannot
shadow the real one.

## 7z: recognised, not read

`.7z` is recognised as an archive but has no reader. Adding one means
vendoring the LZMA SDK's container sources (`7zArcIn.c`, `7zDec.c`,
`Lzma2Dec.c` and friends); `third_party` currently carries only the
`LzmaDec.c` libchdr needs. Until then a `.7z` falls through to the
filename scanner. Note that LZMA offers no offset table and 7z is solid
by default, so it could never be as cheap to seek into as ZArchive is.
