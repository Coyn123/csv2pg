// csv2pg -- bulk-load a USAspending contracts CSV into PostgreSQL.
//
// Usage: csv2pg <path-to.csv | directory-of-csvs> [options]
//
// Threading model. The job is I/O bound at both ends, so threads are used only
// where they actually buy throughput:
//
//   * ONE reader thread walks the input sequentially. Disk reads are fastest in
//     one big sequential stream; splitting the read would just make the drive
//     seek. It scans for record boundaries (respecting CSV quoting) and hands
//     off large byte chunks -- it never parses fields.
//   * N worker threads each hold their own libpq connection with a single
//     COPY ... FROM STDIN open for the whole run, and stream raw chunk bytes
//     into it. The server's COPY parser is single-core per connection, so this
//     is the one place parallelism genuinely multiplies throughput.
//   * A bounded queue between them provides backpressure, so a slow server
//     cannot make the reader eat all of RAM.
//   * Indexes are built after the load, in parallel across connections.
//
// The CSV bytes are never parsed field-by-field in this process -- only split on
// record boundaries -- which keeps the client side essentially free.

#include <libpq-fe.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "schema_sql.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------- options ---

struct Options {
    std::string input;
    std::string host = "localhost";
    std::string port = "5432";
    std::string user = "postgres";
    std::string password;
    std::string dbname = "contracts";
    std::string maintenance_db = "postgres";
    std::string table;                     // empty => the schema's own table name
    std::string schema_file;               // empty => the schema compiled in
    int jobs = 0;                          // 0 => auto
    size_t chunk_bytes = 16u << 20;        // 16 MiB
    size_t read_block = 8u << 20;          // 8 MiB
    bool create_db = true;
    bool create_indexes = true;
    bool truncate = false;
    bool unlogged = false;
    bool dry_run = false;
};

static void PrintUsage() {
    std::fprintf(stderr,
        "csv2pg -- parallel COPY loader for USAspending contract CSVs\n"
        "\n"
        "Usage:\n"
        "  csv2pg <file.csv|directory> [options]\n"
        "\n"
        "The single positional argument is the data source: one .csv file, or a\n"
        "directory, in which case every *.csv inside it is loaded in name order.\n"
        "\n"
        "Connection (falls back to the standard PG* environment variables):\n"
        "  --host H             default localhost, or $PGHOST\n"
        "  --port P             default 5432, or $PGPORT\n"
        "  --user U             default postgres, or $PGUSER\n"
        "  --password P         default $PGPASSWORD\n"
        "  --dbname D           database to create and load into (default contracts)\n"
        "  --maintenance-db D   database used to issue CREATE DATABASE (default postgres)\n"
        "\n"
        "Load:\n"
        "  --table T         target table name (default: the schema's, %s)\n"
        "  --schema FILE     read the column and index list from a pg_dump .sql at\n"
        "                    startup instead of using the one compiled in\n"
        "  --jobs N          parallel COPY connections (default: cores, capped at 8)\n"
        "  --chunk-mb N      bytes handed to a worker per put (default 16)\n"
        "  --read-mb N       reader block size (default 8)\n"
        "  --truncate        TRUNCATE the table before loading\n"
        "  --unlogged        create the table UNLOGGED (much faster, not crash-safe)\n"
        "  --no-create-db    assume the database already exists\n"
        "  --no-indexes      skip building indexes after the load\n"
        "  --dry-run         validate CSV headers against the schema and exit\n"
        "  -h, --help        this message\n",
        schema::kDefaultTable);
}

static bool NeedsValue(const char* flag, int i, int argc) {
    if (i + 1 < argc) return true;
    std::fprintf(stderr, "error: %s requires a value\n", flag);
    return false;
}

