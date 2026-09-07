#!/usr/bin/env python3
"""Derive a pg_dump-shaped schema from CSV data, for csv2pg --schema.

Sanitizes the headers the way the reference database does, samples rows from
several offsets in every file to infer a column type, and writes a CREATE TABLE
plus one CREATE INDEX per requested column.

The types are inferred from a sample, so treat the output as a first draft to
read before loading, not as an authority. Widening a guess (bigint -> double
precision -> text) is always safe; narrowing it is what breaks a COPY.

  python tools/csv2schema.py <file.csv|dir> --table grants -o grants_schema.sql

Several years of a download whose columns drifted are handled with --union: the
schema becomes the union of every header seen, so each year loads into the one
table with its absent columns left NULL.

  python tools/csv2schema.py FY20*/ --union --recursive --table assistance \\
      --partition-by action_date_fiscal_year --parent-ddl assistance_parent.sql \\
      -o assistance_schema.sql
"""
import argparse
import csv
import io
import multiprocessing
import os
import pathlib
import re
import sys
from collections import Counter

csv.field_size_limit(1 << 30)

DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
TS_RE = re.compile(r"^\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}(:\d{2}(\.\d+)?)?\s*([Zz]|[+-]\d{2}(:?\d{2})?)?$")
INT_RE = re.compile(r"^[+-]?\d+$")
FLOAT_RE = re.compile(r"^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$")
PADDED_RE = re.compile(r"^[+-]?0\d")  # 047, 08 -- a code, not a number
# Calibrated against the reference contracts dump, which types t/f and YES/NO
# columns as boolean but keeps the Y/N ones as text -- those are code domains
# that can grow a third value, so "y"/"n" is deliberately absent here.
BOOL_VALUES = {"true", "false", "t", "f", "yes", "no", "on", "off"}

INT64_MIN, INT64_MAX = -(2 ** 63), 2 ** 63 - 1


def sanitize(name):
    """Header -> column name, matching what the csv2pg matcher expects."""
    out = re.sub(r"[^0-9a-z]+", "_", name.strip().lower()).strip("_")
    if out and out[0].isdigit():
        out = "c_" + out
    return out


class ColumnStats:
    __slots__ = ("nonempty", "is_bool", "is_int", "is_float", "is_date", "is_ts", "sample")

    def __init__(self):
        self.nonempty = 0
        self.is_bool = self.is_int = self.is_float = self.is_date = self.is_ts = True
        self.sample = None

    def add(self, v):
        if v == "":
            return
        self.nonempty += 1
        if self.sample is None:
            self.sample = v
        if self.is_bool and v.lower() not in BOOL_VALUES:
            self.is_bool = False
        padded = PADDED_RE.match(v) is not None
        if self.is_int and (padded or not (INT_RE.match(v) and INT64_MIN <= int(v) <= INT64_MAX)):
            self.is_int = False
        if self.is_float and (padded or not FLOAT_RE.match(v)):
            self.is_float = False
        if self.is_date and not DATE_RE.match(v):
            self.is_date = False
        if self.is_ts and not (DATE_RE.match(v) or TS_RE.match(v)):
            self.is_ts = False

    def sql_type(self):
        # Never guess from nothing: an all-blank column stays text.
        if self.nonempty == 0:
            return "text"
        if self.is_bool:
            return "boolean"
        if self.is_int:
            return "bigint"
        if self.is_float:
            return "double precision"
        if self.is_date:
            return "date"
        if self.is_ts:
            return "timestamp without time zone"
        return "text"


def collect(inputs, recursive):
    """Every .csv under the given files and directories, deduplicated."""
    files, seen = [], set()
    for raw in inputs:
        p = pathlib.Path(raw)
        if p.is_dir():
            found = sorted(p.rglob("*.csv") if recursive else p.glob("*.csv"))
        elif p.is_file():
            found = [p]
        else:
            print("error: no such file or directory: %s" % p, file=sys.stderr)
            return None
        for f in found:
            key = f.resolve()
            if key not in seen:
                seen.add(key)
                files.append(f)
    return files


def read_header(path):
    with open(path, "r", encoding="utf-8-sig", errors="replace", newline="") as fh:
        return next(csv.reader(fh))


