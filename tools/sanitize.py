"""HTML sanitisation + minimal Markdown -> HTML conversion for PocketWiki.

Everything here is deterministic and dependency-free (stdlib html.parser only).
The firmware stores the output of these functions; the reference server renders it.
"""

from __future__ import annotations

import html
import posixpath
import re
from html.parser import HTMLParser

# --- escaping ---------------------------------------------------------------

def escape_html(s: str) -> str:
    """Escape for HTML text/attribute context (mirrors firmware html_helpers.c)."""
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;") \
            .replace('"', "&quot;").replace("'", "&#39;")


def escape_attr(s: str) -> str:
    return escape_html(s)

# --- link rewriting ---------------------------------------------------------

# known scheme prefixes that must never be followed (offline device)
_EXTERNAL_PREFIXES = ("http://", "https://", "//", "ftp://", "data:", "javascript:", "mailto:", "tel:", "file:")


def rewrite_href(href: str, base_dir: str, resolver) -> str | None:
    """Return the rewritten local href, or None to drop the link (text kept).

    resolver(key) -> article id | None, where key is either a path without
    extension or a normalized title.
    """
    href = href.strip()
    if not href or href.startswith("#"):
        return None
    # Already-rewritten archive links survive a second sanitizer pass.  This
    # is used by importers that resolve a complete corpus before packing it.
    if re.fullmatch(r"/a/\d+(?:#[A-Za-z0-9_.:%-]+)?", href):
        return href
    low = href.lower()
    if low.startswith(_EXTERNAL_PREFIXES):
        return None
    # strip query/fragment
    if "?" in href:
        href = href.split("?", 1)[0]
    frag = None
    if "#" in href:
        href, frag = href.split("#", 1)
    if not href:
        return None
    if href.startswith("/"):
        key = href.lstrip("/")
    else:
        key = posixpath.normpath(posixpath.join(base_dir, href))
    # try path key (extension-stripped), then normalized-title key
    aid = resolver(path_key(key))
    if aid is None:
        aid = resolver(key)
    if aid is None:
        return None
    return f"/a/{aid}" + (f"#{frag}" if frag else "")


def path_key(p: str) -> str:
    """'dir/foo.html' -> 'dir/foo'; strips any known article extension."""
    if p.lower().endswith((".html", ".htm", ".md")):
        p = p[: -len(p.split(".")[-1]) - 1]
    return p

# --- minimal Markdown subset ------------------------------------------------

_INLINE_CODE = re.compile(r"`([^`\n]+)`")
_LINK = re.compile(r"\[([^\]\n]+)\]\(([^)\n]+)\)")
_BOLD = re.compile(r"\*\*([^*\n]+)\*\*")
_ITALIC = re.compile(r"(?<!\w)\*([^*\n]+)\*(?!\w)|(?<!\w)_([^_\n]+)_(?!\w)")
_ESCAPE = re.compile(r"\\([\\`*_[\]()#+\-.!>|])")
_HEADING = re.compile(r"^(#{1,6})\s+(.+?)\s*#*\s*$")
_HR = re.compile(r"^\s{0,3}([-*_])[ \t]*\1[ \t]*\1[ \t]*$")
_UL = re.compile(r"^\s*[-*+]\s+(.*)$")
_OL = re.compile(r"^\s*\d+\.\s+(.*)$")
_BQ = re.compile(r"^\s*>\s?(.*)$")
_FENCE = re.compile(r"^\s*```\s*(\w*)")
_TABLE_DIVIDER_CELL = re.compile(r"^\s*:?-{3,}:?\s*$")

_TAG_RE = re.compile(r"</?([a-zA-Z][a-zA-Z0-9-]*)")


