#!/usr/bin/env python3
"""Extract a glibc width profile into C initializers.

OFFLINE TOOL, NOT A BUILD DEPENDENCY. Run this once when adding or refreshing
a profile; the emitted arrays are checked in. Validating the result against a
real glibc is win32/test_wcwidth_oracle.cc's job, not this script's.

    python3 win32/extract_glibc_profile.py CHARMAP [--label NAME] [--manifest FILE] [--check FILE]

Every failure is fatal: a codepoint out of range, a range with hi < lo, an
overlap, a width outside the permitted domain, an unparseable line, an empty
or unterminated section, a stray end marker, or a count that disagrees with
the manifest all raise. Nothing is silently clipped or skipped -- a
quietly-truncated profile would corrupt the screen in exactly the way this
work exists to fix.

PROVENANCE. The data is read from the glibc charmap, but the charmap's own
WIDTH header records that it was generated from EastAsianWidth.txt,
PropList.txt, and UnicodeData.txt. So the data is doubly sourced: glibc
(LGPL-2.1-or-later) for the artifact, Unicode (Unicode License V3) for the
underlying data. Both notices must travel with the vendored file; see the
header template in the implementation plan.

SEPARATORS DIFFER BY SECTION: WIDTH uses THREE dots between range endpoints
(<U0300>...<U036F>) while CHARMAP uses TWO (<U3400>..<U343F>). A pattern that
assumes one form silently drops hundreds of lines from the other, so the
counts are validated rather than trusted.
"""
import argparse
import json
import re
import sys

MAX_CP = 0x110000

# In the repertoire, but excluded from the width table by localedef.
INVARIANT_UNPRINTABLE = [(0x0000, 0x001F), (0x007F, 0x009F), (0x2028, 0x2029)]

WIDTH_BEGIN = "/* BEGIN VENDORED WIDTH"
WIDTH_END = "/* END VENDORED WIDTH. */"
UNPRINT_BEGIN = "/* BEGIN VENDORED UNPRINTABLE"
UNPRINT_END = "/* END VENDORED UNPRINTABLE. */"
DEFAULT_LABEL = "glibc 2.39"

CODRANGE = re.compile(r"^<U([0-9A-Fa-f]+)>(?:\.{2,3}<U([0-9A-Fa-f]+)>)?")

WIDTH_DOMAIN = (0, 2)


class ExtractError(Exception):
    pass


def read_sections(path):
    """Return (width_lines, charmap_lines), comments and blanks removed."""
    width, charmap, section = [], [], None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if line.startswith("CHARMAP"):
                if section is not None:
                    raise ExtractError(f"{path}:{lineno}: nested CHARMAP section")
                section = "charmap"
                continue
            if line.startswith("WIDTH"):
                if section is not None:
                    raise ExtractError(f"{path}:{lineno}: nested WIDTH section")
                section = "width"
                continue
            if line.startswith("END CHARMAP"):
                if section != "charmap":
                    raise ExtractError(
                        f"{path}:{lineno}: END CHARMAP outside a CHARMAP section")
                section = None
                continue
            if line.startswith("END WIDTH"):
                if section != "width":
                    raise ExtractError(
                        f"{path}:{lineno}: END WIDTH outside a WIDTH section")
                section = None
                continue
            if not line or line[0] in "%#" or section is None:
                continue
            (width if section == "width" else charmap).append((lineno, line))
    if section is not None:
        raise ExtractError(f"{path}: {section.upper()} section unterminated at end of file")
    return width, charmap


def parse_span(lineno, line, path):
    m = CODRANGE.match(line)
    # A span must be followed by whitespace or end of line: an unanchored
    # pattern silently truncates "<U0010FFC0>.<U0010FFFD>" to its singleton
    # prefix and discards the rest of the range.
    if not m or (m.end() < len(line) and not line[m.end()].isspace()):
        raise ExtractError(f"{path}:{lineno}: unparseable line: {line!r}")
    lo = int(m.group(1), 16)
    hi = int(m.group(2), 16) if m.group(2) else lo
    if hi < lo:
        raise ExtractError(f"{path}:{lineno}: reversed range U+{lo:04X}..U+{hi:04X}")
    if lo < 0 or hi >= MAX_CP:
        raise ExtractError(
            f"{path}:{lineno}: range U+{lo:04X}..U+{hi:04X} is outside U+0000..U+10FFFF")
    return lo, hi, m.end()


def parse_widths(lines, path):
    out = []
    for lineno, line in lines:
        lo, hi, end = parse_span(lineno, line, path)
        try:
            w = int(line[end:].strip())
        except ValueError:
            raise ExtractError(f"{path}:{lineno}: unparseable WIDTH value in {line!r}")
        if w not in WIDTH_DOMAIN:
            raise ExtractError(
                f"{path}:{lineno}: width {w} outside {WIDTH_DOMAIN} for U+{lo:04X}..U+{hi:04X}")
        out.append((lo, hi, w))
    out.sort()
    for i in range(1, len(out)):
        if out[i][0] <= out[i - 1][1]:
            raise ExtractError(
                f"{path}: overlapping or unsorted WIDTH ranges at "
                f"U+{out[i-1][0]:04X}..U+{out[i-1][1]:04X} and U+{out[i][0]:04X}..U+{out[i][1]:04X}")
    return out


