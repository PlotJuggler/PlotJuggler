#!/usr/bin/env python3
"""Download Google Material Symbols SVGs and save them under PJ4 icon names.

The icon mapping is defined inline below as `ICON_MAPPING`. The key is the
target filename (unique); the value is a `(Material Symbol Name, transforms)`
tuple. Transforms is an optional dict supporting:

    rotate=<deg>    Wrap the SVG body in a rotate transform centered on the viewBox.
    hflip=1         Horizontal mirror.
    vflip=1         Vertical mirror.
    fill=0|1        Per-entry override of Material's fill axis.
    fill_color=<#> Per-entry override of the global --fill-color (e.g. "#FFFFFF").

Variant axes (configurable via flags):

    --style         outlined | rounded | sharp     (sharpness; default rounded)
    --weight        100..700                       (default 300)
    --fill          0 | 1                          (default 0; per-entry fill= wins)
    --grade         -25 | 0 | 200                  (default 0)
    --optical-size  20 | 24 | 40 | 48              (default 48)

URL pattern used:

    https://fonts.gstatic.com/s/i/short-term/release/
        materialsymbols{style}/{name}/{variant}/{size}px.svg

where {variant} is "default" if every axis is at its default, otherwise
the non-default axes concatenated as "wght{N}grad{N}fill{N}".

Example:

    python3 scripts/download_material_icons.py \
        --style rounded --optical-size 48 \
        -o resources/svg/

A duplicate Material name (e.g. "Circle" -> green/red, or "Position Top
Right" -> top-right and h-flipped top-left) is fetched once per
(name, variant) pair and written to each target.
"""

from __future__ import annotations

import argparse
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

GSTATIC_URL = (
    "https://fonts.gstatic.com/s/i/short-term/release/"
    "materialsymbols{style}/{name}/{variant}/{size}px.svg"
)