def _inline(text: str, resolver) -> str:
    text = _ESCAPE.sub(r"\1", text)
    text = _INLINE_CODE.sub(lambda m: f"<code>{escape_html(m.group(1))}</code>", text)
    text = _LINK.sub(lambda m: _md_link(m, resolver), text)

    # bold first, then italic: `**x**` must not be eaten by the italic regex
    text = _BOLD.sub(lambda m: f"<strong>{_inline(m.group(1), resolver)}</strong>", text)

    def rep(m):
        inner = m.group(1) if m.group(1) is not None else m.group(2)
        return f"<em>{_inline(inner, resolver)}</em>"
    text = _ITALIC.sub(rep, text)
    return text


def _md_link(m, resolver) -> str:
    text = m.group(1)
    url = m.group(2).strip()
    href = rewrite_href(url, "", resolver)
    if href is None:
        return escape_html(text)
    return f'<a href="{escape_attr(href)}">{escape_html(text)}</a>'


def _table_cells(line: str) -> list[str] | None:
    """Split a pipe-table row while preserving escaped literal pipes."""
    stripped = line.strip()
    if "|" not in stripped:
        return None
    if stripped.startswith("|"):
        stripped = stripped[1:]
    if stripped.endswith("|") and not stripped.endswith("\\|"):
        stripped = stripped[:-1]
    cells: list[str] = []
    current: list[str] = []
    escaped = False
    for char in stripped:
        if char == "|" and not escaped:
            cells.append("".join(current).strip())
            current = []
            continue
        if char == "\\" and not escaped:
            escaped = True
            current.append(char)
            continue
        escaped = False
        current.append(char)
    cells.append("".join(current).strip())
    return cells if len(cells) >= 2 else None


def _is_table_divider(line: str, columns: int) -> bool:
    cells = _table_cells(line)
    return cells is not None and len(cells) == columns and all(
        _TABLE_DIVIDER_CELL.fullmatch(cell) for cell in cells)


def md_to_html(text: str, resolver=None) -> str:
    """Convert a documented Markdown subset to minimal semantic HTML.

    Subset: ATX headings, paragraphs, unordered/ordered flat lists, pipe tables,
    fenced code, blockquotes, horizontal rules, links, bold/italic, inline code,
    and backslash escapes.
    Raw HTML in Markdown is escaped (treated as literal text). The first `# `
    heading is kept in the body (it doubles as the article title in the index).
    """
    if resolver is None:
        resolver = lambda key: None
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    out: list[str] = []
    i = 0
    n = len(lines)

    def emit_para(buf):
        para = " ".join(l.strip() for l in buf if l.strip())
        if para:
            out.append("<p>" + _inline(escape_html(para), resolver) + "</p>")

    def emit_list(buf, ordered):
        items = [re.sub(r"^\s*[-*+]\s+|\s*\d+\.\s+", "", l) for l in buf]
        tag = "ol" if ordered else "ul"
        out.append(f"<{tag}>")
        for it in items:
            out.append(f"<li>{_inline(escape_html(it.strip()), resolver)}</li>")
        out.append(f"</{tag}>")

    while i < n:
        line = lines[i]
        # fenced code
        fm = _FENCE.match(line)
        if fm:
            i += 1
            buf = []
            while i < n and not _FENCE.match(lines[i]):
                buf.append(lines[i])
                i += 1
            i += 1  # closing fence
            out.append("<pre><code>" + escape_html("\n".join(buf)) + "</code></pre>")
            continue
        # blank
        if not line.strip():
            i += 1
            continue
        # heading
        hm = _HEADING.match(line)
        if hm:
            level = len(hm.group(1))
            title = hm.group(2).strip()
            out.append(f"<h{level}>{_inline(escape_html(title), resolver)}</h{level}>")
            i += 1
            continue
        # GitHub-style pipe table. The divider must be present immediately
        # after the header so ordinary prose containing a pipe stays prose.
        header = _table_cells(line)
        if header is not None and i + 1 < n and _is_table_divider(lines[i + 1], len(header)):
            i += 2
            rows: list[list[str]] = []
            while i < n:
                cells = _table_cells(lines[i])
                if cells is None or len(cells) != len(header):
                    break
                rows.append(cells)
                i += 1
            out.append("<table><thead><tr>" + "".join(
                f"<th>{_inline(escape_html(cell), resolver)}</th>" for cell in header
            ) + "</tr></thead>")
            if rows:
                out.append("<tbody>" + "".join(
                    "<tr>" + "".join(
                        f"<td>{_inline(escape_html(cell), resolver)}</td>" for cell in row
                    ) + "</tr>" for row in rows
                ) + "</tbody>")
            out.append("</table>")
            continue
        # hr
        if _HR.match(line):
            out.append("<hr>")
            i += 1
            continue
        # blockquote
        if _BQ.match(line):
            buf = []
            while i < n and _BQ.match(lines[i]):
                buf.append(_BQ.match(lines[i]).group(1))
                i += 1
            quote = " ".join(l.strip() for l in buf if l.strip())
            out.append("<blockquote>" + _inline(escape_html(quote), resolver) + "</blockquote>")
            continue
        # list (flat; blank line or non-item ends it)
        om = _OL.match(line)
        um = _UL.match(line)
        if om or um:
            ordered = bool(om)
            buf = [line]
            i += 1
            while i < n:
                m2 = (_OL if ordered else _UL).match(lines[i])
                if m2 and lines[i].strip():
                    buf.append(lines[i])
                    i += 1
                else:
                    break
            emit_list(buf, ordered)
            continue
        # paragraph (consecutive non-blank, non-special lines)
        buf = [line]
        i += 1
        while i < n and lines[i].strip() and not (
                _HEADING.match(lines[i]) or _HR.match(lines[i]) or _FENCE.match(lines[i])
                or _BQ.match(lines[i]) or _OL.match(lines[i]) or _UL.match(lines[i])):
            buf.append(lines[i])
            i += 1
        emit_para(buf)
    return "\n".join(out)

