# Princess Peach Showtime Local Co-op public release build configuration.
LOAD_KIND := Module
PROGRAM_ID := 01007A3009184000
ELF_EXTRACT :=
PYTHON := python3
NPDM_JSON := qlaunch.json
C_FLAGS := -g0 -ffile-prefix-map=$(PWD)=/workspace/exlaunch -fdebug-prefix-map=$(PWD)=/workspace/exlaunch -fmacro-prefix-map=$(PWD)=/workspace/exlaunch
CXX_FLAGS :=
