# csv2pg -- build with the MSYS2 UCRT64 toolchain.
#
#   make            build build/csv2pg.exe and copy its runtime DLLs beside it
#   make dist       zip build/ into dist/csv2pg-win64.zip for handing off
#   make schema     regenerate src/schema.h from contracts_schema.sql
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -pipe -Wall -Wextra -Wno-unused-parameter
# Fold the GCC runtime into the exe so only libpq's DLLs have to travel with it.
LDFLAGS  ?= -static-libgcc -static-libstdc++
LDLIBS   ?= -lpq

PG_INC   ?= C:/msys64/ucrt64/include
PG_LIB   ?= C:/msys64/ucrt64/lib
DLL_DIR  ?= C:/msys64/ucrt64/bin

BUILD := build
BIN   := $(BUILD)/csv2pg.exe
SRC   := src/csv2pg.cpp
HDR   := src/schema.h src/schema_sql.h

.PHONY: all schema clean deps dist

all: $(BIN) deps

# GCC needs a writable temp dir; when make is launched from a shell that has no
# TMPDIR it falls back to C:\WINDOWS and fails, so point it somewhere local.
export TMPDIR := $(CURDIR)/$(BUILD)/tmp
export TMP    := $(CURDIR)/$(BUILD)/tmp
export TEMP   := $(CURDIR)/$(BUILD)/tmp

$(BIN): $(SRC) $(HDR) | $(BUILD)/tmp
	$(CXX) $(CXXFLAGS) -I$(PG_INC) -Isrc $(SRC) -o $@ -L$(PG_LIB) $(LDFLAGS) $(LDLIBS)

$(BUILD)/tmp:
	mkdir -p $(BUILD)/tmp

# Walk the import tables and copy every DLL that comes from the MSYS2 prefix
# next to the exe, so it runs anywhere without MSYS2 on PATH. System DLLs are
# left alone. Repeats until the set stops growing (libpq pulls in ssl, icu, ...).
deps: $(BIN)
	@sh tools/stage_dlls.sh "$(BIN)" "$(BUILD)" "$(DLL_DIR)"

# Zips the self-contained build/ folder (exe + DLLs) for handing to a machine
# that has neither MSYS2 nor a compiler. That folder is the whole product;
# nothing else needs to travel with it.
dist: all
	mkdir -p dist
	powershell -NoProfile -Command "Compress-Archive -Path '$(BUILD)/*.exe','$(BUILD)/*.dll' -DestinationPath 'dist/csv2pg-win64.zip' -Force"
	@echo "packaged dist/csv2pg-win64.zip"

schema:
	python tools/gen_schema.py

clean:
	rm -rf $(BUILD)
