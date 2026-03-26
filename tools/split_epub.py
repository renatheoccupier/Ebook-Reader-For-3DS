#!/usr/bin/env python3

import argparse
import copy
from dataclasses import dataclass
import html
import os
import pathlib
import re
import shutil
import uuid
import xml.etree.ElementTree as ET
import zipfile


CONTAINER_NS = "urn:oasis:names:tc:opendocument:xmlns:container"
OPF_NS = "http://www.idpf.org/2007/opf"
DC_NS = "http://purl.org/dc/elements/1.1/"
XHTML_NS = "http://www.w3.org/1999/xhtml"
EPUB_NS = "http://www.idpf.org/2007/ops"
NCX_NS = "http://www.daisy.org/z3986/2005/ncx/"
COPY_CHUNK_SIZE = 1024 * 1024
TEXT_MEDIA_TYPES = {
    "application/xhtml+xml",
    "text/html",
    "text/css",
    "image/svg+xml",
    "application/x-dtbncx+xml",
}
URL_PATTERNS = [
    re.compile(r'''(?:href|src)=["']([^"'#]+(?:#[^"']*)?)["']''', re.IGNORECASE),
    re.compile(r"""url\((['"]?)([^)'"]+)\1\)""", re.IGNORECASE),
]
CONTENTS_TITLES = {"mục lục", "muc luc", "contents", "table of contents", "toc", "navigation"}
CONTENTS_FILES = {"contents.xhtml", "contents.html", "toc.xhtml", "toc.html", "nav.xhtml", "mucluc.html"}


ET.register_namespace("", OPF_NS)
ET.register_namespace("dc", DC_NS)
ET.register_namespace("epub", EPUB_NS)


@dataclass
class TocNode:
    title: str
    href: str
    children: list["TocNode"]


@dataclass
class SplitGroup:
    title: str
    href: str
    start_id: str
    toc: TocNode


@dataclass
class ManifestItem:
    item_id: str
    href: str
    media_type: str
    element: ET.Element
    zip_path: str


@dataclass
class BookContext:
    source: pathlib.Path
    rootfile: str
    opf_dir: str
    version: str
    package: ET.Element
    metadata: ET.Element
    manifest: ET.Element
    spine: ET.Element
    guide: ET.Element | None
    bindings: ET.Element | None
    manifest_items: dict[str, ManifestItem]
    manifest_by_zip_path: dict[str, ManifestItem]
    spine_itemrefs: list[ET.Element]
    spine_ids: list[str]
    cover_id: str | None
    title: str
    nav_item: ManifestItem | None
    ncx_item: ManifestItem | None


