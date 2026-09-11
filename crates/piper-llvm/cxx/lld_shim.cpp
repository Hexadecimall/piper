// In-process linking through lld's library API. Nothing here spawns a process.
#include "lld/Common/Driver.h"
#include "llvm/Support/raw_ostream.h"
#include <cstring>
#include <string>
#include <vector>

LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(mingw)

extern "C" {

// flavor: "darwin", "gnu", "link" (COFF), or "mingw". Returns lld's exit
// code; diagnostics are returned through *out_log (caller frees with
// piper_lld_free).
int piper_lld_link(const char *flavor, const char *const *args, size_t nargs, char **out_log) {
    std::vector<const char *> argv;
    argv.reserve(nargs + 1);
    argv.push_back(flavor);
    for (size_t i = 0; i < nargs; i++) argv.push_back(args[i]);
    std::string log;
    llvm::raw_string_ostream out(log);
    const lld::DriverDef drivers[] = {
        {lld::Darwin, &lld::macho::link},
        {lld::Gnu, &lld::elf::link},
        {lld::WinLink, &lld::coff::link},
        {lld::MinGW, &lld::mingw::link},
    };
    lld::Result r = lld::lldMain(argv, out, out, drivers);
    out.flush();
    if (out_log) {
        *out_log = (char *)malloc(log.size() + 1);
        memcpy(*out_log, log.c_str(), log.size() + 1);
    }
    return r.retCode;
}

void piper_lld_free(char *p) { free(p); }

}