# {target_filename: (material_symbol_name, transforms)}. Keyed by target so
# each output file is unambiguous; the same Material name can appear in
# multiple entries (different transforms / different recolor targets).
ICON_MAPPING: dict[str, tuple[str, dict[str, str]]] = {
    "acute.svg":                           ("Acute",                      {}),
    "add_circle.svg":                      ("Add Circle",                 {}),
    "add_column.svg":                      ("Add Column Right",           {}),
    "add_row.svg":                         ("Add Row Below",              {}),
    "add.svg":                         ("Add",                        {}),
    "align_horizontal_left.svg":           ("Align Horizontal Left",      {}),
    "align_horizontal_center.svg":         ("Align Horizontal Center",    {}),
    "align_horizontal_right.svg":          ("Align Horizontal Right",     {}),
    "alarm-bell.svg":                      ("Notifications",              {}),
    "alarm-bell-active.svg":               ("Notifications Active",       {}),
    "apps_box.svg":                        ("Apps",                       {}),
    "archive.svg":                         ("Archive",                    {}),
    "arrow_right_alt.svg":                 ("Arrow Right Alt",            {}),
    "calendar_month.svg":                  ("Calendar Month",             {}),
    "cancel.svg":                          ("Cancel",                     {}),
    "cast.svg":                            ("Cast",                       {}),
    "cell_merge.svg":                      ("Cell Merge",                 {}),
    "check.svg":                           ("Check",                      {}),
    "checkbox_checked_light.svg":          ("Check Box",                  {}),
    "checkbox_checked_dark.svg":           ("Check Box",                  {"fill_color": "#E0E0E0"}),
    "checkbox_checked_disabled_light.svg": ("Check Box",                  {}),
    "checkbox_checked_disabled_dark.svg":  ("Check Box",                  {"fill_color": "#E0E0E0"}),
    "checkbox_unchecked_light.svg":        ("Check Box Outline Blank",    {}),
    "checkbox_unchecked_dark.svg":         ("Check Box Outline Blank",    {"fill_color": "#E0E0E0"}),
    "checkbox_unchecked_disabled_light.svg": ("Check Box Outline Blank",  {}),
    "checkbox_unchecked_disabled_dark.svg":  ("Check Box Outline Blank",  {"fill_color": "#E0E0E0"}),
    "clear.svg":                           ("Mop",                        {}),
    "cloud.svg":                           ("Cloud",                      {}),
    "close-button.svg":                    ("Close",                      {}),
    "collapse.svg":                        ("Collapse Content",           {}),
    "color_background.svg":                ("Filter B And W",             {}),
    "colored_charts.svg":                  ("Ssid Chart",                 {}),
    "combine_columns.svg":                 ("Combine Columns",            {"rotate": "90"}),
    "compare_arrows.svg":                  ("Compare Arrows",             {}),
    "contract.svg":                        ("Contract",                   {}),
    "copy.svg":                            ("Content Copy",               {}),
    "copy_all.svg":                        ("Copy All",                   {}),
    "create_new_folder.svg":               ("Create New Folder",          {}),
    "cube.svg":                            ("Deployed Code",              {}),
    "function.svg":                        ("Function",                   {}),
    "dark_mode_light.svg":                 ("Dark Mode",                  {"fill_color": "#FFFFFF"}),
    "dashboard_load.svg":                  ("Dashboard 2 Gear",           {}),
    "database.svg":                        ("Database",                   {}),
    "datetime.svg":                        ("Calendar Clock",             {}),
    "delete_forever.svg":                  ("Delete Forever",             {}),
    "diag_error.svg":                      ("Release Alert",              {}),
    "diag_info.svg":                       ("Info",                       {}),
    "diag_warning.svg":                    ("Warning",                    {}),
    "draft.svg":                           ("Draft",                      {}),
    "drag_handle_horizontal.svg":          ("Drag Handle",                {}),
    "drag_handle_vertical.svg":            ("Drag Handle",                {"rotate": "90"}),
    "expand.svg":                          ("Expand Content",             {}),
    "expand_more.svg":                     ("Expand More",                {}),
    "export.svg":                          ("Upload",                     {}),
    "extension.svg":                       ("Extension",                  {}),
    "file_open.svg":                       ("File Open",                  {}),
    "folder.svg":                          ("Folder",                     {}),
    "folder_open.svg":                     ("Folder Open",                {}),
    "filter_list.svg":                     ("Filter List",                {}),
    "format_paint.svg":                    ("Format Paint",               {}),
    "fullscreen.svg":                      ("Fullscreen",                 {}),
    "Fx.svg":                              ("Function",                   {}),
    "graph_1.svg":                         ("Graph 1",                    {}),
    "graph_4.svg":                         ("Graph 4",                    {}),
    "green_circle.svg":                    ("Circle",                     {}),
    "grid.svg":                            ("Background Grid Small",      {}),
    "grid_4x4.svg":                        ("Grid 4x4",                   {}),
    "grid_view.svg":                       ("Grid View",                  {}),
    "help.svg":                            ("Help",                       {}),
    "home.svg":                            ("Home",                       {}),
    "image.svg":                           ("Image",                      {}),
    "import.svg":                          ("Download",                   {}),
    "keyboard_arrow_down_light.svg":       ("Keyboard Arrow Down",        {}),
    "keyboard_arrow_down_dark.svg":        ("Keyboard Arrow Down",        {"fill_color": "#E0E0E0"}),
    "keyboard_arrow_down_hover.svg":       ("Keyboard Arrow Down",        {"fill_color": "#1177FF"}),
    "keyboard_arrow_down_pressed.svg":     ("Keyboard Arrow Down",        {"fill_color": "#CC00CC"}),
    "keyboard_arrow_left_light.svg":       ("Keyboard Arrow Left",        {}),
    "keyboard_arrow_left_dark.svg":        ("Keyboard Arrow Left",        {"fill_color": "#E0E0E0"}),
    "keyboard_arrow_left_hover.svg":       ("Keyboard Arrow Left",        {"fill_color": "#1177FF"}),
    "keyboard_arrow_left_pressed.svg":     ("Keyboard Arrow Left",        {"fill_color": "#CC00CC"}),
    "keyboard_arrow_right_light.svg":      ("Keyboard Arrow Right",       {}),
    "keyboard_arrow_right_dark.svg":       ("Keyboard Arrow Right",       {"fill_color": "#E0E0E0"}),
    "keyboard_arrow_right_hover.svg":      ("Keyboard Arrow Right",       {"fill_color": "#1177FF"}),
    "keyboard_arrow_right_pressed.svg":    ("Keyboard Arrow Right",       {"fill_color": "#CC00CC"}),
    "keyboard_arrow_up_light.svg":         ("Keyboard Arrow Up",          {}),
    "keyboard_arrow_up_dark.svg":          ("Keyboard Arrow Up",          {"fill_color": "#E0E0E0"}),
    "keyboard_arrow_up_hover.svg":         ("Keyboard Arrow Up",          {"fill_color": "#1177FF"}),
    "keyboard_arrow_up_pressed.svg":       ("Keyboard Arrow Up",          {"fill_color": "#CC00CC"}),
    "left-arrow.svg":                      ("Arrow Left",                 {}),
    "legend.svg":                          ("List",                       {}),
    "light_mode_light.svg":                ("Light Mode",                 {"fill_color": "#FFFFFF"}),
    "line_axis.svg":                       ("Line Axis",                  {}),
    "line_width_1_0.svg":                  ("Pen Size 1",                 {}),
    "line_width_1_5.svg":                  ("Pen Size 2",                 {}),
    "line_width_2_0.svg":                  ("Pen Size 3",                 {}),
    "line_width_3_0.svg":                  ("Pen Size 4",                 {}),
    "link.svg":                            ("Link 2",                     {}),
    "list.svg":                            ("List",                       {}),
    "logout.svg":                          ("Logout",                     {}),
    "loop.svg":                            ("Laps",                       {}),
    "merge.svg":                           ("Merge",                      {"rotate": "90"}),
    "mobile_layout.svg":                   ("Mobile Layout",              {}),
    "more_vert.svg":                       ("More Vert",                  {}),
    "move_selection_right.svg":            ("Move Selection Right",       {}),
    "move_view.svg":                       ("Drag Pan",                   {}),
    "numbers.svg":                         ("123",                        {}),
    "panel_left.svg":                      ("Dock To Left",               {"fill": "1"}),
    "panel_right.svg":                     ("Dock To Right",              {"fill": "1"}),
    "open_in_new.svg":                     ("Open In New",                {}),
    "panel_bottom.svg":                    ("Dock To Bottom",             {"fill": "1"}),
    "panel_left_off.svg":                  ("Dock To Left",               {"fill": "0"}),
    "panel_right_off.svg":                 ("Dock To Right",              {"fill": "0"}),
    "panel_bottom_off.svg":                ("Dock To Bottom",             {"fill": "0"}),
    "paste.svg":                           ("Content Paste",              {}),
    "pause.svg":                           ("Pause",                      {}),
    "play_arrow.svg":                      ("Play Arrow",                 {}),
    "plug_connect.svg":                    ("Plug Connect",               {}),
    "play_arrow_left.svg":                 ("Play Arrow",                 {"hflip": "1"}),
    "point_chart.svg":                     ("Timeline",                   {}),
    "position_bottom_left.svg":            ("Position Bottom Left",       {}),
    "position_bottom_right.svg":           ("Position Bottom Right",      {}),
    "position_top_left.svg":               ("Position Top Right",         {"hflip": "1"}),
    "position_top_right.svg":              ("Position Top Right",         {}),
    "radio_checked_light.svg":             ("Radio Button Checked",       {}),
    "radio_checked_dark.svg":              ("Radio Button Checked",       {"fill_color": "#E0E0E0"}),
    "radio_checked_disabled_light.svg":    ("Radio Button Checked",       {}),
    "radio_checked_disabled_dark.svg":     ("Radio Button Checked",       {"fill_color": "#E0E0E0"}),
    "radio_unchecked_light.svg":           ("Radio Button Unchecked",     {}),
    "radio_unchecked_dark.svg":            ("Radio Button Unchecked",     {"fill_color": "#E0E0E0"}),
    "radio_unchecked_disabled_light.svg":  ("Radio Button Unchecked",     {}),
    "radio_unchecked_disabled_dark.svg":   ("Radio Button Unchecked",     {"fill_color": "#E0E0E0"}),
    "ratio.svg":                           ("View Real Size",             {}),
    "red_circle.svg":                      ("Circle",                     {}),
    "recenter.svg":                        ("Recenter",                   {}),
    "reference_line.svg":                  ("Line Axis",                  {}),
    "refresh.svg":                         ("Refresh",                    {}),
    "restart_alt.svg":                     ("Restart Alt",                {}),
    "reload_light.svg":                    ("Refresh",                    {}),
    "reload_dark.svg":                     ("Refresh",                    {"fill_color": "#E0E0E0"}),
    "replay.svg":                          ("Replay",                     {}),
    "remove_list.svg":                     ("Playlist Remove",            {}),
    "remove_red.svg":                      ("Cancel",                     {}),
    "restore_page.svg":                    ("Restore Page",               {}),
    "right-arrow.svg":                     ("Arrow Right",                {}),
    "save.svg":                            ("Save",                       {}),
    "save_as.svg":                         ("Save As",                    {}),
    "scatter.svg":                         ("Grain",                      {}),
    "scatter_plot.svg":                    ("Scatter Plot",               {}),
    "search.svg":                          ("Search",                     {}),
    "search_light.svg":                    ("Search",                     {}),
    "search_dark.svg":                     ("Search",                     {"fill_color": "#E0E0E0"}),
    "service_toolbox.svg":                 ("Service Toolbox",            {}),
    "settings_cog_light.svg":              ("Settings",                   {}),
    "settings_cog_dark.svg":               ("Settings",                   {"fill_color": "#E0E0E0"}),
    "share_eta.svg":                       ("Share Eta",                  {}),
    "show_point.svg":                      ("Step Into",                  {}),
    "t0.svg":                              ("Start",                      {}),
    "tab_move.svg":                        ("Tab Move",                   {}),
    "trash.svg":                           ("Delete",                     {}),
    "transition_push.svg":                 ("Transition Push",            {}),
    "tree.svg":                            ("Account Tree",               {}),
    "tune.svg":                            ("Tune",                       {}),
    "upload_file.svg":                     ("Upload File",                {}),
    "view_list.svg":                       ("View List",                  {}),
    "visibility.svg":                      ("Visibility",                 {}),
    "visibility_off.svg":                  ("Visibility Off",             {}),
    "xy.svg":                              ("Table Chart View",           {}),
    "zoom_horizontal.svg":                 ("Arrows Outward",             {}),
    "zoom_in.svg":                         ("Zoom In",                    {}),
    "zoom_max.svg":                        ("Open With",                  {}),
    "zoom_vertical.svg":                   ("Arrows Outward",             {"rotate": "90"}),
}