def positive_int(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return number


def normalize_zip_path(path):
    parts = []
    for part in pathlib.PurePosixPath(path).parts:
        if part in ("", "."):
            continue
        if part == "..":
            if parts:
                parts.pop()
            continue
        parts.append(part)
    return "/".join(parts)


def resolve_href(base_path, href):
    if not href:
        return ""
    href = href.split("#", 1)[0]
    if not href:
        return ""
    return normalize_zip_path(str(pathlib.PurePosixPath(base_path).parent / href))


def relative_href(from_path, to_path):
    from_dir = pathlib.PurePosixPath(from_path).parent
    return pathlib.PurePosixPath(os.path.relpath(to_path, start=from_dir)).as_posix()


def decode_text(data):
    for encoding in ("utf-8", "utf-8-sig", "cp1252", "latin-1"):
        try:
            return data.decode(encoding)
        except UnicodeDecodeError:
            continue
    return data.decode("utf-8", errors="replace")


def read_xml(zf, name):
    return ET.fromstring(zf.read(name))


def find_rootfile(zf):
    container = read_xml(zf, "META-INF/container.xml")
    rootfile = container.find(f".//{{{CONTAINER_NS}}}rootfile")
    if rootfile is None:
        raise SystemExit("container.xml does not declare a rootfile")
    full_path = rootfile.get("full-path")
    if not full_path:
        raise SystemExit("rootfile is missing full-path")
    return normalize_zip_path(full_path)


def build_context(source):
    with zipfile.ZipFile(source, "r") as zf:
        rootfile = find_rootfile(zf)
        package = read_xml(zf, rootfile)

    metadata = package.find(f"{{{OPF_NS}}}metadata")
    manifest = package.find(f"{{{OPF_NS}}}manifest")
    spine = package.find(f"{{{OPF_NS}}}spine")
    if metadata is None or manifest is None or spine is None:
        raise SystemExit(f"{source} is missing OPF metadata, manifest, or spine")

    guide = package.find(f"{{{OPF_NS}}}guide")
    bindings = package.find(f"{{{OPF_NS}}}bindings")
    opf_dir = pathlib.PurePosixPath(rootfile).parent.as_posix()
    if opf_dir == ".":
        opf_dir = ""

    manifest_items = {}
    manifest_by_zip_path = {}
    nav_item = None
    ncx_item = None
    for item in manifest.findall(f"{{{OPF_NS}}}item"):
        item_id = item.get("id")
        href = item.get("href")
        media_type = item.get("media-type")
        if not item_id or not href or not media_type:
            continue
        zip_path = normalize_zip_path(str(pathlib.PurePosixPath(opf_dir) / href))
        manifest_item = ManifestItem(item_id, href, media_type, item, zip_path)
        manifest_items[item_id] = manifest_item
        manifest_by_zip_path[zip_path] = manifest_item
        properties = item.get("properties", "")
        if "nav" in properties.split():
            nav_item = manifest_item

    toc_id = spine.get("toc")
    if toc_id:
        ncx_item = manifest_items.get(toc_id)
    if ncx_item is None:
        for item in manifest_items.values():
            if item.media_type == "application/x-dtbncx+xml":
                ncx_item = item
                break

    spine_itemrefs = spine.findall(f"{{{OPF_NS}}}itemref")
    spine_ids = [itemref.get("idref") for itemref in spine_itemrefs if itemref.get("idref")]

    cover_id = None
    cover_meta = metadata.find(f"{{{OPF_NS}}}meta[@name='cover']")
    if cover_meta is not None:
        cover_id = cover_meta.get("content")
    if cover_id is None:
        for item in manifest_items.values():
            if "cover-image" in item.element.get("properties", "").split():
                cover_id = item.item_id
                break

    title_node = metadata.find(f"{{{DC_NS}}}title")
    title = title_node.text.strip() if title_node is not None and title_node.text else source.stem

    return BookContext(
        source=source,
        rootfile=rootfile,
        opf_dir=opf_dir,
        version=package.get("version", "2.0"),
        package=package,
        metadata=metadata,
        manifest=manifest,
        spine=spine,
        guide=guide,
        bindings=bindings,
        manifest_items=manifest_items,
        manifest_by_zip_path=manifest_by_zip_path,
        spine_itemrefs=spine_itemrefs,
        spine_ids=spine_ids,
        cover_id=cover_id,
        title=title,
        nav_item=nav_item,
        ncx_item=ncx_item,
    )


def toc_text(element):
    return " ".join(part.strip() for part in element.itertext() if part.strip())


def parse_nav_nodes(nav_root, nav_zip_path):
    toc_nav = None
    for nav in nav_root.findall(f".//{{{XHTML_NS}}}nav"):
        nav_type = nav.get(f"{{{EPUB_NS}}}type") or nav.get("type") or ""
        if "toc" in nav_type.split():
            toc_nav = nav
            break
    if toc_nav is None:
        raise SystemExit("EPUB3 navigation document does not contain a TOC nav")

    top_ol = toc_nav.find(f"{{{XHTML_NS}}}ol")
    if top_ol is None:
        raise SystemExit("EPUB3 navigation document does not contain a top-level list")

    def parse_li(li):
        anchor = li.find(f"{{{XHTML_NS}}}a")
        if anchor is None or not anchor.get("href"):
            return None
        title = toc_text(anchor)
        href = resolve_href(nav_zip_path, anchor.get("href"))
        child_ol = li.find(f"{{{XHTML_NS}}}ol")
        children = []
        if child_ol is not None:
            for child_li in child_ol.findall(f"{{{XHTML_NS}}}li"):
                child = parse_li(child_li)
                if child:
                    children.append(child)
        return TocNode(title, href, children)

    nodes = []
    for li in top_ol.findall(f"{{{XHTML_NS}}}li"):
        node = parse_li(li)
        if node:
            nodes.append(node)
    return nodes


def parse_ncx_nodes(ncx_root, ncx_zip_path):
    nav_map = ncx_root.find(f"{{{NCX_NS}}}navMap")
    if nav_map is None:
        raise SystemExit("NCX file does not contain navMap")

    def parse_navpoint(navpoint):
        label = navpoint.find(f"{{{NCX_NS}}}navLabel/{{{NCX_NS}}}text")
        content = navpoint.find(f"{{{NCX_NS}}}content")
        if label is None or content is None or not content.get("src"):
            return None
        title = (label.text or "").strip()
        href = resolve_href(ncx_zip_path, content.get("src"))
        children = []
        for child_navpoint in navpoint.findall(f"{{{NCX_NS}}}navPoint"):
            child = parse_navpoint(child_navpoint)
            if child:
                children.append(child)
        return TocNode(title, href, children)

    nodes = []
    for navpoint in nav_map.findall(f"{{{NCX_NS}}}navPoint"):
        node = parse_navpoint(navpoint)
        if node:
            nodes.append(node)
    return nodes


def is_contents_like(title, href):
    normalized_title = " ".join(title.lower().split())
    file_name = pathlib.PurePosixPath(href).name.lower()
    if normalized_title in CONTENTS_TITLES:
        return True
    if file_name in CONTENTS_FILES:
        return True
    return False


def select_groups(context):
    with zipfile.ZipFile(context.source, "r") as zf:
        if context.nav_item:
            nodes = parse_nav_nodes(read_xml(zf, context.nav_item.zip_path), context.nav_item.zip_path)
        elif context.ncx_item:
            nodes = parse_ncx_nodes(read_xml(zf, context.ncx_item.zip_path), context.ncx_item.zip_path)
        else:
            raise SystemExit(f"{context.source} does not contain a navigation document")

    start_nodes = []
    for node in nodes:
        manifest_item = context.manifest_by_zip_path.get(node.href)
        if not manifest_item or manifest_item.item_id not in context.spine_ids:
            continue
        if is_contents_like(node.title, node.href):
            continue
        start_nodes.append((node, manifest_item.item_id))

    if len(start_nodes) < 2:
        raise SystemExit(
            f"{context.source} does not contain multiple top-level TOC sections that can be split safely"
        )

    spine_index = {item_id: index for index, item_id in enumerate(context.spine_ids)}
    groups = []
    for index, (node, item_id) in enumerate(start_nodes):
        start = spine_index[item_id]
        end = spine_index[start_nodes[index + 1][1]] if index + 1 < len(start_nodes) else len(context.spine_ids)
        if start >= end:
            continue
        groups.append(
            SplitGroup(
                title=node.title,
                href=node.href,
                start_id=item_id,
                toc=node,
            )
        )
    return groups


def extract_document_title(text, fallback):
    title_match = re.search(r"<title[^>]*>(.*?)</title>", text, re.IGNORECASE | re.DOTALL)
    if title_match:
        title = re.sub(r"\s+", " ", html.unescape(title_match.group(1))).strip()
        if title:
            return title
    heading_match = re.search(r"<h[1-6][^>]*>(.*?)</h[1-6]>", text, re.IGNORECASE | re.DOTALL)
    if heading_match:
        title = re.sub(r"<[^>]+>", "", heading_match.group(1))
        title = re.sub(r"\s+", " ", html.unescape(title)).strip()
        if title:
            return title
    return fallback


def referenced_paths(context, zf, start_zip_paths):
    queue = list(start_zip_paths)
    visited = set()
    collected = set()
    while queue:
        zip_path = queue.pop()
        if zip_path in visited:
            continue
        visited.add(zip_path)
        manifest_item = context.manifest_by_zip_path.get(zip_path)
        if manifest_item is None:
            continue
        collected.add(zip_path)
        if manifest_item.media_type not in TEXT_MEDIA_TYPES:
            continue
        text = decode_text(zf.read(zip_path))
        for pattern in URL_PATTERNS:
            for match in pattern.findall(text):
                raw_href = match[1] if isinstance(match, tuple) else match
                href = raw_href.strip()
                if not href or ":" in href.split("/", 1)[0] or href.startswith(("#", "data:")):
                    continue
                resolved = resolve_href(zip_path, href)
                if resolved and resolved not in visited:
                    queue.append(resolved)
    return collected


def copy_entry(zf, dst, name):
    info = zf.getinfo(name)
    clone = zipfile.ZipInfo(info.filename, date_time=info.date_time)
    clone.comment = info.comment
    clone.extra = info.extra
    clone.create_system = info.create_system
    clone.create_version = info.create_version
    clone.extract_version = info.extract_version
    clone.flag_bits = info.flag_bits
    clone.external_attr = info.external_attr
    clone.internal_attr = info.internal_attr
    clone.compress_type = zipfile.ZIP_STORED if info.filename == "mimetype" else zipfile.ZIP_DEFLATED
    if info.is_dir():
        dst.writestr(clone, b"")
        return
    with zf.open(info, "r") as src_handle, dst.open(clone, "w") as dst_handle:
        shutil.copyfileobj(src_handle, dst_handle, length=COPY_CHUNK_SIZE)


def sanitize_filename(value):
    value = re.sub(r"[\\/:*?\"<>|]+", "_", value)
    value = re.sub(r"\s+", " ", value).strip().strip(".")
    return value or "split"


def make_book_title(source_title, group_title):
    source_lower = source_title.casefold()
    group_lower = group_title.casefold()
    if source_lower in group_lower:
        return group_title
    return f"{source_title} - {group_title}"


def build_nav_document(title, book_title, entries):
    lines = [
        "<?xml version='1.0' encoding='utf-8'?>",
        '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">',
        "  <head>",
        f"    <title>{html.escape(title)}</title>",
        '    <meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>',
        "  </head>",
        "  <body>",
        '    <nav epub:type="toc" id="toc">',
        f"      <h1>{html.escape(book_title)}</h1>",
        "      <ol>",
    ]

    def render(nodes, indent):
        for node in nodes:
            lines.append(f"{indent}<li><a href=\"{html.escape(node.href)}\">{html.escape(node.title)}</a>")
            if node.children:
                lines.append(f"{indent}  <ol>")
                render(node.children, indent + "    ")
                lines.append(f"{indent}  </ol>")
            lines.append(f"{indent}</li>")

    render(entries, "        ")
    lines.extend(
        [
            "      </ol>",
            "    </nav>",
            "  </body>",
            "</html>",
        ]
    )
    return "\n".join(lines) + "\n"


def build_ncx_document(identifier, book_title, entries):
    lines = [
        "<?xml version='1.0' encoding='utf-8'?>",
        '<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN" '
        '"http://www.daisy.org/z3986/2005/ncx-2005-1.dtd">',
        '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">',
        "  <head>",
        f'    <meta name="dtb:uid" content="{html.escape(identifier)}"/>',
        '    <meta name="dtb:depth" content="1"/>',
        '    <meta name="dtb:totalPageCount" content="0"/>',
        '    <meta name="dtb:maxPageNumber" content="0"/>',
        "  </head>",
        "  <docTitle>",
        f"    <text>{html.escape(book_title)}</text>",
        "  </docTitle>",
        "  <navMap>",
    ]

    play_order = 1

    def render(nodes, indent):
        nonlocal play_order
        for index, node in enumerate(nodes, start=1):
            current_order = play_order
            play_order += 1
            lines.append(f'{indent}<navPoint id="nav{current_order}" playOrder="{current_order}">')
            lines.append(f"{indent}  <navLabel>")
            lines.append(f"{indent}    <text>{html.escape(node.title)}</text>")
            lines.append(f"{indent}  </navLabel>")
            lines.append(f'{indent}  <content src="{html.escape(node.href)}"/>')
            if node.children:
                render(node.children, indent + "  ")
            lines.append(f"{indent}</navPoint>")

    render(entries, "    ")
    lines.extend(
        [
            "  </navMap>",
            "</ncx>",
        ]
    )
    return "\n".join(lines) + "\n"


def relative_toc_node(node, base_path):
    return TocNode(
        node.title,
        relative_href(base_path, node.href),
        [relative_toc_node(child, base_path) for child in node.children],
    )


def collect_section_entries(context, zf, selected_ids, group, nav_zip_path):
    entries = []
    selected_zip_paths = {context.manifest_items[item_id].zip_path for item_id in selected_ids}

    def within_group(node):
        return node.href in selected_zip_paths

    root = group.toc
    if root.children:
        child_entries = []
        for child in root.children:
            if within_group(child):
                child_entries.append(relative_toc_node(child, nav_zip_path))
        if child_entries:
            return [TocNode(group.toc.title, relative_href(nav_zip_path, group.href), child_entries)]

    for item_id in selected_ids:
        manifest_item = context.manifest_items[item_id]
        if manifest_item.media_type not in {"application/xhtml+xml", "text/html"}:
            continue
        text = decode_text(zf.read(manifest_item.zip_path))
        entries.append(
            TocNode(
                extract_document_title(text, pathlib.PurePosixPath(manifest_item.href).stem),
                relative_href(nav_zip_path, manifest_item.zip_path),
                [],
            )
        )
    return entries


def update_metadata(metadata, package, title):
    title_node = metadata.find(f"{{{DC_NS}}}title")
    if title_node is None:
        title_node = ET.SubElement(metadata, f"{{{DC_NS}}}title")
    title_node.text = title

    package_uuid = f"urn:uuid:{uuid.uuid4()}"
    unique_identifier_id = package.get("unique-identifier")
    identifier_nodes = metadata.findall(f"{{{DC_NS}}}identifier")
    target_node = None
    if unique_identifier_id:
        for node in identifier_nodes:
            if node.get("id") == unique_identifier_id:
                target_node = node
                break
    if target_node is None and identifier_nodes:
        target_node = identifier_nodes[0]
    if target_node is None:
        identifier = ET.SubElement(metadata, f"{{{DC_NS}}}identifier")
        if unique_identifier_id:
            identifier.set("id", unique_identifier_id)
        target_node = identifier
    target_node.text = package_uuid
    return package_uuid


def split_one_epub(source, output_dir, limit=None):
    context = build_context(source)
    groups = select_groups(context)
    output_dir.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(source, "r") as zf:
        split_paths = []
        spine_index = {item_id: index for index, item_id in enumerate(context.spine_ids)}
        real_start_ids = [group.start_id for group in groups]
        real_starts = [spine_index[item_id] for item_id in real_start_ids]
        emit_count = len(groups) if limit is None else min(limit, len(groups))

        for group_index in range(emit_count):
            group = groups[group_index]
            start = real_starts[group_index]
            end = real_starts[group_index + 1] if group_index + 1 < len(real_starts) else len(context.spine_ids)
            selected_ids = context.spine_ids[start:end]
            selected_zip_paths = {context.manifest_items[item_id].zip_path for item_id in selected_ids}

            if context.cover_id and context.cover_id in context.manifest_items:
                selected_zip_paths.add(context.manifest_items[context.cover_id].zip_path)

            asset_paths = referenced_paths(context, zf, selected_zip_paths)
            included_ids = {
                context.manifest_by_zip_path[path].item_id
                for path in asset_paths
                if path in context.manifest_by_zip_path
            }
            included_ids.update(selected_ids)
            if context.cover_id:
                included_ids.add(context.cover_id)

            split_title = make_book_title(context.title, group.title)
            nav_zip_path = normalize_zip_path(
                str(pathlib.PurePosixPath(context.opf_dir) / "nav.xhtml")
            )
            ncx_zip_path = normalize_zip_path(
                str(pathlib.PurePosixPath(context.opf_dir) / "toc.ncx")
            )
            nav_entries = collect_section_entries(context, zf, selected_ids, group, nav_zip_path)

            package = ET.Element(f"{{{OPF_NS}}}package", context.package.attrib)
            metadata = copy.deepcopy(context.metadata)
            identifier = update_metadata(metadata, package, split_title)
            package.append(metadata)

            manifest = ET.SubElement(package, f"{{{OPF_NS}}}manifest")
            for item_id in context.manifest_items:
                if item_id in included_ids:
                    manifest.append(copy.deepcopy(context.manifest_items[item_id].element))

            nav_id = "split_nav"
            nav_href = "nav.xhtml"
            ET.SubElement(
                manifest,
                f"{{{OPF_NS}}}item",
                {
                    "id": nav_id,
                    "href": nav_href,
                    "media-type": "application/xhtml+xml",
                    "properties": "nav",
                },
            )

            ncx_id = "split_ncx"
            ncx_href = "toc.ncx"
            if context.version.startswith("2"):
                ET.SubElement(
                    manifest,
                    f"{{{OPF_NS}}}item",
                    {
                        "id": ncx_id,
                        "href": ncx_href,
                        "media-type": "application/x-dtbncx+xml",
                    },
                )

            spine_attrs = dict(context.spine.attrib)
            if context.version.startswith("2"):
                spine_attrs["toc"] = ncx_id
            else:
                spine_attrs.pop("toc", None)
            spine = ET.SubElement(package, f"{{{OPF_NS}}}spine", spine_attrs)
            selected_id_set = set(selected_ids)
            for itemref in context.spine_itemrefs:
                idref = itemref.get("idref")
                if idref in selected_id_set:
                    spine.append(copy.deepcopy(itemref))

            if context.guide is not None:
                guide = copy.deepcopy(context.guide)
                for reference in list(guide):
                    href = reference.get("href", "")
                    zip_path = normalize_zip_path(str(pathlib.PurePosixPath(context.opf_dir) / href.split("#", 1)[0]))
                    if zip_path and zip_path not in asset_paths:
                        guide.remove(reference)
                if list(guide):
                    package.append(guide)

            if context.bindings is not None and list(context.bindings):
                package.append(copy.deepcopy(context.bindings))

            output_name = sanitize_filename(f"{source.stem} - {group.title}") + ".epub"
            output_path = output_dir / output_name

            nav_content = build_nav_document(split_title, split_title, nav_entries)
            ncx_content = build_ncx_document(identifier, split_title, nav_entries)

            with zipfile.ZipFile(
                output_path,
                "w",
                compression=zipfile.ZIP_DEFLATED,
                compresslevel=9,
            ) as dst:
                if "mimetype" in zf.namelist():
                    dst.writestr("mimetype", zf.read("mimetype"), compress_type=zipfile.ZIP_STORED)
                for name in sorted(n for n in zf.namelist() if n.startswith("META-INF/") and n != "META-INF/container.xml"):
                    copy_entry(zf, dst, name)
                dst.writestr("META-INF/container.xml", zf.read("META-INF/container.xml"))

                for item_id in included_ids:
                    manifest_item = context.manifest_items.get(item_id)
                    if manifest_item is None:
                        continue
                    copy_entry(zf, dst, manifest_item.zip_path)

                if context.opf_dir:
                    dst.writestr(f"{context.opf_dir}/{nav_href}", nav_content.encode("utf-8"))
                    if context.version.startswith("2"):
                        dst.writestr(f"{context.opf_dir}/{ncx_href}", ncx_content.encode("utf-8"))
                    dst.writestr(
                        context.rootfile,
                        ET.tostring(package, encoding="utf-8", xml_declaration=True),
                    )
                else:
                    dst.writestr(nav_href, nav_content.encode("utf-8"))
                    if context.version.startswith("2"):
                        dst.writestr(ncx_href, ncx_content.encode("utf-8"))
                    dst.writestr(
                        context.rootfile,
                        ET.tostring(package, encoding="utf-8", xml_declaration=True),
                    )

            split_paths.append(output_path)

    return split_paths


def collect_epubs(source_dir):
    matches = []
    for root, _, filenames in os.walk(source_dir):
        for filename in filenames:
            path = pathlib.Path(root) / filename
            if path.suffix.lower() == ".epub":
                matches.append(path)
    return sorted(matches)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Split omnibus EPUB files into smaller valid EPUBs using top-level TOC sections.",
        epilog=(
            "examples:\n"
            "  split_epub.py input.epub output_dir\n"
            "  split_epub.py --input-dir books --output-dir split_books"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("input", nargs="?", help="source EPUB file")
    parser.add_argument("output", nargs="?", help="output folder for split EPUB files")
    parser.add_argument("--input-dir", help="folder scanned recursively for .epub files")
    parser.add_argument("--output-dir", help="output folder for folder mode")
    parser.add_argument("--limit", type=positive_int, help="split only the first N sections from each EPUB")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.input_dir:
        if args.input or args.output:
            raise SystemExit("Use either positional input/output or --input-dir/--output-dir.")
        if not args.output_dir:
            raise SystemExit("Provide --output-dir when using --input-dir.")
        source_dir = pathlib.Path(args.input_dir)
        if not source_dir.is_dir():
            raise SystemExit(f"missing input folder: {source_dir}")
        output_dir = pathlib.Path(args.output_dir)
        epubs = collect_epubs(source_dir)
        if not epubs:
            raise SystemExit(f"no EPUB files found in {source_dir}")
        for source in epubs:
            relative_parent = source.parent.relative_to(source_dir)
            target_dir = output_dir / relative_parent / source.stem
            split_paths = split_one_epub(source, target_dir, limit=args.limit)
            print(f"{source}: wrote {len(split_paths)} split file(s) to {target_dir}")
    else:
        if not args.input or not args.output:
            raise SystemExit("Provide an input EPUB path and an output folder.")
        source = pathlib.Path(args.input)
        if not source.is_file():
            raise SystemExit(f"missing input file: {source}")
        output_dir = pathlib.Path(args.output)
        split_paths = split_one_epub(source, output_dir, limit=args.limit)
        print(f"{source}: wrote {len(split_paths)} split file(s) to {output_dir}")
        for path in split_paths:
            print(path)


if __name__ == "__main__":
    main()
