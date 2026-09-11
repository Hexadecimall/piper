# Piper

Piper is a Python 3.14 implementation with an LLVM native compiler and a
terminal-first interactive environment.

```sh
piper
piper eval '40 + 2'
piper compile program.py -o program
```

Piper embeds Python standard-library source and can carry target runtimes for
cross compilation. LLVM and lld are linked as libraries; compiling a Python
program does not invoke an external compiler or linker.

## Install

Release binaries can be installed through the Piper Homebrew tap or with
`install.sh`. Source builds require Rust, Python 3.14.7, LLVM 23, lld 23, and a
C++ compiler.

## Status

Piper is under active development. Language and standard-library compatibility
continue to expand toward Python 3.14.7.

## License

MIT
