# TJpgDec

TJpgDec R0.03, the Tiny JPEG Decompressor by ChaN (http://elm-chan.org/fsw/tjpgd/),
from Bodmer's copy (https://github.com/Bodmer/TJpg_Decoder), for the engine's covers
(`pva_art.c`). Its own licence is at the top of `tjpgd.c`: free for any use, the
copyright notice kept.

Changed here:

* `tjpgdcnf.h`: the engine's configuration (RGB888 output, scaling on, the 32-bit
  barrel shifter).
* `tjpgd.h`: `stdint.h` on every platform (it defined the integer types itself on
  Windows, which clashes with today's compilers).

Baseline JPEGs only: a progressive one is no cover (the engine logs it).