static bool ParseArgs(int argc, char** argv, Options& o) {
    if (const char* e = std::getenv("PGHOST")) o.host = e;
    if (const char* e = std::getenv("PGPORT")) o.port = e;
    if (const char* e = std::getenv("PGUSER")) o.user = e;
    if (const char* e = std::getenv("PGPASSWORD")) o.password = e;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { PrintUsage(); std::exit(0); }
        else if (a == "--host") { if (!NeedsValue("--host", i, argc)) return false; o.host = argv[++i]; }
        else if (a == "--port") { if (!NeedsValue("--port", i, argc)) return false; o.port = argv[++i]; }
        else if (a == "--user") { if (!NeedsValue("--user", i, argc)) return false; o.user = argv[++i]; }
        else if (a == "--password") { if (!NeedsValue("--password", i, argc)) return false; o.password = argv[++i]; }
        else if (a == "--dbname") { if (!NeedsValue("--dbname", i, argc)) return false; o.dbname = argv[++i]; }
        else if (a == "--maintenance-db") { if (!NeedsValue("--maintenance-db", i, argc)) return false; o.maintenance_db = argv[++i]; }
        else if (a == "--table") { if (!NeedsValue("--table", i, argc)) return false; o.table = argv[++i]; }
        else if (a == "--schema") { if (!NeedsValue("--schema", i, argc)) return false; o.schema_file = argv[++i]; }
        else if (a == "--jobs") { if (!NeedsValue("--jobs", i, argc)) return false; o.jobs = std::atoi(argv[++i]); }
        else if (a == "--chunk-mb") { if (!NeedsValue("--chunk-mb", i, argc)) return false; o.chunk_bytes = size_t(std::atoi(argv[++i])) << 20; }
        else if (a == "--read-mb") { if (!NeedsValue("--read-mb", i, argc)) return false; o.read_block = size_t(std::atoi(argv[++i])) << 20; }
        else if (a == "--truncate") o.truncate = true;
        else if (a == "--unlogged") o.unlogged = true;
        else if (a == "--no-create-db") o.create_db = false;
        else if (a == "--no-indexes") o.create_indexes = false;
        else if (a == "--dry-run") o.dry_run = true;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", a.c_str());
            return false;
        } else if (o.input.empty()) {
            o.input = a;
        } else {
            std::fprintf(stderr, "error: unexpected extra argument %s\n", a.c_str());
            return false;
        }
    }
    if (o.input.empty()) { PrintUsage(); return false; }
    if (o.jobs <= 0) {
        unsigned hw = std::thread::hardware_concurrency();
        o.jobs = int(std::min(hw ? hw : 4u, 8u));
    }
    if (o.chunk_bytes < (1u << 20)) o.chunk_bytes = 1u << 20;
    if (o.read_block < (1u << 20)) o.read_block = 1u << 20;
    return true;
}

// ------------------------------------------------------------- SQL helpers ---

static std::string QuoteIdent(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

static std::string QuoteLiteral(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += '\'';
        out += c;
    }
    out += '\'';
    return out;
}

static std::string ConnInfo(const Options& o, const std::string& db) {
    auto kv = [](const char* k, const std::string& v) {
        std::string out = k;
        out += "='";
        for (char c : v) {
            if (c == '\'' || c == '\\') out += '\\';
            out += c;
        }
        out += "' ";
        return out;
    };
    std::string s;
    s += kv("host", o.host);
    s += kv("port", o.port);
    s += kv("user", o.user);
    if (!o.password.empty()) s += kv("password", o.password);
    s += kv("dbname", db);
    s += "client_encoding='UTF8' application_name='csv2pg'";
    return s;
}

// Runs a statement, reporting and returning false on failure.
static bool Exec(PGconn* c, const std::string& sql, std::string* err = nullptr) {
    PGresult* r = PQexec(c, sql.c_str());
    ExecStatusType st = PQresultStatus(r);
    bool ok = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK);
    if (!ok) {
        std::string msg = PQerrorMessage(c);
        if (err) *err = msg;
        else std::fprintf(stderr, "error: %s  while running: %s\n", msg.c_str(), sql.c_str());
    }
    PQclear(r);
    return ok;
}

static PGconn* Connect(const Options& o, const std::string& db) {
    PGconn* c = PQconnectdb(ConnInfo(o, db).c_str());
    if (PQstatus(c) != CONNECTION_OK) {
        std::fprintf(stderr, "error: could not connect to database \"%s\": %s",
                     db.c_str(), PQerrorMessage(c));
        PQfinish(c);
        return nullptr;
    }
    return c;
}

// ------------------------------------------------------------ CSV plumbing ---

