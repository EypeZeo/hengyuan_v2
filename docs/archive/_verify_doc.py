import re, sys, io, collections

path = r"D:\My_Projects\hengyuan_v2\docs\HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.md"
text = io.open(path, encoding="utf-8").read()
lines = text.split("\n")

# 1. headings -> github slugs (with duplicate de-dup like GitHub)
def slug(h):
    s = h.strip()
    s = re.sub(r"<[^>]+>", "", s)
    s = s.lower()
    s = re.sub(r"[^\w\s-]", "", s, flags=re.UNICODE)
    s = re.sub(r"\s+", "-", s)
    s = re.sub(r"-{2,}", "-", s)
    return s.strip("-")

heads = []
seen = collections.Counter()
for i, ln in enumerate(lines, 1):
    m = re.match(r"^(#{1,6})\s+(.*?)\s*$", ln)
    if m:
        h = m.group(2)
        s = slug(h)
        seen[s] += 1
        if seen[s] > 1:
            s = f"{s}-{seen[s]-1}"
        heads.append((i, len(m.group(1)), h, s))

headset = {h[3] for h in heads}

# 2. internal links
links = []
for i, ln in enumerate(lines, 1):
    for m in re.finditer(r"\]\(#([^)]+)\)", ln):
        links.append((i, m.group(1)))

bad = [(i, a) for i, a in links if a not in headset]
print("HEADINGS:", len(heads))
print("INTERNAL ANCHOR LINKS:", len(links))
print("BROKEN ANCHORS:", len(bad))
for i, a in bad:
    print(f"  line {i}: #{a}")

# 3. fence balance
fences = [i for i, ln in enumerate(lines, 1) if ln.strip().startswith("```")]
print("FENCE MARKERS:", len(fences), "balanced:", len(fences) % 2 == 0)
langs = collections.Counter()
for i in fences:
    t = lines[i-1].strip().strip("`").strip()
    if t:
        langs[t] += 1
print("FENCE LANGUAGES:", dict(langs))

# 4. math delimiters
disp = text.count("$$")
inline = len(re.findall(r"(?<!\$)\$(?!\$)", text))
print("$$ OCCURRENCES:", disp, "even:", disp % 2 == 0)

# 5. mermaid block contents sanity
inb = False
for i, ln in enumerate(lines, 1):
    if ln.strip().startswith("```mermaid"):
        inb = True
        start = i
        continue
    if inb and ln.strip() == "```":
        inb = False
        continue
if inb:
    print("UNCLOSED MERMAID BLOCK starting at", start)

# 6. forbidden mentions
for pat in ["GPT", "gpt", "Claude", "claude", "Codex", "codex", "AI 会话"]:
    c = text.count(pat)
    if c:
        print(f"MENTION {pat}: {c}")

# 7. example code snippets (non-mermaid fenced blocks)
print("NON-MERMAID FENCES:", len(fences) - 2 * langs.get("mermaid", 0))

# 8. table sanity: rows per table block
print("TOTAL LINES:", len(lines))
print("STATUS MARKERS: DONE=%d WIP=%d TODO=%d BLOCKED=%d DROPPED=%d" % (
    text.count("[DONE]"), text.count("[WIP:"), text.count("[TODO:"),
    text.count("[BLOCKED]"), text.count("[DROPPED]")))
print("COMMENT BLOCKS: EXTENSION_POINT=%d RATIONALE=%d MODULE_BOUNDARY=%d" % (
    text.count("<!-- EXTENSION_POINT:"), text.count("<!-- RATIONALE:"),
    text.count("<!-- MODULE_BOUNDARY:")))

# 9. table column consistency
def cols(row):
    r = row.strip()
    if r.startswith("|"):
        r = r[1:]
    if r.endswith("|"):
        r = r[:-1]
    return len(r.split("|"))

issues = 0
i = 0
while i < len(lines):
    if lines[i].strip().startswith("|"):
        block = []
        j = i
        while j < len(lines) and lines[j].strip().startswith("|"):
            block.append((j + 1, lines[j]))
            j += 1
        widths = {}
        for ln_no, row in block:
            if re.match(r"^\s*\|[\s:|-]+\|\s*$", row):
                continue
            widths.setdefault(cols(row), []).append(ln_no)
        if len(widths) > 1:
            issues += 1
            print(f"  TABLE at line {block[0][0]}: inconsistent column counts {[(k, v[:3]) for k, v in widths.items()]}")
        i = j
    else:
        i += 1
print("TABLE BLOCKS WITH COLUMN MISMATCH:", issues)

# 10. fault registry completeness
fi = sorted({int(m) for m in re.findall(r"\*\*FI-(\d{3})\*\*", text)})
missing = [n for n in range(1, 42) if n not in fi]
dups = [n for n, c in collections.Counter(re.findall(r"\*\*FI-(\d{3})\*\*", text)).items() if c > 1]
print("FI ENTRIES:", len(fi), "range:", (min(fi), max(fi)) if fi else None)
print("FI MISSING:", missing, "FI DUPLICATED:", dups)

# 11. station map class coverage (nodes are stadium-shaped: ID(["label"]))
station_ids = set(re.findall(r'^\s{8}([A-Z]{1,2}\d?)\(', text, flags=re.M))
classed = set()
for m in re.finditer(r"^    class ([A-Z0-9,]+) (\w+)$", text, flags=re.M):
    classed |= set(m.group(1).split(","))
print("STATION NODES:", len(station_ids), "CLASSED:", len(classed))
print("STATIONS DECLARED BUT NOT CLASSED:", sorted(station_ids - classed))
print("CLASSED BUT NOT DECLARED:", sorted(classed - station_ids))

# 12. annotation comments must live ONLY inside the appendix E html fence
in_fence = False
lang = ""
fence_hits = []
outside = []
for n, l in enumerate(lines, 1):
    if l.startswith("```"):
        if not in_fence:
            in_fence, lang = True, l[3:].strip()
        else:
            in_fence, lang = False, ""
        continue
    if "<!--" in l:
        (fence_hits if (in_fence and lang == "html") else outside).append(n)
print("ANNOTATION LINES IN html FENCE:", len(fence_hits))
print("ANNOTATION LINES OUTSIDE FENCE:", outside)

# 13. body purity: no raw HTML outside fenced blocks at all
raw = []
in_fence = False
for n, l in enumerate(lines, 1):
    if l.startswith("```"):
        in_fence = not in_fence
        continue
    if not in_fence and re.search(r"<[A-Za-z/!]", l):
        raw.append((n, l[:60]))
print("RAW HTML OUTSIDE FENCES:", raw)