def material_name_to_id(name: str) -> str:
    """`Add Column Right` -> `add_column_right`."""
    tokens = re.findall(r"[A-Za-z0-9]+", name)
    return "_".join(tokens).lower()


def build_variant(weight: int, fill: int, grade: int) -> str:
    if weight == 400 and fill == 0 and grade == 0:
        return "default"
    # Google's CDN expects non-default axes in the order: wght, grad, fill.
    parts: list[str] = []
    if weight != 400:
        parts.append(f"wght{weight}")
    if grade != 0:
        parts.append(f"grad{grade}")
    if fill != 0:
        parts.append(f"fill{fill}")
    return "".join(parts)


def inject_fill(svg_bytes: bytes, color: str) -> bytes:
    """Set `fill="<color>"` on every `<path>` element, replacing any existing
    fill attribute. Used to colorize freshly-downloaded Material Symbols
    SVGs (which ship without a fill) into the project's theme palette."""
    text = svg_bytes.decode("utf-8")

    def repl(match: re.Match[str]) -> str:
        tag = match.group(0)
        tag = re.sub(r'\s+fill="[^"]*"', "", tag)
        return tag.replace("<path", f'<path fill="{color}"', 1)

    text = re.sub(r"<path\b[^>]*?(?:/>|>)", repl, text)
    return text.encode("utf-8")