# --- HTML sanitizer ---------------------------------------------------------

ALLOWED_TAGS = {
    "h1", "h2", "h3", "h4", "h5", "h6", "p", "ul", "ol", "li", "a",
    "em", "strong", "code", "pre", "blockquote", "table", "thead", "tbody",
    "tr", "th", "td", "br", "hr", "dl", "dt", "dd", "b", "i", "u",
}
_ALLOWED_ATTRS = {"a": {"href"}, "th": {"colspan", "rowspan"}, "td": {"colspan", "rowspan"}}
_VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input",
         "link", "meta", "param", "source", "track", "wbr"}   # HTML5 void
_TRANSPARENT = {"html", "head", "body", "div", "span"}  # containers dropped, content kept
_INLINE = {"a", "em", "strong", "code", "b", "i", "u", "span"}
_AUTO_CLOSE = {"p": {"h1", "h2", "h3", "h4", "h5", "h6", "p", "ul", "ol", "dl",
                      "blockquote", "pre", "table", "div"},
               "li": {"li"}, "dt": {"dt", "dd"}, "dd": {"dt", "dd"},
               "tr": {"tr"}, "td": {"td", "th"}, "th": {"td", "th"}}
_STACK_CAP = 64


class _Sanitizer(HTMLParser):
    """Whitelist sanitizer using an open-element stack.

    Robust against void elements written without end tags (<meta>, <link>)
    and against mismatched/unclosed tags: an unclosed disallowed element
    suppresses the rest of the document (the safe direction) until </body>.
    """

    def __init__(self, resolver):
        super().__init__(convert_charrefs=True)
        self.resolver = resolver
        self.out: list[str] = []
        self.stack: list[str] = []
        self.capped = False            # pathological nesting -> suppress all
        self.base_dir = ""            # set per-article for relative link resolution

    def _dropped(self) -> bool:
        if self.capped:
            return True
        return any(t not in ALLOWED_TAGS and t not in _TRANSPARENT for t in self.stack)

    def _push(self, tag: str):
        if len(self.stack) >= _STACK_CAP:
            self.capped = True
            return
        self.stack.append(tag)

    def _pop(self, tag: str):
        if tag in ("body", "html"):
            self.stack.clear()
            self.capped = False
            return
        if tag in _VOID:
            return
        if not self.stack:
            return
        if self.stack[-1] == tag:
            self.stack.pop()
            return
        if tag in self.stack:            # malformed nesting: pop through it
            while self.stack and self.stack[-1] != tag:
                self.stack.pop()
            self.stack.pop()

    def _auto_close(self, tag: str):
        while self.stack:
            top = self.stack[-1]
            if top in _INLINE:
                self.stack.pop()
                continue
            if top in _AUTO_CLOSE and tag in _AUTO_CLOSE[top]:
                self.stack.pop()
                continue
            break

    # -- emission ------------------------------------------------------------

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        if tag in _TRANSPARENT:
            self._push(tag)
            return
        if tag in _VOID:
            if tag == "img" and not self._dropped():
                alt = dict(attrs).get("alt")
                if alt:
                    self.out.append(escape_html(alt))
            elif tag in ALLOWED_TAGS and not self._dropped():
                self._emit(tag, self._clean_attrs(tag, attrs))
            return
        self._auto_close(tag)
        self._push(tag)
        if self._dropped():
            return
        if tag not in ALLOWED_TAGS:
            return
        self._emit(tag, self._clean_attrs(tag, attrs))

    def _clean_attrs(self, tag: str, attrs):
        keep = []
        for k, v in attrs:
            if v is None or k not in _ALLOWED_ATTRS.get(tag, ()):
                continue
            keep.append((k, v))
        if tag == "a" and any(k == "href" for k, _ in keep):
            href = rewrite_href(dict(keep)["href"], self.base_dir, self.resolver)
            keep = [("href", href)] if href is not None else []
        return keep

    def _emit(self, tag: str, keep):
        if keep:
            self.out.append("<%s %s>" % (tag, " ".join(f'{k}="{escape_attr(v)}"' for k, v in keep)))
        else:
            self.out.append(f"<{tag}>")

    def handle_startendtag(self, tag, attrs):
        tag = tag.lower()
        if tag in _VOID and not self._dropped():
            if tag == "img":
                alt = dict(attrs).get("alt")
                if alt:
                    self.out.append(escape_html(alt))
            elif tag in ALLOWED_TAGS:
                self._emit(tag, self._clean_attrs(tag, attrs))
        keep = []
        for k in _ALLOWED_ATTRS.get(tag, ()):
            if k in attrs and attrs[k] is not None:
                keep.append((k, attrs[k]))
        if tag == "a" and any(k == "href" for k, _ in keep):
            href = rewrite_href(dict(keep)["href"], self.base_dir, self.resolver)
            keep = [("href", href)] if href is not None else []
        if tag in _VOID:
            if keep:
                self.out.append("<%s %s>" % (tag, " ".join(f'{k}="{escape_attr(v)}"' for k, v in keep)))
            else:
                self.out.append(f"<{tag}>")
            return
        if keep:
            self.out.append("<%s %s>" % (tag, " ".join(f'{k}="{escape_attr(v)}"' for k, v in keep)))
        else:
            self.out.append(f"<{tag}>")

    def handle_endtag(self, tag):
        tag = tag.lower()
        if self._dropped():
            was_dropped = True
        else:
            was_dropped = False
        self._pop(tag)
        if not was_dropped and tag in ALLOWED_TAGS and tag not in _VOID:
            self.out.append(f"</{tag}>")

    def handle_data(self, data):
        if not self._dropped():
            self.out.append(escape_html(data))

    def handle_comment(self, data):
        pass

    def handle_decl(self, decl):
        pass

    def handle_pi(self, data):
        pass


def sanitize_html(doc: str, resolver=None, base_dir: str = "") -> str:
    """Strip everything except allowed semantic tags; rewrite internal links."""
    if resolver is None:
        resolver = lambda key: None
    p = _Sanitizer(resolver)
    p.base_dir = base_dir
    try:
        p.feed(doc)
        p.close()
    except Exception as exc:  # malformed markup must not kill the packer
        raise ValueError(f"malformed HTML: {exc}") from exc
    return "".join(p.out)
