#!/usr/bin/env python3

import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass, replace
from functools import lru_cache
import io
import os
import pathlib
import shutil
import sys
import tempfile
import zipfile


IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png"}
JPEG_SUFFIXES = {".jpg", ".jpeg"}
COPY_CHUNK_SIZE = 1024 * 1024
DEFAULT_MIN_QUALITY = 25
DEFAULT_MIN_PNG_COLORS = 16
DEFAULT_QUALITY_STEP = 10
DEFAULT_SCALE_STEP = 0.85


@dataclass(frozen=True)
class OptimizeSettings:
    max_width: int
    max_height: int
    grayscale: bool
    quality: int
    png_colors: int | None
    max_output_bytes: int | None
    min_width: int
    min_height: int
    min_quality: int


@dataclass
class FileResult:
    source: str
    target: str
    converted: int
    skipped: int
    logs: list[str]
    rewritten: bool
    failed: bool = False


@lru_cache(maxsize=1)
def load_pillow():
    try:
        from PIL import Image, ImageOps
    except ImportError as exc:
        raise SystemExit("Pillow is required: python3 -m pip install Pillow") from exc
    return Image, ImageOps


def resample_filter(image_module):
    if hasattr(image_module, "Resampling"):
        return image_module.Resampling.LANCZOS
    return image_module.LANCZOS


def flatten_image(image_module, image, background):
    if "A" not in image.getbands():
        return image
    canvas = image_module.new("RGBA", image.size, background + (255,))
    canvas.alpha_composite(image.convert("RGBA"))
    return canvas


def optimize_image(data, suffix, max_width, max_height, grayscale, quality):
    return optimize_image_with_settings(
        data,
        suffix,
        OptimizeSettings(
            max_width=max_width,
            max_height=max_height,
            grayscale=grayscale,
            quality=quality,
            png_colors=None,
            max_output_bytes=None,
            min_width=max_width,
            min_height=max_height,
            min_quality=quality,
        ),
    )


def optimize_image_with_settings(data, suffix, settings):
    image_module, image_ops = load_pillow()
    with image_module.open(io.BytesIO(data)) as image:
        if getattr(image, "format", None) == "JPEG":
            draft_mode = "L" if settings.grayscale else "RGB"
            image.draft(draft_mode, (settings.max_width, settings.max_height))
        image.load()
        if image.mode == "P":
            image = image.convert("RGBA")
        if "A" in image.getbands():
            background = (0, 0, 0) if settings.grayscale else (255, 255, 255)
            image = flatten_image(image_module, image, background)
        if settings.grayscale:
            image = image_ops.grayscale(image)
        elif image.mode not in ("RGB", "L"):
            image = image.convert("RGB")

        image.thumbnail((settings.max_width, settings.max_height), resample_filter(image_module))
        out = io.BytesIO()
        if suffix in JPEG_SUFFIXES:
            if image.mode != "RGB":
                image = image.convert("RGB")
            image.save(
                out,
                "JPEG",
                quality=settings.quality,
                optimize=True,
                progressive=False,
            )
        else:
            if image.mode not in ("RGB", "L"):
                image = image.convert("RGB")
            if settings.png_colors:
                image = image.convert(
                    "P",
                    palette=image_module.ADAPTIVE,
                    colors=max(DEFAULT_MIN_PNG_COLORS, min(256, settings.png_colors)),
                )
            image.save(out, "PNG", optimize=True)
        return out.getvalue(), image.size


def clone_info(info, compress_type):
    clone = zipfile.ZipInfo(info.filename, date_time=info.date_time)
    clone.compress_type = compress_type
    clone.comment = info.comment
    clone.extra = info.extra
    clone.create_system = info.create_system
    clone.create_version = info.create_version
    clone.extract_version = info.extract_version
    clone.flag_bits = info.flag_bits
    clone.external_attr = info.external_attr
    clone.internal_attr = info.internal_attr
    return clone


def copy_entry(src, dst, info, clone):
    if info.is_dir():
        dst.writestr(clone, b"")
        return
    with src.open(info, "r") as src_handle, dst.open(clone, "w") as dst_handle:
        shutil.copyfileobj(src_handle, dst_handle, length=COPY_CHUNK_SIZE)