def sample_file(path, col_map, ncsv, stats, chunks, chunk_bytes, rows_cap):
    """Reads `chunks` windows spread through the file, parsing whole records.

    `col_map[i]` is the schema column that CSV field `i` feeds. After a seek the
    position is mid-record, so the first partial line is dropped, and any record
    that does not have exactly `ncsv` fields is skipped -- a quoted newline can
    desynchronize the reader briefly.
    """
    size = os.path.getsize(path)
    rows = 0
    # One window already covers a file smaller than the window; more would just
    # re-read the same rows and inflate the counts.
    if size <= chunk_bytes:
        chunks = 1
    for k in range(chunks):
        start = 0 if k == 0 else (size // (chunks + 1)) * (k + 1)
        with open(path, "rb") as fh:
            fh.seek(start)
            raw = fh.read(chunk_bytes)
        if not raw:
            continue
        text = raw.decode("utf-8", errors="replace")
        nl = text.find("\n")
        if nl < 0:
            continue
        text = text[nl + 1:]                    # drop the partial first line
        cut = text.rfind("\n")
        if cut < 0:
            continue
        text = text[:cut + 1]                   # and the partial last one
        for rec in csv.reader(io.StringIO(text, newline="")):
            if len(rec) != ncsv:
                continue
            for i, v in enumerate(rec):
                stats[col_map[i]].add(v)
            rows += 1
            if rows >= rows_cap:
                return rows
    return rows


def type_ok(v, t):
    """Would Postgres accept `v` into a column of type `t`?"""
    if v == "":
        return True
    if t == "text":
        return True
    padded = PADDED_RE.match(v) is not None
    if t == "boolean":
        return v.lower() in BOOL_VALUES
    if t == "bigint":
        return not padded and INT_RE.match(v) is not None and INT64_MIN <= int(v) <= INT64_MAX
    if t == "double precision":
        return not padded and FLOAT_RE.match(v) is not None
    if t == "date":
        return DATE_RE.match(v) is not None
    if t == "timestamp without time zone":
        return DATE_RE.match(v) is not None or TS_RE.match(v) is not None
    return True


def verify_file(path, col_map, ncsv, types, offenders, max_examples):
    """Full scan: every row, every non-text column, against its inferred type.

    Sampling can only ever prove a type wrong, never right, and a partial year
    is exactly where the one stray value hides. This reads all of it.
    """
    rows = 0
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        r = csv.reader(fh)
        next(r, None)  # header
        for rec in r:
            rows += 1
            if len(rec) != ncsv:
                offenders.setdefault("__row__", []).append(
                    (path.name, rows, "", "%d fields, expected %d" % (len(rec), ncsv)))
                continue
            for csv_i, sch_i in enumerate(col_map):
                t = types[sch_i]
                if t == "text":
                    continue
                v = rec[csv_i]
                if not type_ok(v, t):
                    lst = offenders.setdefault(sch_i, [])
                    if len(lst) < max_examples:
                        lst.append((path.name, rows, v, t))
    return rows


def _verify_worker(task):
    path, col_map, ncsv, types, max_examples = task
    offenders = {}
    rows = verify_file(pathlib.Path(path), col_map, ncsv, types, offenders, max_examples)
    return path, rows, offenders


def merge_offenders(into, more, max_examples):
    for key, examples in more.items():
        lst = into.setdefault(key, [])
        room = max_examples - len(lst)
        if room > 0:
            lst.extend(examples[:room])
        elif key == "__row__":
            lst.extend(examples)  # bad rows are counted, not just sampled


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="+", help="csv files and/or directories of them")
    ap.add_argument("--table", required=True, help="table name to emit")
    ap.add_argument("-o", "--output", help="destination .sql (default: stdout)")
    ap.add_argument("--index", default="", help="comma-separated columns to index")
    ap.add_argument("--index-all", action="store_true", help="index every column")
    ap.add_argument("--brin", default="",
                    help="comma-separated columns to give a BRIN index -- cheap and effective "
                         "for a column the rows are already roughly ordered by, such as a date")
    ap.add_argument("--force-text", default="",
                    help="comma-separated columns to keep as text whatever the sample says")
    ap.add_argument("--verify", action="store_true",
                    help="after inferring, read every row of every file and report values "
                         "their column's type would reject (slow, but it is the only thing "
                         "that catches the stray value a sample missed)")
    ap.add_argument("--fix", action="store_true",
                    help="with --verify, widen any contradicted column to text")
    ap.add_argument("--union", action="store_true",
                    help="allow headers to differ between files and take their union")
    ap.add_argument("--recursive", action="store_true", help="descend into subdirectories")
    ap.add_argument("--partition-by", help="column to LIST-partition the parent table on")
    ap.add_argument("--parent-ddl", help="write the partitioned parent table's DDL here")
    ap.add_argument("--chunks", type=int, default=3, help="sample windows per file (default 3)")
    ap.add_argument("--sample-mb", type=int, default=20, help="MiB per window (default 20)")
    ap.add_argument("--max-rows", type=int, default=200000, help="row cap per file (default 200k)")
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel workers for --verify (default: cores, capped at 8). The scan "
                         "is one file per worker and IO bound, so this is the whole speedup.")
    a = ap.parse_args()

    if a.parent_ddl and not a.partition_by:
        print("error: --parent-ddl needs --partition-by", file=sys.stderr)
        return 2

    files = collect(a.input, a.recursive)
    if files is None:
        return 2
    if not files:
        print("error: no CSV files found", file=sys.stderr)
        return 2

    # Read every header first, so the column set is settled before any sampling.
    headers = {}
    cols, seen = [], {}
    for f in files:
        h = read_header(f)
        headers[f] = h
        if not a.union and headers[files[0]] != h:
            print("error: %s has a different header from %s (pass --union to take "
                  "the union of both)" % (f.name, files[0].name), file=sys.stderr)
            return 1
        for i, name in enumerate(h):
            c = sanitize(name)
            if not c:
                print("error: %s column %d has an empty name" % (f.name, i + 1), file=sys.stderr)
                return 1
            if c not in seen:
                seen[c] = len(cols)
                cols.append(c)

    # Two different headers in one file collapsing to one column name would make
    # the CSV ambiguous; across files it is fine and expected.
    for f, h in headers.items():
        local = {}
        for i, name in enumerate(h):
            c = sanitize(name)
            if c in local:
                print("error: in %s, columns %d and %d both sanitize to %s (%r, %r)"
                      % (f.name, local[c] + 1, i + 1, c, h[local[c]], h[i]), file=sys.stderr)
                return 1
            local[c] = i

    if a.union and len(set(tuple(h) for h in headers.values())) > 1:
        print("union: %d distinct header(s) across %d file(s) -> %d columns"
              % (len(set(tuple(h) for h in headers.values())), len(files), len(cols)),
              file=sys.stderr)
        for f in files:
            missing = [c for c in cols if c not in {sanitize(x) for x in headers[f]}]
            if missing:
                print("  %s lacks %d: %s" % (f.name, len(missing), ", ".join(missing[:6])
                                             + (" ..." if len(missing) > 6 else "")),
                      file=sys.stderr)

    stats = [ColumnStats() for _ in cols]
    total = 0
    for f in files:
        h = headers[f]
        col_map = [seen[sanitize(x)] for x in h]
        n = sample_file(f, col_map, len(h), stats, a.chunks, a.sample_mb << 20, a.max_rows)
        print("sampled %7d rows from %s" % (n, f.name), file=sys.stderr)
        total += n
    if total == 0:
        print("error: no complete records sampled", file=sys.stderr)
        return 1

    types = [s.sql_type() for s in stats]

    # An escape hatch for values that parse as a number but are identifiers, so
    # the schema stays reproducible from the command line instead of hand-edited.
    for c in (sanitize(x) for x in a.force_text.split(",") if x.strip()):
        if c not in seen:
            print("error: --force-text names %s, which is not a column" % c, file=sys.stderr)
            return 1
        types[seen[c]] = "text"

    if a.verify:
        offenders = {}
        scanned = 0
        jobs = a.jobs if a.jobs > 0 else min(multiprocessing.cpu_count() or 4, 8)
        jobs = max(1, min(jobs, len(files)))
        tasks = []
        for f in files:
            h = headers[f]
            tasks.append((str(f), [seen[sanitize(x)] for x in h], len(h), types, 5))
        print("verifying %d file(s) on %d worker(s)" % (len(files), jobs), file=sys.stderr)
        done = 0
        if jobs == 1:
            results = (_verify_worker(t) for t in tasks)
            for path, n, off in results:
                scanned += n
                done += 1
                merge_offenders(offenders, off, 5)
                print("  [%d/%d] %9d rows  %s" % (done, len(files), n,
                      os.path.basename(path)), file=sys.stderr)
        else:
            with multiprocessing.Pool(jobs) as pool:
                for path, n, off in pool.imap_unordered(_verify_worker, tasks):
                    scanned += n
                    done += 1
                    merge_offenders(offenders, off, 5)
                    print("  [%d/%d] %9d rows  %s" % (done, len(files), n,
                          os.path.basename(path)), file=sys.stderr)
        bad_rows = offenders.pop("__row__", [])
        if bad_rows:
            print("\n%d row(s) had the wrong field count -- the first few:" % len(bad_rows),
                  file=sys.stderr)
            for name, ln, _, why in bad_rows[:5]:
                print("  %s line %d: %s" % (name, ln, why), file=sys.stderr)
        if offenders:
            print("\n%d column(s) hold values their inferred type would reject, over %d rows:"
                  % (len(offenders), scanned), file=sys.stderr)
            for sch_i, ex in sorted(offenders.items()):
                print("  %s (%s)" % (cols[sch_i], types[sch_i]), file=sys.stderr)
                for name, ln, v, t in ex:
                    print("      %s line %d: %r" % (name, ln, v), file=sys.stderr)
                if a.fix:
                    types[sch_i] = "text"
            if a.fix:
                print("--fix: widened %d column(s) to text" % len(offenders), file=sys.stderr)
            else:
                print("rerun with --fix to widen these to text, or edit the schema by hand.",
                      file=sys.stderr)
        else:
            print("verify: all %d rows load cleanly under these types" % scanned, file=sys.stderr)

    if a.index_all:
        idx = list(cols)
    else:
        idx = [sanitize(c) for c in a.index.split(",") if c.strip()]
        for c in idx:
            if c not in seen:
                print("error: --index names %s, which is not a column" % c, file=sys.stderr)
                return 1

    brin = [sanitize(c) for c in a.brin.split(",") if c.strip()]
    for c in brin:
        if c not in seen:
            print("error: --brin names %s, which is not a column" % c, file=sys.stderr)
            return 1

    if a.partition_by:
        pk = sanitize(a.partition_by)
        if pk not in seen:
            print("error: --partition-by names %s, which is not a column" % pk, file=sys.stderr)
            return 1
        # A NULL key would land every such row in the DEFAULT partition, which is
        # a silent trap when the column is the one thing routing depends on.
        if stats[seen[pk]].nonempty < total:
            print("warning: %s was empty in %d of %d sampled rows; those rows route to the "
                  "DEFAULT partition" % (pk, total - stats[seen[pk]].nonempty, total),
                  file=sys.stderr)

    src = ", ".join(str(x) for x in a.input)
    out = ["--",
           "-- Generated by tools/csv2schema.py from %s" % src,
           "-- %d file(s), types inferred from %d sampled rows -- review before loading."
           % (len(files), total),
           "--",
           ""]
    out.append("CREATE TABLE public.%s (" % a.table)
    for i, (c, t) in enumerate(zip(cols, types)):
        out.append("    %s %s%s" % (c, t, "," if i + 1 < len(cols) else ""))
    out.append(");")
    out.append("")
    for c in idx:
        out.append("CREATE INDEX ix_%s_%s ON public.%s USING btree (%s);" % (a.table, c, a.table, c))
    for c in brin:
        out.append("CREATE INDEX ix_%s_%s_brin ON public.%s USING brin (%s);" % (a.table, c, a.table, c))
    text = "\n".join(out) + "\n"

    if a.output:
        pathlib.Path(a.output).write_text(text, encoding="utf-8")
        print("wrote %s: %d columns, %d btree + %d brin index(es)"
              % (a.output, len(cols), len(idx), len(brin)), file=sys.stderr)
    else:
        sys.stdout.write(text)

    if a.parent_ddl:
        # Kept in its own file: csv2pg's --schema parser wants the plain
        # "CREATE TABLE ... \n);" shape, which a PARTITION BY clause breaks.
        pd = ["--",
              "-- Partitioned parent for %s, generated by tools/csv2schema.py." % a.table,
              "-- Load each year into its own table, then ATTACH it.",
              "--",
              "",
              "CREATE TABLE public.%s (" % a.table]
        for i, (c, t) in enumerate(zip(cols, types)):
            pd.append("    %s %s%s" % (c, t, "," if i + 1 < len(cols) else ""))
        pd.append(") PARTITION BY LIST (%s);" % sanitize(a.partition_by))
        pd.append("")
        pd.append("-- Catches rows whose %s does not match any attached year."
                  % sanitize(a.partition_by))
        pd.append("CREATE TABLE public.%s_unrouted PARTITION OF public.%s DEFAULT;"
                  % (a.table, a.table))
        pd.append("")
        pathlib.Path(a.parent_ddl).write_text("\n".join(pd) + "\n", encoding="utf-8")
        print("wrote %s: parent partitioned by %s" % (a.parent_ddl, sanitize(a.partition_by)),
              file=sys.stderr)

    print("types: " + ", ".join("%s=%d" % kv for kv in sorted(Counter(types).items())),
          file=sys.stderr)
    # Anything thin is worth a second look before committing to a multi-GB load.
    thin = [(c, t, s.nonempty) for c, t, s in zip(cols, types, stats)
            if s.nonempty < max(20, total // 1000)]
    if thin:
        print("\n%d column(s) had almost no data in the sample -- their type is a guess:"
              % len(thin), file=sys.stderr)
        for c, t, n in thin:
            print("  %-62s %-28s %d non-empty" % (c, t, n), file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
