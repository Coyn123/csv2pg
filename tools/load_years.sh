#!/bin/sh
# Load several fiscal years into one LIST-partitioned table.
#
# csv2pg refuses a folder whose files do not all share a header, and the
# USAspending column set drifts between years, so each year is loaded on its own
# into <table>_fy<year> and then attached as a partition. That is also what makes
# a year re-loadable later: drop the partition, load it again, attach it again.
#
#   PGPASSWORD=... tools/load_years.sh --schema assistance_schema.sql \
#       --parent-ddl assistance_parent.sql --dbname usaspending --table assistance \
#       /downloads/FY2020 /downloads/FY2021 ...
#
# The year comes from the first FY<year> in each directory's path. Years already
# attached are skipped, so a failed run can be repeated.
set -eu

EXE=./build/csv2pg.exe
SCHEMA=
PARENT_DDL=
DBNAME=
TABLE=
KEY=action_date_fiscal_year
DRY=0
PSQL=psql
CSV2PG_ARGS=
PASSWORD=
USERNAME=

usage() {
    cat >&2 <<'EOF'
usage: load_years.sh --schema FILE --dbname DB --table T [options] DIR [DIR...]

  --schema FILE       column schema passed to csv2pg --schema (required)
  --dbname DB         database to load into (required)
  --table T           partitioned parent table (required)
  --parent-ddl FILE   SQL creating the parent; run once if the parent is absent
  --key COL           partition key (default action_date_fiscal_year)
  --exe PATH          csv2pg binary (default ./build/csv2pg.exe)
  --psql PATH         psql binary (default psql)
  --csv2pg-args "..." extra flags for csv2pg, e.g. "--unlogged --jobs 4"
  --user U            exported as PGUSER. psql otherwise sends your OS account
                      name, which is rarely a Postgres role -- csv2pg defaults to
                      "postgres" on its own, so the two disagree without this.
  --password P        exported as PGPASSWORD for both csv2pg and psql. psql has
                      no password flag of its own, so this or a preset
                      PGPASSWORD is required -- otherwise psql stops to prompt
                      and the run hangs after the first year loads.
  --dry-run           print what would run, touch nothing

Connection settings come from the standard PG* environment variables, so set
PGHOST / PGUSER / PGPASSWORD once and both csv2pg and psql pick them up.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --schema) SCHEMA=$2; shift 2 ;;
        --parent-ddl) PARENT_DDL=$2; shift 2 ;;
        --dbname) DBNAME=$2; shift 2 ;;
        --table) TABLE=$2; shift 2 ;;
        --key) KEY=$2; shift 2 ;;
        --exe) EXE=$2; shift 2 ;;
        --psql) PSQL=$2; shift 2 ;;
        --csv2pg-args) CSV2PG_ARGS=$2; shift 2 ;;
        --password) PASSWORD=$2; shift 2 ;;
        --user) USERNAME=$2; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --*) echo "error: unknown option $1" >&2; usage; exit 2 ;;
        *) break ;;
    esac
done

[ -n "$SCHEMA" ] && [ -n "$DBNAME" ] && [ -n "$TABLE" ] || { usage; exit 2; }
[ $# -gt 0 ] || { echo "error: no input directories" >&2; exit 2; }
[ -f "$SCHEMA" ] || { echo "error: no such schema file: $SCHEMA" >&2; exit 2; }

if [ -n "$USERNAME" ]; then
    PGUSER=$USERNAME
    export PGUSER
fi
if [ -n "$PASSWORD" ]; then
    PGPASSWORD=$PASSWORD
    export PGPASSWORD
fi
# psql would sit at an unanswerable prompt in the middle of a long run.
if [ "$DRY" = 0 ] && [ -z "${PGPASSWORD:-}" ]; then
    echo "warning: no --password and no PGPASSWORD set; psql will prompt" >&2
fi

run() {
    if [ "$DRY" = 1 ]; then
        echo "+ $*"
    else
        echo "+ $*" >&2
        "$@"
    fi
}

# psql that fails the script on the first SQL error rather than plowing on.
sql() {
    if [ "$DRY" = 1 ]; then
        echo "+ $PSQL -d $DBNAME -c \"$1\""
    else
        echo "+ psql: $1" >&2
        "$PSQL" -v ON_ERROR_STOP=1 -q -d "$DBNAME" -c "$1"
    fi
}

sql_value() {
    if [ "$DRY" = 1 ]; then
        echo ""
    else
        "$PSQL" -v ON_ERROR_STOP=1 -tAq -d "$DBNAME" -c "$1"
    fi
}

# ---- the parent table -------------------------------------------------------
# csv2pg creates the database on its first load, so the parent cannot be built
# before then unless it already exists. Defer it to just before the first attach.
parent_ready=0
ensure_parent() {
    [ "$parent_ready" = 0 ] || return 0
    exists=$(sql_value "SELECT 1 FROM pg_class WHERE relname = '$TABLE' AND relkind = 'p'")
    if [ -z "$exists" ] && [ "$DRY" = 0 ]; then
        [ -n "$PARENT_DDL" ] || {
            echo "error: parent table $TABLE does not exist and no --parent-ddl given" >&2
            exit 1
        }
        echo "+ creating parent $TABLE from $PARENT_DDL" >&2
        "$PSQL" -v ON_ERROR_STOP=1 -q -d "$DBNAME" -f "$PARENT_DDL"
    elif [ "$DRY" = 1 ]; then
        echo "+ $PSQL -d $DBNAME -f ${PARENT_DDL:-<none>}   # if $TABLE is absent"
    fi
    parent_ready=1
}

for dir in "$@"; do
    [ -d "$dir" ] || { echo "error: not a directory: $dir" >&2; exit 2; }

    year=$(echo "$dir" | grep -oE 'FY[0-9]{4}' | head -1 | cut -c3-)
    [ -n "$year" ] || {
        echo "error: no FY<year> in path, cannot tell which year: $dir" >&2
        exit 2
    }
    child="${TABLE}_fy${year}"

    attached=$(sql_value "SELECT 1 FROM pg_inherits i
        JOIN pg_class c ON c.oid = i.inhrelid
        JOIN pg_class p ON p.oid = i.inhparent
        WHERE c.relname = '$child' AND p.relname = '$TABLE'")
    if [ -n "$attached" ]; then
        echo "== FY$year already attached as $child, skipping" >&2
        continue
    fi

    echo "== FY$year: loading $dir into $child" >&2
    # shellcheck disable=SC2086
    run "$EXE" --dbname "$DBNAME" --table "$child" --schema "$SCHEMA" $CSV2PG_ARGS "$dir"

    ensure_parent

    # An unlogged table cannot be a partition of a logged parent.
    sql "ALTER TABLE $child SET LOGGED"
    # A matching CHECK lets ATTACH skip its own validation scan, and catches a
    # year whose files hold rows from a different fiscal year before it is
    # silently mixed in.
    sql "ALTER TABLE $child ADD CONSTRAINT ${child}_ck CHECK ($KEY = $year)"
    sql "ALTER TABLE $TABLE ATTACH PARTITION $child FOR VALUES IN ($year)"
    sql "ANALYZE $child"
    echo "== FY$year done" >&2
done

echo "all years processed" >&2