def parse_repertoire(lines, path):
    present = bytearray(MAX_CP)
    spans = []
    for lineno, line in lines:
        lo, hi, _ = parse_span(lineno, line, path)
        spans.append((lo, hi))
    spans.sort()
    for i in range(1, len(spans)):
        if spans[i][0] <= spans[i - 1][1]:
            raise ExtractError(
                f"{path}: overlapping or unsorted CHARMAP ranges at "
                f"U+{spans[i-1][0]:04X}..U+{spans[i-1][1]:04X} and "
                f"U+{spans[i][0]:04X}..U+{spans[i][1]:04X}")
    for lo, hi in spans:
        for cp in range(lo, hi + 1):
            present[cp] = 1
    return present


def unprintable_intervals(present):
    mask = bytearray(present)
    for lo, hi in INVARIANT_UNPRINTABLE:
        for cp in range(lo, hi + 1):
            mask[cp] = 0
    out, start, cur = [], None, None
    for cp in range(MAX_CP):
        v = 0 if mask[cp] else 1
        if v != cur:
            if cur == 1:
                out.append((start, cp - 1))
            cur, start = v, cp
    if cur == 1:
        out.append((start, MAX_CP - 1))
    return out


def cross_check_disjoint(widths, unprintable):
    """The runtime relies on the two arrays being disjoint."""
    i = j = 0
    while i < len(widths) and j < len(unprintable):
        w, u = widths[i], unprintable[j]
        if w[1] < u[0]:
            i += 1
        elif u[1] < w[0]:
            j += 1
        else:
            raise ExtractError(
                f"WIDTH U+{w[0]:04X}..U+{w[1]:04X} overlaps "
                f"UNPRINTABLE U+{u[0]:04X}..U+{u[1]:04X}")


def check_manifest(counts, manifest_path):
    with open(manifest_path, encoding="utf-8") as fh:
        man = json.load(fh)
    for key in ("width_entries", "unprintable_intervals"):
        if key not in man:
            raise ExtractError(f"{manifest_path}: missing key {key!r}")
        if man[key] != counts[key]:
            raise ExtractError(
                f"{manifest_path}: {key} is {man[key]}, extractor produced {counts[key]}")
    return man


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("charmap")
    ap.add_argument("--label", default=DEFAULT_LABEL,
                    help=f"profile name written into the markers (default: {DEFAULT_LABEL!r})")
    ap.add_argument("--manifest", metavar="FILE",
                    help="JSON file with expected counts to validate against")
    ap.add_argument("--check", metavar="FILE",
                    help="compare generated output with FILE instead of printing")
    args = ap.parse_args()

    try:
        with open(args.charmap, encoding="utf-8", errors="replace") as fh:
            if "<code_set_name> UTF-8" not in fh.read(200):
                raise ExtractError(f"{args.charmap}: does not look like a glibc UTF-8 charmap")

        width_lines, charmap_lines = read_sections(args.charmap)
        if not width_lines:
            raise ExtractError(f"{args.charmap}: no WIDTH section")
        if not charmap_lines:
            raise ExtractError(f"{args.charmap}: no CHARMAP section")

        widths = parse_widths(width_lines, args.charmap)
        present = parse_repertoire(charmap_lines, args.charmap)
        unprintable = unprintable_intervals(present)
        cross_check_disjoint(widths, unprintable)

        counts = {"width_entries": len(widths), "unprintable_intervals": len(unprintable)}
        if args.manifest:
            check_manifest(counts, args.manifest)
    except ExtractError as exc:
        sys.stderr.write(f"extract_glibc_profile: {exc}\n")
        return 1

    body = [f"{WIDTH_BEGIN}: {args.label} */"]
    body += [f"    {{ 0x{lo:05x}, 0x{hi:05x}, {w} }}," for lo, hi, w in widths]
    body.append(WIDTH_END)
    body.append(f"{UNPRINT_BEGIN}: {args.label} */")
    body += [f"    {{ 0x{lo:05x}, 0x{hi:05x}, -1 }}," for lo, hi in unprintable]
    body.append(UNPRINT_END)
    text = "\n".join(body) + "\n"

    print(f"{counts['width_entries']} WIDTH entries, "
          f"{counts['unprintable_intervals']} unprintable intervals, "
          f"{sum(present)} repertoire codepoints", file=sys.stderr)

    if args.check:
        try:
            with open(args.check, encoding="utf-8") as fh:
                existing = fh.read()
            wb = existing.index(WIDTH_BEGIN)
            we = existing.index(WIDTH_END, wb) + len(WIDTH_END)
            ub = existing.index(UNPRINT_BEGIN)
            ue = existing.index(UNPRINT_END, ub) + len(UNPRINT_END)
        except (OSError, ValueError) as exc:
            sys.stderr.write(f"extract_glibc_profile: {args.check}: {exc}\n")
            return 1
        # Compare block by block: a real header separates them with array
        # boilerplate, so one slice across both would report false drift.
        head, tail = text.split(UNPRINT_BEGIN)
        want_w = head.strip()
        want_u = (UNPRINT_BEGIN + tail).strip()
        got_w = existing[wb:we].strip()
        got_u = existing[ub:ue].strip()
        if got_w != want_w or got_u != want_u:
            sys.stderr.write(f"{args.check}: vendored arrays differ from the charmap\n")
            if got_w != want_w:
                sys.stderr.write("  the WIDTH block differs\n")
            if got_u != want_u:
                sys.stderr.write("  the UNPRINTABLE block differs\n")
            return 1
        print(f"{args.check}: vendored arrays match the charmap", file=sys.stderr)
        return 0

    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
