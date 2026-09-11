#include "internal.h"

#ifdef _WIN32
#include <windows.h>
typedef struct { HANDLE input; DWORD mode; } piper_terminal_state;

int piper_terminal_raw_begin(int fd, void *storage, size_t size) {
    PIPER_UNUSED(fd);
    if (size < sizeof(piper_terminal_state)) return -1;
    piper_terminal_state *state = storage;
    state->input = GetStdHandle(STD_INPUT_HANDLE);
    if (state->input == INVALID_HANDLE_VALUE || !GetConsoleMode(state->input, &state->mode)) return -1;
    DWORD mode = state->mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
    mode |= ENABLE_PROCESSED_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT;
    return SetConsoleMode(state->input, mode) ? 0 : -1;
}

void piper_terminal_raw_end(int fd, void *storage) {
    PIPER_UNUSED(fd);
    piper_terminal_state *state = storage;
    SetConsoleMode(state->input, state->mode);
}
#else
#include <termios.h>
#include <unistd.h>

int piper_terminal_raw_begin(int fd, void *storage, size_t size) {
    if (size < sizeof(struct termios)) return -1;
    struct termios *saved = storage, raw;
    if (tcgetattr(fd, saved) < 0) return -1;
    raw = *saved;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(fd, TCSAFLUSH, &raw);
}

void piper_terminal_raw_end(int fd, void *storage) {
    tcsetattr(fd, TCSAFLUSH, (struct termios *)storage);
}
#endif
