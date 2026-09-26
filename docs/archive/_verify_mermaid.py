import re
import sys

p = sys.argv[1]
t = open(p, encoding='utf-8').read()
blocks = re.findall(r"```mermaid(.*?)```", t, flags=re.S)
print("MERMAID BLOCKS:", len(blocks))
m = blocks[0]
decl = set(re.findall(r'([A-Z]{1,2}\d?)\(\["', m))
edges = re.findall(r"^\s+(\S+) (?:-->|-) (\S+)$", m, flags=re.M)
cls = set()
for mm in re.finditer(r"^    class ([A-Z0-9,]+) (\w+)$", m, flags=re.M):
    cls |= set(x for x in mm.group(1).split(",") if x)
print("NODES DECLARED:", len(decl), sorted(decl))
print("CLASSED:", len(cls), sorted(cls))
print("DECLARED BUT NOT CLASSED:", sorted(decl - cls))
print("CLASSED BUT NOT DECLARED:", sorted(cls - decl))
bad = sorted({x for e in edges for x in e if x not in decl})
print("EDGE ENDPOINTS UNDECLARED:", bad)
print("EDGES:", len(edges))
# every station id used in the detail table must exist as a node
tbl = set(re.findall(r"^\| ([A-Z]{1,2}\d?) \| `\[", t, flags=re.M))
print("TABLE STATION IDS:", len(tbl))
print("TABLE IDS MISSING FROM MAP:", sorted(tbl - decl))
print("MAP IDS MISSING FROM TABLE:", sorted(decl - tbl))