def apply_rotation(svg_bytes: bytes, angle: float) -> bytes:
    """Wrap the SVG body in `<g transform="rotate(angle cx cy)">` centered
    on the viewBox. Falls back to (0, 0) if no viewBox is present."""
    text = svg_bytes.decode("utf-8")
    cx = cy = 0.0
    box = re.search(r'viewBox\s*=\s*"([^"]+)"', text)
    if box is not None:
        try:
            x, y, w, h = (float(v) for v in box.group(1).split())
            cx, cy = x + w / 2, y + h / 2
        except ValueError:
            pass
    open_end = text.find(">")
    close_start = text.rfind("</svg>")
    if open_end < 0 or close_start < 0 or close_start <= open_end:
        return svg_bytes
    rotated = (
        text[: open_end + 1]
        + f'<g transform="rotate({angle} {cx} {cy})">'
        + text[open_end + 1 : close_start]
        + "</g>"
        + text[close_start:]
    )
    return rotated.encode("utf-8")


def apply_flip(svg_bytes: bytes, axis: str) -> bytes:
    """Wrap the SVG body in `<g transform="...">` to mirror it.

    axis="h" flips horizontally (mirrors left/right), axis="v" flips
    vertically. The transform is centered on the viewBox so the icon
    stays inside its drawing area; falls back to (0, 0) if no viewBox.
    """
    text = svg_bytes.decode("utf-8")
    cx = cy = 0.0
    box = re.search(r'viewBox\s*=\s*"([^"]+)"', text)
    if box is not None:
        try:
            x, y, w, h = (float(v) for v in box.group(1).split())
            cx, cy = x + w / 2, y + h / 2
        except ValueError:
            pass
    open_end = text.find(">")
    close_start = text.rfind("</svg>")
    if open_end < 0 or close_start < 0 or close_start <= open_end:
        return svg_bytes
    if axis == "h":
        transform = f"translate({2 * cx} 0) scale(-1 1)"
    elif axis == "v":
        transform = f"translate(0 {2 * cy}) scale(1 -1)"
    else:
        return svg_bytes
    flipped = (
        text[: open_end + 1]
        + f'<g transform="{transform}">'
        + text[open_end + 1 : close_start]
        + "</g>"
        + text[close_start:]
    )
    return flipped.encode("utf-8")


