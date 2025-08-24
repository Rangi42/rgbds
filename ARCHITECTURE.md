# RGBDS Architecture

## Folder Organization

The RGBDS source code file structure is as follows:

```
rgbds/
├── .github/
│   ├── scripts/
│   │   └── ...
│   └── workflows/
│       └── ...
├── contrib/
│   ├── bash_compl/
│   ├── zsh_compl/
│   │   └── ...
│   └── ...
├── include/
│   └── ...
├── man/
│   └── ...
├── src/
│   ├── asm/
│   │   └── ...
│   ├── extern/
│   │   └── ...
│   ├── fix/
│   │   └── ...
│   ├── gfx/
│   │   └── ...
│   ├── link/
│   │   └── ...
│   ├── CMakeLists.txt
│   ├── bison.sh
│   └── ...
├── test/
│   ├── ...
│   └── run-tests.sh
├── .clang-format
├── CMakeLists.txt
├── compile_flags.txt
├── Dockerfile
└── Makefile
```

- `.github/` - files and scripts related to the integration of the RGBDS codebase with GitHub.
  * `scripts/` - scripts used by workflow files.
  * `workflows/` - CI workflow description files.
- `contrib/` - scripts and other resources which may be useful to users and developers of RGBDS.
  * `bash_compl/` - contains tab completion scripts for use with `bash`. Run them with `source`
    somewhere in your `.bashrc`, and they should auto-load when you open a shell.
  * `zsh_compl/` - contains tab completion scripts for use with `zsh`. Put them somewhere in your
    `fpath`, and they should auto-load when you open a shell.
- `include/` - header files for the respective source files in `src`.
- `man/` - manual pages.
- `src/` - source code of RGBDS.
  * `asm/` - source code of RGBASM.
  * `extern/` - source code copied from external sources.
  * `fix/` - source code of RGBFIX.
  * `gfx/` - source code of RGBGFX.
  * `link/` - source code of RGBLINK.
  * `CMakeLists.txt` - defines how to build individual RGBDS programs with CMake, including the
    source files that each program depends on.
  * `bison.sh` - script used to run the Bison parser generator with the latest flags that the
    user's version supports.
- `test/` - testing framework used to verify that changes to the code don't break or
  modify the behavior of RGBDS.
  * `fetch-test-deps.sh` - script used to fetch dependencies for building external repositories.
    `fetch-test-deps.sh --help` describes its options.
  * `run-tests.sh` - script used to run tests, including internal test cases and external
    repositories. `run-tests.sh --help` describes its options.
- `.clang-format` - code style for automated C++ formatting with
  [`clang-format`](https://clang.llvm.org/docs/ClangFormat.html) (for which we define the shortcut
  `make format`).
- `.clang-tidy` - configuration for C++ static analysis with
  [`clang-tidy`](https://clang.llvm.org/extra/clang-tidy/) (for which we define the shortcut
  `make tidy`).
- `CMakeLists.txt` - defines how to build RGBDS with CMake.
- `compile_flags.txt` - compiler flags for `clang-tidy`.
- `Dockerfile` - defines how to build RGBDS with Docker.
- `Makefile` - defines how to build RGBDS with `make`, including the source files that each program
  depends on.

## RGBDS

These files are in the `src/` directory. They apply to more than one program, usually all four of them.

- `backtrace.cpp` - generic printing of location backtraces for RGBASM and RGBLINK. Allows configuring backtrace styles with a command-line flag (conventionally `-B/--backtrace`). Renders warnings in yellow, errors in red, and locations in cyan.
- `diagnostics.cpp` - generic warning/error diagnostic support for all programs. Allows command-line flags (conventionally `-W`) to have `no-`, `error=`, or `no-error=` prefixes, `=` level suffixes, and allows "meta" flags to affect groups of individual flags. Every program has its own `warning.cpp` file that uses this.
- `linkdefs.cpp` - constants, data, and functions related to RGBDS object files, which are used for RGBASM output and RGBLINK input. Defines two global variables, `sectionTypeInfo` and `sectionModNames`.
- `opmath.cpp` - functions for mathematical operations in RGBASM and RGBLINK that aren't trivially equivalent to built-in C++ ones.
- `style.cpp` - generic printing of cross-platform colored or bold text. Obeys the `FORCE_COLOR` or `NO_COLOR` envionment variables, and allows configuring with a command-line flag (conventionally `--color`).
- `usage.cpp` - generic printing of usage information. Renders headings in green, flags in cyan, and URLs in blue. Every program has its own `main.cpp` file that uses this.
- `util.cpp` - utility functions applicable to most programs, mostly dealing with text strings.
- `verbosity.cpp` - generic printing of messages conditionally at different verbosity levels. Allows configuring with a command-line flag (conventionally `-v/--verbose`).
- `version.cpp` - RGBDS version number and string for all the programs.

## External

These source files are in the `src/extern/` directory. They have been copied from external authors and adapted for use with RGBDS.

- `getopt.cpp` - 
- `utf8decoder.cpp` - 

## RGBASM

- `actions.cpp` - 
- `charmap.cpp` - 
- `fixpoint.cpp` - 
- `format.cpp` - 
- `fstack.cpp` - 
- `lexer.cpp` - 
- `macro.cpp` - 
- `main.cpp` - 
- `opt.cpp` - 
- `output.cpp` - 
- `parser.y` - 
- `rpn.cpp` - 
- `section.cpp` - 
- `symbol.cpp` - 
- `warning.cpp` - 

## RGBLINK

- `assign.cpp` - 
- `fstack.cpp` - 
- `layout.cpp` - 
- `lexer.cpp` - 
- `main.cpp` - 
- `object.cpp` - 
- `output.cpp` - 
- `patch.cpp` - 
- `script.y` - 
- `sdas_obj.cpp` - 
- `section.cpp` - 
- `symbol.cpp` - 
- `warning.cpp` - 

## RGBFIX

- `main.cpp` - 
- `mbc.cpp` - 
- `warning.cpp` - 

## RGBGFX

- `color_set.cpp` - 
- `main.cpp` - 
- `pal_packing.cpp` - 
- `pal_sorting.cpp` - 
- `pal_spec.cpp` - 
- `png.cpp` - 
- `process.cpp` - 
- `reverse.cpp` - 
- `rgba.cpp` - 
- `warning.cpp` - 