def optimize_epub(source, target, settings):
    converted = 0
    skipped = 0
    logs = []
    with zipfile.ZipFile(source, "r") as src, zipfile.ZipFile(
        target,
        "w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
    ) as dst:
        for info in src.infolist():
            suffix = pathlib.Path(info.filename.lower()).suffix
            compress_type = zipfile.ZIP_STORED if info.filename == "mimetype" else zipfile.ZIP_DEFLATED
            clone = clone_info(info, compress_type)
            if not info.is_dir() and suffix in IMAGE_SUFFIXES and info.file_size:
                with src.open(info, "r") as image_file:
                    data = image_file.read()
                try:
                    data, size = optimize_image_with_settings(data, suffix, settings)
                    converted += 1
                    logs.append(f"optimized {info.filename} -> {size[0]}x{size[1]}")
                except Exception as exc:
                    skipped += 1
                    logs.append(f"skipped {info.filename}: {exc}")
                dst.writestr(clone, data)
                continue
            copy_entry(src, dst, info, clone)
    logs.append(f"done: {converted} optimized, {skipped} skipped")
    return converted, skipped, logs


def positive_int(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return number


def positive_float(value):
    number = float(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be greater than 0")
    return number


def png_color_count(value):
    number = int(value)
    if number < 2 or number > 256:
        raise argparse.ArgumentTypeError("must be between 2 and 256")
    return number


def jpeg_quality(value):
    number = int(value)
    if number < 1 or number > 100:
        raise argparse.ArgumentTypeError("must be between 1 and 100")
    return number


def normalize_path(path):
    return pathlib.Path(path).resolve(strict=False)


def is_relative_to(path, base):
    try:
        path.relative_to(base)
        return True
    except ValueError:
        return False


def same_path(path_a, path_b):
    return normalize_path(path_a) == normalize_path(path_b)


def collect_epubs(source_dir, exclude_dir=None):
    source_dir = normalize_path(source_dir)
    exclude_dir = normalize_path(exclude_dir) if exclude_dir else None
    matches = []
    for root, dirnames, filenames in os.walk(source_dir):
        root_path = pathlib.Path(root)
        if exclude_dir:
            dirnames[:] = [
                dirname
                for dirname in dirnames
                if not is_relative_to(root_path / dirname, exclude_dir)
            ]
        for filename in filenames:
            path = root_path / filename
            if path.suffix.lower() != ".epub":
                continue
            if exclude_dir and is_relative_to(path, exclude_dir):
                continue
            matches.append(path)
    return sorted(matches)


def print_result(result):
    prefix = result.source
    for line in result.logs:
        stream = sys.stderr if result.failed or line.startswith("skipped ") else sys.stdout
        print(f"[{prefix}] {line}", file=stream)
    if not result.failed:
        action = "rewrote" if result.rewritten else "wrote"
        print(f"{action} {result.target}")


def format_size(size_bytes):
    if size_bytes >= 1024 * 1024:
        return f"{size_bytes / (1024 * 1024):.2f} MB"
    if size_bytes >= 1024:
        return f"{size_bytes / 1024:.1f} KB"
    return f"{size_bytes} B"


def describe_settings(settings):
    description = [
        f"{settings.max_width}x{settings.max_height}",
        f"quality {settings.quality}",
    ]
    if settings.png_colors:
        description.append(f"png-colors {settings.png_colors}")
    return ", ".join(description)


def next_settings(settings):
    next_quality = settings.quality
    next_png_colors = settings.png_colors
    changed = False

    if settings.quality > settings.min_quality:
        next_quality = max(settings.min_quality, settings.quality - DEFAULT_QUALITY_STEP)
        changed = True

    if settings.png_colors is None and settings.max_output_bytes is not None:
        next_png_colors = 64 if settings.grayscale else 128
        changed = True

    if settings.png_colors and settings.png_colors > DEFAULT_MIN_PNG_COLORS:
        next_png_colors = max(DEFAULT_MIN_PNG_COLORS, settings.png_colors // 2)
        if next_png_colors != settings.png_colors:
            changed = True

    if changed:
        return replace(settings, quality=next_quality, png_colors=next_png_colors)

    next_width = max(settings.min_width, int(round(settings.max_width * DEFAULT_SCALE_STEP)))
    next_height = max(settings.min_height, int(round(settings.max_height * DEFAULT_SCALE_STEP)))
    if next_width == settings.max_width and next_height == settings.max_height:
        return None

    return replace(settings, max_width=next_width, max_height=next_height)


def build_settings(args):
    max_output_bytes = None
    if args.max_output_mb is not None:
        max_output_bytes = int(args.max_output_mb * 1024 * 1024)

    min_width = min(args.max_width, max(64, int(round(args.max_width * 0.6))))
    min_height = min(args.max_height, max(48, int(round(args.max_height * 0.6))))

    return OptimizeSettings(
        max_width=args.max_width,
        max_height=args.max_height,
        grayscale=args.grayscale,
        quality=args.quality,
        png_colors=args.png_colors,
        max_output_bytes=max_output_bytes,
        min_width=min_width,
        min_height=min_height,
        min_quality=min(args.quality, DEFAULT_MIN_QUALITY),
    )


def create_temp_epub(base_path):
    with tempfile.NamedTemporaryFile(
        prefix=base_path.stem + "_",
        suffix=".epub",
        dir=base_path.parent,
        delete=False,
    ) as temp_file:
        return pathlib.Path(temp_file.name)


def optimize_file(source, target, settings):
    source = pathlib.Path(source)
    target = pathlib.Path(target)
    rewritten = same_path(source, target)
    final_target = source if rewritten else target
    if not rewritten:
        final_target.parent.mkdir(parents=True, exist_ok=True)

    current_settings = settings
    attempt = 1
    attempt_logs = []
    temp_path = None
    try:
        while True:
            temp_path = create_temp_epub(final_target)
            converted, skipped, detail_logs = optimize_epub(
                source,
                temp_path,
                current_settings,
            )
            output_size = temp_path.stat().st_size
            attempt_summary = f"attempt {attempt}: {describe_settings(current_settings)} -> {format_size(output_size)}"

            if not current_settings.max_output_bytes or output_size <= current_settings.max_output_bytes:
                temp_path.replace(final_target)
                temp_path = None
                logs = attempt_logs + [attempt_summary] + detail_logs
                if current_settings.max_output_bytes:
                    logs.append(
                        f"within size target: {format_size(output_size)} <= "
                        f"{format_size(current_settings.max_output_bytes)}"
                    )
                return FileResult(
                    str(source),
                    str(final_target),
                    converted,
                    skipped,
                    logs,
                    rewritten,
                )

            if converted == 0:
                temp_path.replace(final_target)
                temp_path = None
                logs = attempt_logs + [attempt_summary] + detail_logs
                logs.append("warning: size target not reached because the EPUB had no optimizable images")
                return FileResult(
                    str(source),
                    str(final_target),
                    converted,
                    skipped,
                    logs,
                    rewritten,
                )

            next_try = next_settings(current_settings)
            if not next_try:
                temp_path.replace(final_target)
                temp_path = None
                logs = attempt_logs + [attempt_summary] + detail_logs
                logs.append(
                    f"warning: could not reach target size {format_size(current_settings.max_output_bytes)}; "
                    f"kept smallest generated file"
                )
                return FileResult(
                    str(source),
                    str(final_target),
                    converted,
                    skipped,
                    logs,
                    rewritten,
                )

            attempt_logs.append(
                f"{attempt_summary} (above target {format_size(current_settings.max_output_bytes)}, retrying)"
            )
            current_settings = next_try
            temp_path.unlink()
            temp_path = None
            attempt += 1
    except Exception as exc:
        if temp_path and temp_path.exists():
            temp_path.unlink()
        if not rewritten and final_target.exists():
            final_target.unlink()
        return FileResult(
            str(source),
            str(final_target),
            0,
            0,
            [f"failed: {exc}"],
            rewritten,
            True,
        )


def build_jobs(args):
    if args.input_dir:
        source_dir = normalize_path(args.input_dir)
        if not source_dir.is_dir():
            raise SystemExit(f"missing input folder: {source_dir}")
        if args.input:
            raise SystemExit("Use either a single input EPUB or --input-dir, not both.")
        if args.output:
            raise SystemExit("Use --output-dir with --input-dir, not a positional output path.")
        if args.in_place and args.output_dir:
            raise SystemExit("Use either --in-place or --output-dir, not both.")
        if not args.in_place and not args.output_dir:
            raise SystemExit("Provide --output-dir for folder input or use --in-place.")

        output_dir = normalize_path(args.output_dir) if args.output_dir else None
        if output_dir and output_dir.exists() and not output_dir.is_dir():
            raise SystemExit(f"output path is not a folder: {output_dir}")
        exclude_dir = None
        if output_dir and is_relative_to(output_dir, source_dir) and output_dir != source_dir:
            exclude_dir = output_dir

        sources = collect_epubs(source_dir, exclude_dir=exclude_dir)
        if not sources:
            raise SystemExit(f"no EPUB files found in {source_dir}")

        if args.in_place:
            return [(source, source) for source in sources]

        return [
            (source, output_dir / source.relative_to(source_dir))
            for source in sources
        ]

    if not args.input:
        raise SystemExit("Provide an input EPUB path or use --input-dir.")
    if args.output_dir:
        raise SystemExit("--output-dir can only be used together with --input-dir.")

    source = pathlib.Path(args.input)
    if not source.is_file():
        raise SystemExit(f"missing input file: {source}")

    if args.in_place:
        if args.output:
            raise SystemExit("Use either --in-place or an explicit output path, not both.")
        return [(source, source)]

    if not args.output:
        raise SystemExit("Provide an output EPUB path or use --in-place.")

    target = pathlib.Path(args.output)
    if target.exists() and target.is_dir():
        target = target / source.name
    return [(source, target)]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Downscale EPUB images for DS/DSi-sized screens.",
        epilog=(
            "examples:\n"
            "  optimize_epub.py book.epub book-optimized.epub --grayscale\n"
            "  optimize_epub.py --input-dir books --output-dir optimized --workers 4\n"
            "  optimize_epub.py --input-dir books --in-place\n"
            "  optimize_epub.py book.epub book-small.epub --max-output-mb 1"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("input", nargs="?", help="source EPUB file")
    parser.add_argument("output", nargs="?", help="output EPUB file")
    parser.add_argument("--input-dir", help="source folder scanned recursively for .epub files")
    parser.add_argument("--output-dir", help="output folder for optimized EPUB files")
    parser.add_argument("--in-place", action="store_true", help="rewrite the source EPUB")
    parser.add_argument("--max-width", type=positive_int, default=250, help="maximum image width inside the EPUB")
    parser.add_argument("--max-height", type=positive_int, default=180, help="maximum image height inside the EPUB")
    parser.add_argument("--grayscale", action="store_true", help="store optimized images in grayscale")
    parser.add_argument("--quality", type=jpeg_quality, default=60, help="JPEG quality for JPEG images")
    parser.add_argument(
        "--png-colors",
        type=png_color_count,
        help="reduce PNG images to an adaptive palette with this many colors",
    )
    parser.add_argument(
        "--max-output-mb",
        type=positive_float,
        help="retry with stronger optimization until the output EPUB is at or below this size",
    )
    parser.add_argument(
        "--workers",
        type=positive_int,
        default=max(1, os.cpu_count() or 1),
        help="worker processes for folder input (default: %(default)s)",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    settings = build_settings(args)
    jobs = build_jobs(args)
    use_pool = args.input_dir and len(jobs) > 1 and args.workers > 1
    results = []

    if use_pool:
        with ProcessPoolExecutor(max_workers=min(args.workers, len(jobs))) as executor:
            futures = {
                executor.submit(
                    optimize_file,
                    str(source),
                    str(target),
                    settings,
                ): (source, target)
                for source, target in jobs
            }
            for future in as_completed(futures):
                result = future.result()
                results.append(result)
                print_result(result)
    else:
        for source, target in jobs:
            result = optimize_file(
                str(source),
                str(target),
                settings,
            )
            results.append(result)
            print_result(result)

    total_files = len(results)
    total_converted = sum(result.converted for result in results)
    total_skipped = sum(result.skipped for result in results)
    total_failed = sum(1 for result in results if result.failed)
    print(
        f"batch complete: {total_files} file(s), "
        f"{total_converted} image(s) optimized, {total_skipped} image(s) skipped, "
        f"{total_failed} file(s) failed"
    )


if __name__ == "__main__":
    main()
