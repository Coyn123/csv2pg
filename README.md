# csv2pg

Bulk-loads USAspending contract CSVs into PostgreSQL. One positional argument —
the CSV — and the tool creates the database, creates the table, loads the data,
and builds the indexes.

```bash
csv2pg FY2025_All_Contracts_Full_20260806_1.csv
```

The argument may also be a **directory**, in which case every `*.csv` in it is
loaded in name order into the same table. That is the usual case: a full-year
USAspending download unzips into seven ~2 GB parts.

## Why it's threaded the way it is

The bottleneck is I/O at both ends, so threads only appear where they multiply
throughput:

| Stage | Threads | Reasoning |
|---|---|---|
| Reading the CSV | 1 | A disk is fastest on one sequential stream. Splitting the read makes it seek. The reader only scans for record boundaries — it never parses fields. |
| `COPY` into Postgres | N (default: cores, capped at 8) | Postgres parses `COPY` input on **one core per connection**. This is the real ceiling, and N connections lift it ~N×. |
| Index builds | N | 20 independent `CREATE INDEX` statements, one per connection. |

Between the reader and the workers sits a bounded queue. It provides
backpressure: if the server falls behind, the reader blocks rather than buffering
the whole file into RAM. Peak memory is roughly `chunk-mb × (jobs × 2 + 2)`.

The CSV bytes are handed to Postgres **unparsed**. The client only splits the
stream on record boundaries (tracking quote state so a newline inside a quoted
field is not mistaken for one), so no per-field work happens in this process at
all. The server's own `COPY` parser does that job, in parallel, N ways.

## Column mapping

The schema in `contracts_schema.sql` uses sanitized column names, while the raw
CSV headers do not. `csv2pg` reconciles them at startup by matching in three
tiers — literal name, then punctuation-insensitive, then the same with the
leading `c` the reference database prefixes onto names starting with a digit:

```
outlayed_amount_from_COVID-19_supplementals_...  ->  outlayed_amount_from_covid_19_supplementals_...
1862_land_grant_college                          ->  c_1862_land_grant_college
```

If any CSV column has no schema counterpart, the load aborts before touching the
database and names the offenders. `--dry-run` performs just this check. A dataset with a
different column set is loaded by pointing `--schema` at its own dump -- see
"Loading a second, different dataset" below.

Non-text columns (dates, numerics, booleans) are loaded with `FORCE_NULL`, so a
quoted empty string `""` becomes `NULL` instead of failing the row. Text columns
keep `''` as an empty string, which is what the reference database holds.

## Building

Needs a C++20 compiler and libpq. On this machine, MSYS2 UCRT64:

```bash
pacman -S mingw-w64-ucrt-x86_64-postgresql make
```

```bash
make
```

The output is `build/csv2pg.exe` plus the handful of DLLs it needs, staged
alongside it, so the folder can be copied anywhere. The GCC runtime is linked
statically.

Note: `make` lives at `C:/msys64/usr/bin/make.exe`; Git Bash's `/usr/bin` does
not have it. Either call it by full path or run the build from an MSYS2 shell.

If the schema ever changes, regenerate the embedded column table rather than
editing `src/schema.h` by hand:

```bash
make schema
```

## Loading a second, different dataset

Nothing is shared between loads except the connection: `--dbname` picks (and
creates) the database, `--table` picks the table, and index names are
`ix_<table>_<column>`, so a second table can live beside the first in the same
database without colliding. The table is created `IF NOT EXISTS` and the load is
an append -- it never drops or overwrites -- so the only way to disturb an
existing dataset is to aim `--table` at it, and even then the rows are added
rather than replaced.

What a different dataset *does* need is its own column list. Pass its pg_dump
with `--schema` and the built-in one is set aside for that run:

```bash
csv2pg --dbname grants --schema grants_schema.sql /data/FY2025_All_Assistance/
```

The file is parsed at startup by the same rules `tools/gen_schema.py` uses: the
first `CREATE TABLE` supplies the table name, the columns and their types, and
every single-column `CREATE INDEX ... USING btree`, `brin` or `hash` supplies an
index to rebuild after the load; the btree keeps the bare `ix_<table>_<column>`
name and the others are suffixed with the method, so both can index one column.
Multi-column and expression indexes are skipped. With `--schema`
given and `--table` omitted, the target table is the one the dump names, so the
command above loads into `grants.grants`.