// Splits one CSV record into fields. Used only for the header line.
static std::vector<std::string> ParseCsvRecord(std::string_view rec) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quote = false;
    for (size_t i = 0; i < rec.size(); ++i) {
        char c = rec[i];
        if (in_quote) {
            if (c == '"') {
                if (i + 1 < rec.size() && rec[i + 1] == '"') { cur += '"'; ++i; }
                else in_quote = false;
            } else {
                cur += c;
            }
        } else if (c == '"') {
            in_quote = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r' && c != '\n') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

// Lowercases and drops everything that is not a letter or digit, so a CSV
// header can be compared against the column name the reference database gave
// it after sanitizing (COVID-19 -> covid_19, a-76 -> a76, and so on).
static std::string Squash(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (std::isalnum(u)) out += char(std::tolower(u));
    }
    return out;
}

static std::string NormalizeHeader(std::string s) {
    size_t b = s.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    s = s.substr(b, e - b + 1);
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// Bounded chunk queue. Push blocks while full; Pop blocks while empty.
class ChunkQueue {
public:
    explicit ChunkQueue(size_t capacity) : capacity_(capacity) {}

    bool Push(std::vector<char>&& chunk) {
        std::unique_lock<std::mutex> lk(m_);
        not_full_.wait(lk, [&] { return q_.size() < capacity_ || aborted_; });
        if (aborted_) return false;
        q_.push_back(std::move(chunk));
        not_empty_.notify_one();
        return true;
    }

    bool Pop(std::vector<char>& out) {
        std::unique_lock<std::mutex> lk(m_);
        not_empty_.wait(lk, [&] { return !q_.empty() || closed_ || aborted_; });
        if (aborted_ || q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        not_full_.notify_one();
        return true;
    }

    void Close() {
        { std::lock_guard<std::mutex> lk(m_); closed_ = true; }
        not_empty_.notify_all();
    }

    void Abort() {
        { std::lock_guard<std::mutex> lk(m_); aborted_ = true; }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool aborted() {
        std::lock_guard<std::mutex> lk(m_);
        return aborted_;
    }

private:
    std::mutex m_;
    std::condition_variable not_empty_, not_full_;
    std::deque<std::vector<char>> q_;
    size_t capacity_;
    bool closed_ = false;
    bool aborted_ = false;
};

// Collects the first error reported by any thread.
class ErrorSink {
public:
    void Set(const std::string& msg) {
        std::lock_guard<std::mutex> lk(m_);
        if (first_.empty()) first_ = msg;
        failed_.store(true, std::memory_order_relaxed);
    }
    bool failed() const { return failed_.load(std::memory_order_relaxed); }
    std::string first() {
        std::lock_guard<std::mutex> lk(m_);
        return first_;
    }

private:
    std::mutex m_;
    std::string first_;
    std::atomic<bool> failed_{false};
};

// ----------------------------------------------------------------- reader ---

struct ReadResult {
    bool ok = false;
    unsigned long long bytes = 0;
};

// Reads the first record of a file (the header). Everything already read past
// the header is left in `carry` so the caller can continue from there.
static bool ReadHeader(std::FILE* f, size_t block, std::vector<std::string>& header,
                       std::vector<char>& carry, unsigned long long& consumed) {
    std::vector<char> buf(block);
    carry.clear();
    bool in_quote = false;
    size_t scanned = 0;
    while (true) {
        for (size_t i = scanned; i < carry.size(); ++i) {
            char c = carry[i];
            if (c == '"') in_quote = !in_quote;
            else if (c == '\n' && !in_quote) {
                header = ParseCsvRecord(std::string_view(carry.data(), i));
                carry.erase(carry.begin(), carry.begin() + long(i) + 1);
                return true;
            }
        }
        scanned = carry.size();
        size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) {
            if (carry.empty()) return false;
            header = ParseCsvRecord(std::string_view(carry.data(), carry.size()));
            carry.clear();
            return true;
        }
        consumed += n;
        carry.insert(carry.end(), buf.begin(), buf.begin() + long(n));
    }
}

// Splits the input stream on record boundaries and feeds the queue.
// Every file after the first must carry an identical header, which is dropped.
static ReadResult ReaderMain(const Options& opt, const std::vector<fs::path>& files,
                             const std::vector<std::string>& expect_header,
                             ChunkQueue& q, ErrorSink& err,
                             std::atomic<unsigned long long>& progress) {
    ReadResult res;
    std::vector<char> block(opt.read_block);

    for (const fs::path& p : files) {
        std::FILE* f = std::fopen(p.string().c_str(), "rb");
        if (!f) {
            err.Set("could not open " + p.string());
            q.Abort();
            return res;
        }

        std::vector<char> acc;
        acc.reserve(opt.chunk_bytes + opt.read_block + 64);
        unsigned long long consumed = 0;
        std::vector<std::string> header;
        if (!ReadHeader(f, opt.read_block, header, acc, consumed)) {
            std::fclose(f);
            continue;  // empty file
        }
        progress.fetch_add(consumed, std::memory_order_relaxed);
        res.bytes += consumed;

        std::vector<std::string> norm;
        norm.reserve(header.size());
        for (const auto& h : header) norm.push_back(NormalizeHeader(h));
        if (norm != expect_header) {
            err.Set("header of " + p.filename().string() + " does not match the first file's header");
            std::fclose(f);
            q.Abort();
            return res;
        }

        // Boundary scan state, carried across reads.
        bool in_quote = false;
        size_t scanned = 0;    // bytes of acc already examined
        size_t last_nl = 0;    // one past the last record-terminating newline

        auto scan = [&] {
            for (size_t i = scanned; i < acc.size(); ++i) {
                char c = acc[i];
                if (c == '"') in_quote = !in_quote;
                else if (c == '\n' && !in_quote) last_nl = i + 1;
            }
            scanned = acc.size();
        };

        bool eof = false;
        while (!eof) {
            size_t n = std::fread(block.data(), 1, block.size(), f);
            if (n == 0) {
                eof = true;
            } else {
                acc.insert(acc.end(), block.begin(), block.begin() + long(n));
                progress.fetch_add(n, std::memory_order_relaxed);
                res.bytes += n;
            }
            scan();

            while (acc.size() >= opt.chunk_bytes && last_nl > 0) {
                std::vector<char> chunk(acc.begin(), acc.begin() + long(last_nl));
                acc.erase(acc.begin(), acc.begin() + long(last_nl));
                scanned -= last_nl;
                last_nl = 0;
                if (!q.Push(std::move(chunk))) { std::fclose(f); return res; }
            }
            if (err.failed()) { std::fclose(f); q.Abort(); return res; }
        }

        // Flush the tail, adding a terminator if the file lacked one.
        if (!acc.empty()) {
            if (acc.back() != '\n') acc.push_back('\n');
            if (!q.Push(std::move(acc))) { std::fclose(f); return res; }
        }
        std::fclose(f);
    }

    res.ok = true;
    return res;
}

// ---------------------------------------------------------------- workers ---

static void WorkerMain(const Options& opt, const std::string& copy_sql, ChunkQueue& q,
                       ErrorSink& err, std::atomic<long long>& rows_total) {
    PGconn* c = Connect(opt, opt.dbname);
    if (!c) { err.Set("worker could not connect"); q.Abort(); return; }

    std::string e;
    // Per-connection load tuning; session-local, nothing persists.
    Exec(c, "SET synchronous_commit TO off", &e);
    Exec(c, "SET client_min_messages TO warning", &e);

    PGresult* r = PQexec(c, copy_sql.c_str());
    if (PQresultStatus(r) != PGRES_COPY_IN) {
        err.Set(std::string("COPY did not start: ") + PQerrorMessage(c));
        PQclear(r);
        PQfinish(c);
        q.Abort();
        return;
    }
    PQclear(r);

    std::vector<char> chunk;
    while (q.Pop(chunk)) {
        if (PQputCopyData(c, chunk.data(), int(chunk.size())) != 1) {
            err.Set(std::string("COPY write failed: ") + PQerrorMessage(c));
            PQfinish(c);
            q.Abort();
            return;
        }
    }

    if (q.aborted()) {
        PQputCopyEnd(c, "aborted by csv2pg");
        while (PGresult* rr = PQgetResult(c)) PQclear(rr);
        PQfinish(c);
        return;
    }

    if (PQputCopyEnd(c, nullptr) != 1) {
        err.Set(std::string("COPY end failed: ") + PQerrorMessage(c));
        PQfinish(c);
        q.Abort();
        return;
    }

    long long rows = 0;
    bool ok = true;
    while (PGresult* rr = PQgetResult(c)) {
        if (PQresultStatus(rr) != PGRES_COMMAND_OK) {
            err.Set(std::string("COPY failed: ") + PQerrorMessage(c));
            ok = false;
        } else {
            const char* t = PQcmdTuples(rr);
            if (t && *t) rows += std::atoll(t);
        }
        PQclear(rr);
    }
    if (!ok) q.Abort();
    rows_total.fetch_add(rows, std::memory_order_relaxed);
    PQfinish(c);
}

// ---------------------------------------------------------------- indexes ---

static void IndexWorker(const Options& opt, const std::vector<std::string>& stmts,
                        std::atomic<size_t>& next, ErrorSink& err) {
    PGconn* c = Connect(opt, opt.dbname);
    if (!c) { err.Set("index worker could not connect"); return; }
    std::string e;
    Exec(c, "SET maintenance_work_mem TO '1GB'", &e);
    Exec(c, "SET client_min_messages TO warning", &e);
    while (true) {
        size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= stmts.size() || err.failed()) break;
        if (!Exec(c, stmts[i], &e)) err.Set(e);
    }
    PQfinish(c);
}

// ------------------------------------------------------------------- main ---

static std::string HumanBytes(double b) {
    const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    while (b >= 1024.0 && i < 4) { b /= 1024.0; ++i; }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f %s", b, u[i]);
    return buf;
}

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) return 2;

    // ---- gather input files -------------------------------------------------
    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_directory(opt.input, ec)) {
        for (const auto& de : fs::directory_iterator(opt.input, ec)) {
            if (!de.is_regular_file()) continue;
            std::string ext = de.path().extension().string();
            for (char& ch : ext) ch = char(std::tolower((unsigned char)ch));
            if (ext == ".csv") files.push_back(de.path());
        }
        std::sort(files.begin(), files.end());
    } else if (fs::is_regular_file(opt.input, ec)) {
        files.push_back(opt.input);
    }
    if (files.empty()) {
        std::fprintf(stderr, "error: no CSV files found at \"%s\"\n", opt.input.c_str());
        return 2;
    }

    unsigned long long total_bytes = 0;
    for (const auto& p : files) total_bytes += fs::file_size(p, ec);
    std::fprintf(stderr, "csv2pg: %zu file(s), %s to load\n", files.size(),
                 HumanBytes(double(total_bytes)).c_str());

    // ---- pick the schema ----------------------------------------------------
    // Either the one compiled into the binary or, with --schema, one parsed out
    // of a pg_dump at startup. Everything below works off this object, so a
    // second dataset with a different column set needs no rebuild.
    Schema sch = BuiltinSchema();
    if (!opt.schema_file.empty()) {
        if (!schema_sql::LoadFile(opt.schema_file, sch)) return 1;
        size_t nbrin = 0;
        for (const auto& ix : sch.indexes)
            if (ix.method != "btree") ++nbrin;
        std::fprintf(stderr, "csv2pg: schema %s: table %s, %zu columns, %zu indexes (%zu non-btree)\n",
                     opt.schema_file.c_str(), sch.table.c_str(), sch.columns.size(),
                     sch.indexes.size(), nbrin);
    }
    if (opt.table.empty()) opt.table = sch.table;
    const int ncols = int(sch.columns.size());

    // ---- validate the header against the schema -----------------------------
    std::vector<std::string> header;
    {
        std::FILE* f = std::fopen(files[0].string().c_str(), "rb");
        if (!f) { std::fprintf(stderr, "error: cannot open %s\n", files[0].string().c_str()); return 1; }
        std::vector<char> carry;
        unsigned long long consumed = 0;
        std::vector<std::string> raw;
        bool got = ReadHeader(f, opt.read_block, raw, carry, consumed);
        std::fclose(f);
        if (!got) { std::fprintf(stderr, "error: %s is empty\n", files[0].string().c_str()); return 1; }
        for (const auto& h : raw) header.push_back(NormalizeHeader(h));
    }

    // Map each CSV column onto a schema column. Three tiers, most exact first:
    // the literal name, the punctuation-insensitive name, and the same again
    // with the leading "c" the reference database prefixes onto names that
    // would otherwise start with a digit (1862_land_grant_college).
    std::vector<std::string> unknown, force_null, target_cols, renamed;
    std::vector<bool> claimed(size_t(ncols), false);
    for (const auto& h : header) {
        std::string sq = Squash(h);
        int found = -1;
        for (int i = 0; i < ncols && found < 0; ++i)
            if (h == sch.columns[size_t(i)].name) found = i;
        for (int i = 0; i < ncols && found < 0; ++i)
            if (sq == Squash(sch.columns[size_t(i)].name)) found = i;
        for (int i = 0; i < ncols && found < 0; ++i)
            if ("c" + sq == Squash(sch.columns[size_t(i)].name)) found = i;

        if (found < 0) { unknown.push_back(h); continue; }
        if (claimed[size_t(found)]) {
            std::fprintf(stderr, "error: CSV column \"%s\" maps to \"%s\", already taken\n",
                         h.c_str(), sch.columns[size_t(found)].name.c_str());
            return 1;
        }
        claimed[size_t(found)] = true;
        const Column& col = sch.columns[size_t(found)];
        target_cols.push_back(col.name);
        if (!col.is_text) force_null.push_back(col.name);
        if (h != col.name) renamed.push_back(h + " -> " + col.name);
    }
    if (!unknown.empty()) {
        std::fprintf(stderr, "error: %zu CSV column(s) are not in the schema:\n", unknown.size());
        for (const auto& u : unknown) std::fprintf(stderr, "  %s\n", u.c_str());
        std::fprintf(stderr, "Point --schema at a dump that includes them, or regenerate\n"
                             "src/schema.h and rebuild.\n");
        return 1;
    }
    std::fprintf(stderr, "csv2pg: header matches schema (%zu of %d columns present)\n",
                 target_cols.size(), ncols);
    for (const auto& r : renamed) std::fprintf(stderr, "  column mapped: %s\n", r.c_str());
    if (opt.dry_run) return 0;

    // ---- create the database ------------------------------------------------
    if (opt.create_db) {
        PGconn* c = Connect(opt, opt.maintenance_db);
        if (!c) return 1;
        std::string sql = "SELECT 1 FROM pg_database WHERE datname = " + QuoteLiteral(opt.dbname);
        PGresult* r = PQexec(c, sql.c_str());
        bool exists = (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0);
        PQclear(r);
        if (!exists) {
            std::fprintf(stderr, "csv2pg: creating database %s\n", opt.dbname.c_str());
            if (!Exec(c, "CREATE DATABASE " + QuoteIdent(opt.dbname))) { PQfinish(c); return 1; }
        }
        PQfinish(c);
    }

    // ---- create the table ---------------------------------------------------
    {
        PGconn* c = Connect(opt, opt.dbname);
        if (!c) return 1;
        std::string ddl = "CREATE ";
        if (opt.unlogged) ddl += "UNLOGGED ";
        ddl += "TABLE IF NOT EXISTS " + QuoteIdent(opt.table) + " (\n";
        for (int i = 0; i < ncols; ++i) {
            ddl += "  " + QuoteIdent(sch.columns[size_t(i)].name) + " " + sch.columns[size_t(i)].sql_type;
            if (i + 1 < ncols) ddl += ",";
            ddl += "\n";
        }
        ddl += ")";
        if (!Exec(c, ddl)) { PQfinish(c); return 1; }
        if (opt.truncate && !Exec(c, "TRUNCATE " + QuoteIdent(opt.table))) { PQfinish(c); return 1; }
        PQfinish(c);
    }

    // ---- build the COPY statement ------------------------------------------
    std::string cols, fnull;
    for (size_t i = 0; i < target_cols.size(); ++i) {
        if (i) cols += ", ";
        cols += QuoteIdent(target_cols[i]);
    }
    for (size_t i = 0; i < force_null.size(); ++i) {
        if (i) fnull += ", ";
        fnull += QuoteIdent(force_null[i]);
    }
    std::string copy_sql = "COPY " + QuoteIdent(opt.table) + " (" + cols +
                           ") FROM STDIN WITH (FORMAT csv";
    // A quoted empty string is a legitimate '' for text, but has to become NULL
    // for dates, numerics and booleans or the load dies on the first blank.
    if (!fnull.empty()) copy_sql += ", FORCE_NULL (" + fnull + ")";
    copy_sql += ")";

    // ---- load ---------------------------------------------------------------
    std::fprintf(stderr, "csv2pg: loading into %s.%s with %d parallel COPY connections\n",
                 opt.dbname.c_str(), opt.table.c_str(), opt.jobs);

    ChunkQueue queue(size_t(opt.jobs) * 2 + 2);
    ErrorSink err;
    std::atomic<unsigned long long> progress{0};
    std::atomic<long long> rows{0};
    std::atomic<bool> running{true};

    std::vector<std::thread> workers;
    workers.reserve(size_t(opt.jobs));
    for (int i = 0; i < opt.jobs; ++i)
        workers.emplace_back(WorkerMain, std::cref(opt), std::cref(copy_sql), std::ref(queue),
                             std::ref(err), std::ref(rows));

    auto t0 = std::chrono::steady_clock::now();
    std::thread reporter([&] {
        while (running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!running.load(std::memory_order_relaxed)) break;
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            unsigned long long done = progress.load(std::memory_order_relaxed);
            double pct = total_bytes ? 100.0 * double(done) / double(total_bytes) : 0.0;
            std::fprintf(stderr, "\r  %s / %s (%.1f%%)  %.1f MiB/s   ",
                         HumanBytes(double(done)).c_str(), HumanBytes(double(total_bytes)).c_str(),
                         pct, secs > 0 ? double(done) / secs / 1048576.0 : 0.0);
            std::fflush(stderr);
        }
    });

    ReadResult rr = ReaderMain(opt, files, header, queue, err, progress);
    queue.Close();
    for (auto& t : workers) t.join();
    running.store(false, std::memory_order_relaxed);
    reporter.join();
    std::fprintf(stderr, "\r%*s\r", 70, "");

    double load_secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (err.failed() || !rr.ok) {
        std::fprintf(stderr, "csv2pg: load FAILED: %s\n", err.first().c_str());
        return 1;
    }
    std::fprintf(stderr, "csv2pg: loaded %lld rows (%s) in %.1fs -- %.1f MiB/s\n",
                 rows.load(), HumanBytes(double(rr.bytes)).c_str(), load_secs,
                 load_secs > 0 ? double(rr.bytes) / load_secs / 1048576.0 : 0.0);

    // ---- indexes ------------------------------------------------------------
    if (opt.create_indexes && !sch.indexes.empty()) {
        std::vector<std::string> stmts;
        for (size_t i = 0; i < sch.indexes.size(); ++i) {
            const std::string& col = sch.indexes[i].column;
            const std::string& method = sch.indexes[i].method;
            // btree keeps the bare name it has always had; anything else is
            // suffixed so both can index the same column without colliding.
            std::string name = "ix_" + opt.table + "_" + col;
            if (method != "btree") name += "_" + method;
            stmts.push_back("CREATE INDEX IF NOT EXISTS " + QuoteIdent(name) + " ON " +
                            QuoteIdent(opt.table) + " USING " + method +
                            " (" + QuoteIdent(col) + ")");
        }
        std::fprintf(stderr, "csv2pg: building %zu indexes on %d connections\n", stmts.size(), opt.jobs);
        auto ti = std::chrono::steady_clock::now();
        std::atomic<size_t> next{0};
        std::vector<std::thread> ix;
        int n = std::min<int>(opt.jobs, int(stmts.size()));
        for (int i = 0; i < n; ++i)
            ix.emplace_back(IndexWorker, std::cref(opt), std::cref(stmts), std::ref(next), std::ref(err));
        for (auto& t : ix) t.join();
        if (err.failed()) {
            std::fprintf(stderr, "csv2pg: index build FAILED: %s\n", err.first().c_str());
            return 1;
        }
        std::fprintf(stderr, "csv2pg: indexes built in %.1fs\n",
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - ti).count());

        PGconn* c = Connect(opt, opt.dbname);
        if (c) {
            std::fprintf(stderr, "csv2pg: analyzing\n");
            Exec(c, "ANALYZE " + QuoteIdent(opt.table));
            PQfinish(c);
        }
    }

    std::fprintf(stderr, "csv2pg: done in %.1fs total\n",
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}
