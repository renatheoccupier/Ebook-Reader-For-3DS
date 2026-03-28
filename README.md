# EBookReaderFor3DS

Find better app

## Info

- Still on dev and it really bad

## Runtime Paths

The app expects these folders on the SD card:

- `sdmc:/3ds/EBookReaderFor3DS/data/fonts/`
- `sdmc:/3ds/EBookReaderFor3DS/data/translations/`
- `sdmc:/3ds/EBookReaderFor3DS/data/encodings/`
- `sdmc:/3ds/EBookReaderFor3DS/data/bookmarks/`
- `sdmc:/3ds/EBookReaderFor3DS/books/`

You can also keep books in `sdmc:/books/`. The browser checks that path first.

> [!NOTE]
> Make sure download and place data folders inside /3ds/EBookReaderFor3DS
> You can place books folders anywhere you prefer or if you already have folder for epub file then inside app you can browser to it

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

```sh
python3 tools/split_epub.py book.epub out_dir/
```

> [!WARNING]
> It may load the file up to 12mb (hmm i guess) but it make loading time long as fk so well i highly recomment you use file around 5mb it will take 20-30s well maybe more or less (i use emulator only) still long as fk but acceptable and it loading screen i hope you will few it less longer

## Control
-
-
-
-
-
-
-
-
-
-
-
-
-
-
- It not black text in black screen a white text in black screen so don't worry if you not see anything, i'm just too lazy (oh and don't follow the guide in the start screen oh and if you wonder why the start screen missing in the top oh why it feel so blank then yeah it can load image if i not mistake that image should name `rena.png` and have to place in `/3ds/EBookReaderFor3DS/data/` why not in `start_menu_image`folder or something like that? well lazy as fk)