def download(url: str, user_agent: str, timeout: float) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": user_agent})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--style",
        choices=["outlined", "rounded", "sharp"],
        default="rounded",
        help="Material Symbols style (sharpness). Default: rounded.",
    )
    parser.add_argument(
        "--weight",
        type=int,
        default=300,
        choices=[100, 200, 300, 400, 500, 600, 700],
        help="Stroke weight axis. Default: 300 (matches the PJ4 icon family).",
    )
    parser.add_argument(
        "--fill",
        type=int,
        default=0,
        choices=[0, 1],
        help="Filled (1) vs unfilled (0). Default: 0.",
    )
    parser.add_argument(
        "--grade",
        type=int,
        default=0,
        choices=[-25, 0, 200],
        help="Grade axis. Default: 0.",
    )
    parser.add_argument(
        "--optical-size",
        "--size",
        dest="optical_size",
        type=int,
        default=48,
        choices=[20, 24, 40, 48],
        help="Optical size in pixels. Default: 48.",
    )
    parser.add_argument(
        "--fill-color",
        default="#3D3D3D",
        help="Inject fill=\"<color>\" into each downloaded SVG's <path> elements. "
        "Default: #3D3D3D (PJ4 light-theme ink). LoadSvg's RecolorSvgInk swaps "
        "this for #E0E0E0 when dark mode is active, so QSS image: url(...) "
        "rules see the right colour without a recolor pass. Pass an empty "
        "string to disable injection.",
    )
    parser.add_argument(
        "--output-dir",
        "-o",
        default="material_icons",
        help="Output directory. Default: material_icons/",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Overwrite existing files (otherwise they are skipped).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print URLs without downloading.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="HTTP timeout in seconds. Default: 30.",
    )
    parser.add_argument(
        "--user-agent",
        default="PJ4-icon-fetcher/1.0",
        help="User-Agent header sent with each request.",
    )
    args = parser.parse_args(argv)

    output_dir = Path(args.output_dir)
    if not args.dry_run:
        output_dir.mkdir(parents=True, exist_ok=True)

    # Cache so a duplicate Material name (e.g. "Circle" -> green/red) is
    # fetched once and written to each target file. Keyed by (icon_id,
    # variant) so two entries that need the same icon at different fill
    # axes don't trample each other.
    cache: dict[tuple[str, str], bytes] = {}
    ok = failed = skipped = 0

    for entry_no, (target, (material_name, transforms)) in enumerate(ICON_MAPPING.items(), start=1):
        target_path = output_dir / target
        if target_path.exists() and not args.overwrite and not args.dry_run:
            print(f"skip    {target}  (exists; rerun with --overwrite)")
            skipped += 1
            continue
        # Per-entry [fill=1] overrides the global --fill flag so individual
        # entries can opt into the solid variant (e.g. Position icons want
        # the filled corner indicator).
        line_fill = int(transforms.get("fill", args.fill))
        variant = build_variant(args.weight, line_fill, args.grade)
        icon_id = material_name_to_id(material_name)
        url = GSTATIC_URL.format(
            style=args.style, name=icon_id, variant=variant, size=args.optical_size
        )
        suffix_parts = []
        for key in ("rotate", "hflip", "vflip", "fill", "fill_color"):
            if key in transforms:
                suffix_parts.append(f"[{key}={transforms[key]}]")
        suffix = (" " + " ".join(suffix_parts)) if suffix_parts else ""
        if args.dry_run:
            print(f"would   {target}{suffix}  <- {url}")
            ok += 1
            continue

        cache_key = (icon_id, variant)
        try:
            if cache_key in cache:
                blob = cache[cache_key]
            else:
                blob = download(url, args.user_agent, args.timeout)
                cache[cache_key] = blob
        except urllib.error.HTTPError as exc:
            print(
                f"FAIL    {target}  (entry {entry_no}, '{material_name}' -> "
                f"{icon_id}, {variant}): HTTP {exc.code} {exc.reason} for {url}",
                file=sys.stderr,
            )
            failed += 1
            continue
        except urllib.error.URLError as exc:
            print(
                f"FAIL    {target}  (entry {entry_no}, '{material_name}'): {exc.reason}",
                file=sys.stderr,
            )
            failed += 1
            continue

        if "rotate" in transforms:
            try:
                blob = apply_rotation(blob, float(transforms["rotate"]))
            except ValueError:
                print(
                    f"warning: entry {entry_no} has non-numeric rotate value "
                    f"{transforms['rotate']!r}; writing un-rotated",
                    file=sys.stderr,
                )

        if transforms.get("hflip") == "1":
            blob = apply_flip(blob, "h")
        if transforms.get("vflip") == "1":
            blob = apply_flip(blob, "v")

        # Per-entry fill_color overrides the global --fill-color flag, so
        # specific icons (e.g. the toggle sun/moon glyphs that always
        # paint on a coloured track) can ship with a fixed colour baked
        # into every <path> regardless of what the CLI run requested.
        fill_color = transforms.get("fill_color", args.fill_color)
        if fill_color:
            blob = inject_fill(blob, fill_color)

        target_path.parent.mkdir(parents=True, exist_ok=True)
        target_path.write_bytes(blob)
        print(f"ok      {target}{suffix}  <- {icon_id}/{variant}")
        ok += 1

    summary = f"\nDone: {ok} ok, {failed} failed, {skipped} skipped"
    if not args.dry_run:
        summary += f" -> {output_dir}/"
    print(summary, file=sys.stderr)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