Because the parse happens before anything touches the server, `--dry-run
--schema ...` validates a new dataset's headers against a candidate schema
without creating so much as a database.

The compiled-in contracts schema is still the default, so existing commands are
unaffected.

### Deriving a schema when you only have CSVs

`tools/csv2schema.py` writes one from the data: it sanitizes the headers the way
the reference database does, samples rows from several offsets in every file,
infers a type per column, and emits the `CREATE TABLE` plus the indexes you ask
for.

```bash
python tools/csv2schema.py <dir> --table assistance     --index action_date,recipient_name,recipient_uei -o assistance_schema.sql
```

`--brin a,b` gives a column a BRIN index instead of a btree. On a column the
rows already arrive roughly ordered by -- a date, in a download written in date
order -- it is a few megabytes and seconds to build where the btree is gigabytes
and minutes, and it serves range scans about as well. It is the wrong choice for
a high-cardinality lookup key, where a btree is what you want.

`--index-all` indexes every column, and `--force-text a,b` pins columns whose
values parse as numbers but are really identifiers (a CFDA number like `93.243`
is the case that motivated the flag -- as a float it is both lossy and awkward to
compare against).

The inference is calibrated against the contracts dump: run on `testdata/sample.csv`
it reproduces all 297 of `contracts_schema.sql`'s column types exactly. Two rules
do most of that work and are worth knowing about, since both encode a convention
rather than a fact about the data:

- A value with a leading zero (`047`, `08`) is never numeric. Those are agency
  and FIPS codes, and typing them `bigint` would silently drop the padding.
- `t`/`f` and `YES`/`NO` columns are boolean, but `Y`/`N` columns stay text --
  which is exactly the split the reference database makes, on the grounds that a
  single-letter code domain can grow a third value.

It is still a sample. A column that is numeric in every row sampled but has one
stray value elsewhere in 11 GB will be typed too narrowly and fail the `COPY`,
so read the output -- especially the low-coverage columns it lists at the end --
before committing to a long load.

`assistance_schema.sql` in this directory was produced this way, from the FY2025
assistance download (112 columns, 20 indexes mirroring the contracts set).

### Proving the types before a long load

Sampling can only ever prove a type *wrong*. It cannot prove one right: a column
that is numeric in every row sampled may still hold one `NOT_AVAILABLE` somewhere
in the other 11 GB, and that single value fails the `COPY` after the load has
been running for twenty minutes. A partial fiscal year is where this bites most
often -- it has thinner columns and newer conventions than the years around it.

`--verify` closes that gap by reading every row of every file and checking each
value against the type its column was given:

```bash
python tools/csv2schema.py <dir> --table assistance --verify -o assistance_schema.sql
```

It names the file, line and value for each offender, so you can decide per
column. `--fix` decides for you, widening every contradicted column to `text`:

```
1 column(s) hold values their inferred type would reject, over 6 rows:
  federal_action_obligation (double precision)
      FY2026_part_1.csv line 2: 'NOT_AVAILABLE'
```

It also reports rows whose field count is wrong, which is the other way a
malformed file wrecks a load.

The cost is a full read at roughly 8 MiB/s: about 23 minutes for one year of
assistance, a few hours for seven. That is worth paying once, and it is cheaper
than discovering the problem at the end of a 75 GB load. If you would rather not
pay it in full, verify the years you trust least -- the newest and the partial
one -- and sample the rest more heavily with `--chunks 12 --sample-mb 64`.

## Loading several fiscal years into one table

The USAspending column set drifts between years, and csv2pg requires every file
in one run to share a header, so several years cannot be loaded in a single
invocation. The shape that works is one table per year, attached as partitions of
one parent.

Build a union schema across every year, plus the parent's DDL:

```bash
python tools/csv2schema.py /downloads/FY20*/ --recursive --union --verify --fix     --table assistance --force-text cfda_number,recipient_duns     --partition-by action_date_fiscal_year --parent-ddl assistance_parent.sql     --index action_date,recipient_uei,recipient_name,cfda_number     -o assistance_schema.sql
```

`--union` allows the headers to differ and takes the union of them, reporting
which years lack which columns. Every year then loads into that one schema, its
absent columns left NULL -- csv2pg only rejects CSV columns missing *from* the
schema, never schema columns missing from a CSV. The parent DDL goes in its own
file because a `PARTITION BY` clause would break the `--schema` parser.

