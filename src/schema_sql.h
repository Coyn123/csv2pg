// Runtime schema: the column and index list the loader works against.
//
// By default this is the table baked into schema.h at build time. Passing
// --schema <file.sql> replaces it with one parsed out of a pg_dump at startup,
// so a second dataset with different columns needs no rebuild -- the parsing
// rules here mirror tools/gen_schema.py exactly.
#pragma once

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "schema.h"

struct Column {
    std::string name;
    std::string sql_type;
    bool is_text;  // non-text columns need FORCE_NULL so quoted "" loads as NULL
};

// One index to rebuild after the load. `method` is the access method as the
// dump spelled it -- btree for the usual lookups, brin for a column the data is
// already ordered by, where it costs a rounding error of the btree's size.
struct IndexDef {
    std::string column;
    std::string method;
};

struct Schema {
    std::string table;
    std::vector<Column> columns;
    std::vector<IndexDef> indexes;
};

// The schema compiled into the binary.
inline Schema BuiltinSchema() {
    Schema s;
    s.table = schema::kDefaultTable;
    s.columns.reserve(size_t(schema::kColumnCount));
    for (int i = 0; i < schema::kColumnCount; ++i)
        s.columns.push_back({schema::kColumns[i].name, schema::kColumns[i].sql_type,
                             schema::kColumns[i].is_text});
    s.indexes.reserve(size_t(schema::kIndexColumnCount));
    for (int i = 0; i < schema::kIndexColumnCount; ++i)
        s.indexes.push_back({schema::kIndexColumns[i], "btree"});
    return s;
}

namespace schema_sql {

inline std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

inline bool IsTextType(const std::string& t) {
    std::string l = Lower(t);
    return l == "text" || l == "character varying" || l == "varchar" || l == "char" ||
           l == "character";
}

inline bool IdentChar(char c) {
    return std::isalnum((unsigned char)c) || c == '_';
}

inline std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Case-insensitive search for a whole word starting at or after `from`.
inline size_t FindWord(const std::string& hay, const std::string& needle_lower, size_t from) {
    const std::string& h = hay;
    for (size_t i = from; i + needle_lower.size() <= h.size(); ++i) {
        if (i && IdentChar(h[i - 1])) continue;
        bool hit = true;
        for (size_t j = 0; j < needle_lower.size() && hit; ++j) {
            char a = char(std::tolower((unsigned char)h[i + j]));
            char b = needle_lower[j];
            if (b == ' ') {  // one space in the pattern matches a run of whitespace
                if (!std::isspace((unsigned char)h[i + j])) hit = false;
            } else if (a != b) {
                hit = false;
            }
        }
        if (hit) return i;
    }
    return std::string::npos;
}

// Skips whitespace, then reads an identifier, dropping a leading "public.".
inline std::string ReadIdent(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    size_t b = i;
    while (i < s.size() && (IdentChar(s[i]) || s[i] == '.' || s[i] == '"')) ++i;
    std::string id = s.substr(b, i - b);
    if (Lower(id).rfind("public.", 0) == 0) id = id.substr(7);
    std::string out;
    for (char c : id)
        if (c != '"') out += c;
    return out;
}

// Parses the first CREATE TABLE body plus every CREATE INDEX ... USING btree.
// Returns false with `err` set if the file has no CREATE TABLE.
inline bool Parse(const std::string& sql, const std::string& path, Schema& out, std::string& err) {
    size_t ct = FindWord(sql, "create table", 0);
    if (ct == std::string::npos) {
        err = "no CREATE TABLE found in " + path;
        return false;
    }
    size_t i = ct + 12;
    out.table = ReadIdent(sql, i);
    if (out.table.empty()) {
        err = "CREATE TABLE in " + path + " has no table name";
        return false;
    }
    size_t open = sql.find('(', i);
    if (open == std::string::npos) {
        err = "CREATE TABLE " + out.table + " in " + path + " has no column list";
        return false;
    }
    // The body ends at a line that is exactly ");", the shape pg_dump emits.
    size_t close = sql.find("\n);", open);
    if (close == std::string::npos) {
        err = "CREATE TABLE " + out.table + " in " + path + " is not terminated by \"\n);\"";
        return false;
    }

    std::string body = sql.substr(open + 1, close - open - 1);
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        std::string line = Trim(body.substr(pos, (nl == std::string::npos ? body.size() : nl) - pos));
        pos = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        while (!line.empty() && line.back() == ',') line.pop_back();
        line = Trim(line);
        if (line.empty() || line.rfind("--", 0) == 0) continue;
        size_t sp = 0;
        while (sp < line.size() && !std::isspace((unsigned char)line[sp])) ++sp;
        if (sp >= line.size()) continue;  // no type on this line -- not a column
        std::string name = line.substr(0, sp);
        std::string type = Trim(line.substr(sp));
        if (type.empty()) continue;
        out.columns.push_back({name, type, IsTextType(type)});
    }
    if (out.columns.empty()) {
        err = "CREATE TABLE " + out.table + " in " + path + " declares no columns";
        return false;
    }

    // CREATE INDEX <name> ON [public.]<table> USING <method> (<col>)
    size_t at = 0;
    while ((at = FindWord(sql, "create index", at)) != std::string::npos) {
        at += 12;
        size_t us = FindWord(sql, "using", at);
        if (us == std::string::npos) break;
        size_t mi = us + 5;
        std::string method = Lower(ReadIdent(sql, mi));
        // Only the methods that take a plain column list the same way. Anything
        // else (gin, gist with an opclass, ...) is left to the reference dump.
        if (method != "btree" && method != "brin" && method != "hash") {
            at = mi;
            continue;
        }
        size_t bt = mi;
        size_t p = sql.find('(', bt);
        size_t q = (p == std::string::npos) ? std::string::npos : sql.find(')', p);
        if (q == std::string::npos) break;
        std::string col = Trim(sql.substr(p + 1, q - p - 1));
        at = q;
        std::string clean;
        for (char c : col)
            if (c != '"') clean += c;
        // Only single-column btree indexes are reproduced. Multi-column ones and
        // expressions (lower(x), a DESC, ...) are left to the reference dump.
        bool bare = !clean.empty();
        for (char c : clean)
            if (!IdentChar(c)) bare = false;
        if (!bare) continue;
        bool dup = false;
        for (const auto& e : out.indexes)
            if (e.column == clean && e.method == method) dup = true;
        if (!dup) out.indexes.push_back({clean, method});
    }
    return true;
}

// Reads and parses a .sql file. Reports to stderr and returns false on failure.
inline bool LoadFile(const std::string& path, Schema& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "error: cannot open schema file %s\n", path.c_str());
        return false;
    }
    std::string sql;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) sql.append(buf, n);
    std::fclose(f);

    Schema s;
    std::string err;
    if (!schema_sql::Parse(sql, path, s, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return false;
    }
    out = std::move(s);
    return true;
}

}  // namespace schema_sql
