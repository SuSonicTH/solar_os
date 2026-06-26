# Vendored bdfconv

This directory vendors the small `bdfconv` tool from U8g2:

```text
https://github.com/olikraus/u8g2/tree/master/tools/font/bdfconv
```

Only the portable C source, headers, Makefile, and U8g2 license text are kept
here. Build products, Windows binaries, and batch files are intentionally not
vendored.

Build:

```sh
make -C fonts/tools/bdfconv
```

Clean:

```sh
make -C fonts/tools/bdfconv clean
```

The font generator automatically uses `fonts/tools/bdfconv/bdfconv` if it has
been built. An explicit `--bdfconv /path/to/bdfconv` argument still overrides
that.

License: U8g2 is BSD-2-Clause; see `LICENSE.u8g2`.