Then load year by year:

```bash
export PGPASSWORD=...
tools/load_years.sh --schema assistance_schema.sql --parent-ddl assistance_parent.sql     --dbname usaspending --table assistance --csv2pg-args "--unlogged"     /downloads/FY2020 /downloads/FY2021 /downloads/FY2022 /downloads/FY2023     /downloads/FY2024 /downloads/FY2025 /downloads/FY2026
```

For each directory it takes the year from the first `FY<year>` in the path, loads
into `assistance_fy<year>`, sets the table LOGGED, adds a `CHECK` matching the
year, attaches it as a partition and analyzes it. Years already attached are
skipped, so a run that fails halfway can simply be repeated. `--dry-run` prints
every command without running any of them.

The `CHECK` is not just bookkeeping: it lets `ATTACH` skip its validation scan,
and it fails loudly if a year's files contain rows belonging to another fiscal
year instead of quietly mixing them in. Rows whose fiscal year matches no
attached partition land in `assistance_unrouted`, which is worth querying after a
load -- it should be empty.

Reloading one year afterwards is `DROP TABLE assistance_fy2026` followed by
another run, with the other six years untouched.

## Options

```
Connection (falls back to the standard PG* environment variables):
  --host H             default localhost, or $PGHOST
  --port P             default 5432, or $PGPORT
  --user U             default postgres, or $PGUSER
  --password P         default $PGPASSWORD
  --dbname D           database to create and load into (default contracts)
  --maintenance-db D   database used to issue CREATE DATABASE (default postgres)

Load:
  --table T         target table name (default: the schema's own table name)
  --schema FILE     read the column and index list from a pg_dump .sql at startup
                    instead of using the one compiled in
  --jobs N          parallel COPY connections (default: cores, capped at 8)
  --chunk-mb N      bytes handed to a worker per put (default 16)
  --read-mb N       reader block size (default 8)
  --truncate        TRUNCATE the table before loading
  --unlogged        create the table UNLOGGED (much faster, not crash-safe)
  --no-create-db    assume the database already exists
  --no-indexes      skip building indexes after the load
  --dry-run         validate CSV headers against the schema and exit
```

### Measured throughput

826 MiB / 400k real rows, local PostgreSQL 18, 12-core machine, indexes off:

| `--jobs` | logged table | |
|---|---|---|
| 1 | 10.0 MiB/s | 82.8s |
| 2 | 15.4 MiB/s | 53.6s |
| 4 | 21.5 MiB/s | 38.4s |
| 8 | 22.7 MiB/s | 36.4s |
| 8 + `--unlogged` | **102.2 MiB/s** | **8.1s** |

Two things to read out of that. Parallel connections are worth ~2.3× and flatten
out around 4 — past that the bottleneck stops being the COPY parser. And what it
becomes is WAL: dropping it takes the same load from 36s to 8s, a further 4.5×.

### Tuning notes

- `--unlogged` skips WAL for the load, which is by far the largest single win —
  see the table above. The table is not crash-safe while unlogged; run
  `ALTER TABLE contracts SET LOGGED` afterwards if you want it durable (that
  rewrites the table and its WAL, so it is not free).
- Loading with `--no-indexes` and building them in a second pass is already what
  happens by default — indexes are created after the data lands, never during.
- Raising `--jobs` past the number of cores on the *server* does not help; the
  connections just contend. On this hardware 4 was already at the knee.

### Recovering from a failed load

If a worker fails partway through, workers that had already committed leave their
rows behind, so the table can hold a partial load. Rerun with `--truncate`.

## Layout

```
src/csv2pg.cpp          the whole program
src/schema.h            generated column + index table (do not edit)
src/schema_sql.h        the runtime schema: the built-in one, or --schema's
tools/gen_schema.py     parses contracts_schema.sql -> src/schema.h
tools/stage_dlls.sh     copies libpq's DLLs next to the exe
tools/csv2schema.py     derives a schema from CSV data, for --schema
tools/load_years.sh     loads several years and attaches them as partitions
contracts_schema.sql    the pg_dump this was built from
assistance_schema.sql   derived schema for the FY2025 assistance download
testdata/sample.csv     20k real rows, for quick end-to-end runs
```
