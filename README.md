# EBookReaderFor3DS

Standalone Nintendo 3DS EPUB reader project based on the design and core reader logic from `EbookReaderForDS`.

This project does not modify the DS app. It keeps the existing EPUB parser, bookmarks, table of contents, image preview, themes, and settings flow, but swaps the hardware layer for a native 3DS target.

## Current Layout

- `source/`: 3DS app source
- `include/`: shared headers and 3DS platform declarations
- `lib/`: local `freetype` and `zlib` archives reused from the DS project
- `sdmc_template/data/`: fonts, translations, encodings, and runtime folders to copy to the SD card
- `tools/`: EPUB helper scripts copied from the DS project

## Runtime Paths

The app expects these folders on the SD card:

- `sdmc:/3ds/EBookReaderFor3DS/data/fonts/`
- `sdmc:/3ds/EBookReaderFor3DS/data/translations/`
- `sdmc:/3ds/EBookReaderFor3DS/data/encodings/`
- `sdmc:/3ds/EBookReaderFor3DS/data/bookmarks/`
- `sdmc:/3ds/EBookReaderFor3DS/books/`

You can also keep books in `sdmc:/books/`. The browser checks that path first.

To prepare runtime data, copy the contents of [`sdmc_template/data/`](/home/rena/Work/NDS/EBookReaderFor3DS/sdmc_template/data) into `sdmc:/3ds/EBookReaderFor3DS/data/`.

## Build

This project uses the standard devkitPro 3DS toolchain:

- `devkitARM`
- `libctru`
- 3DS build tools such as `3dsxtool` and `smdhtool`

Build with:

```sh
make
```

The expected output is:

- `EBookReaderFor3DS.3dsx`
- `EBookReaderFor3DS.smdh`

## EPUB Prep

Use the bundled optimizer before copying large books to the SD card:

```sh
python3 tools/optimize_epub.py book.epub book-3ds.epub
```

The default preset is now tuned for 3DS screens (`400x240`, lighter JPEG/PNG budgets) and it reports whether the EPUB spine is still too large for the 3DS parser. If the tool warns that total spine text or a single chapter is over budget, split the book first:

```sh
python3 tools/split_epub.py book.epub out_dir/
```

## Notes

- The renderer uses a 400x240 top-screen buffer and a 320x240 bottom-screen buffer, so the top screen can show a wider reading page, preview cards, and status information.
- Home, browser, bookmark, and settings screens now favor simpler 3DS-native panels instead of DS-style pop-up dashboards on the top screen.
- Reader and cover-preview caches are a bit larger than the DS build to take advantage of 3DS memory headroom.
- Search uses the 3DS software keyboard instead of the DS on-screen keyboard.
- The DS project remains untouched in [`EbookReaderForDS/`](/home/rena/Work/NDS/EbookReaderForDS).
