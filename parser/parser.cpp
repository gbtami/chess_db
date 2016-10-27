#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "misc.h"
#include "position.h"

namespace {

struct __attribute__ ((packed)) KeyEntry {
    uint64_t key;
    uint32_t moveOffset;
};

typedef uint16_t MoveEntry;

inline bool operator<(const KeyEntry& f, const KeyEntry& s) {
    return f.key < s.key;
}

struct Stats {
    int64_t games;
    int64_t moves;
};

enum State {
    HEADER, BRACKET, COMMENT, NEW_MOVE, WHITE_MOVE, BLACK_MOVE, RESULT
};

enum Token {
    T_NONE, T_LF, T_SPACE, T_DOT, T_RESULT, T_DIGIT, T_MOVE, T_OPEN_BRACKET,
    T_CLOSE_BRACKET, T_OPEN_COMMENT, T_CLOSE_COMMENT
};

Token CharToToken[256];
Position RootPos;

void map(const char* fname, void** baseAddress, uint64_t* mapping) {

#ifndef _WIN32
    struct stat statbuf;
    int fd = ::open(fname, O_RDONLY);
    fstat(fd, &statbuf);
    *mapping = statbuf.st_size;
    *baseAddress = mmap(nullptr, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);

    if (*baseAddress == MAP_FAILED) {
        std::cerr << "Could not mmap() " << fname << std::endl;
        exit(1);
    }
#else
    HANDLE fd = CreateFile(fname.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD size_high;
    DWORD size_low = GetFileSize(fd, &size_high);
    HANDLE mmap = CreateFileMapping(fd, nullptr, PAGE_READONLY, size_high, size_low, nullptr);
    CloseHandle(fd);

    if (!mmap) {
        std::cerr << "CreateFileMapping() failed" << std::endl;
        exit(1);
    }

    *mapping = (uint64_t)mmap;
    *baseAddress = MapViewOfFile(mmap, FILE_MAP_READ, 0, 0, 0);

    if (!*baseAddress) {
        std::cerr << "MapViewOfFile() failed, name = " << fname
                  << ", error = " << GetLastError() << std::endl;
        exit(1);
    }
#endif
}

void unmap(void* baseAddress, uint64_t mapping) {

#ifndef _WIN32
    munmap(baseAddress, mapping);
#else
    UnmapViewOfFile(baseAddress);
    CloseHandle((HANDLE)mapping);
#endif
}

void error(const std::string& desc, const char* data) {

    std::string what = std::string(data, 10);
    std::cerr << desc << ": " << what << "' " << std::endl;
    exit(0);
}

void sort_by_frequency(KeyEntry* kt, size_t size, const MoveEntry* mTable) {

    struct Score {
        KeyEntry kt;
        MoveEntry move;
        int score;
    };

    std::map<MoveEntry, int> moves;
    Score* s = new Score[size];

    for (size_t i = 0; i < size; ++i)
    {
        s[i].kt = kt[i];
        s[i].move = mTable[kt[i].moveOffset +  1]; // Next move
        if (s[i].move != MOVE_NONE)
            moves[s[i].move]++;
    }

    for (size_t i = 0; i < size; ++i)
        s[i].score = moves[s[i].move];

    std::sort(s, s + size, [](const Score& a, const Score& b) -> bool
                           {
                               return    a.score > b.score
                                     || (a.score == b.score && a.move > b.move);
                           });

    for (size_t i = 0; i < size; ++i)
        kt[i] = s[i].kt;

    delete [] s;
}

void parse_game(const char* moves, const char* end, KeyEntry** kt,
                MoveEntry** mt, const MoveEntry* mTable) {

    StateInfo states[1024], *st = states;
    Position pos = RootPos;
    const char *cur = moves, *next = moves;

    while (cur < end)
    {
        while (*next++) {} // Go to next move

        bool givesCheck;
        Move move = pos.san_to_move(cur, &givesCheck, next == end);
        if (!move)
            error("Illegal move", cur);

        pos.do_move(move, *st++, givesCheck);
        **mt = uint16_t(move);
        (*kt)->key = pos.key();
        (*kt)->moveOffset = uint32_t(*mt - mTable);
        ++(*mt);
        ++(*kt);
        cur = next;
    }

    **mt = uint16_t(MOVE_NONE); // Game separator
    ++(*mt);
}

void parse_pgn(void* baseAddress, uint64_t size, Stats& stats,
               bool dryRun, KeyEntry* kTable, MoveEntry* mTable) {

    KeyEntry* kt = kTable;
    MoveEntry* mt = mTable;
    int state = HEADER, prevState = HEADER;
    char moves[1024 * 8] = {};
    char* curMove = moves;
    char* end = curMove;
    size_t moveCnt = 0, gameCnt = 0;
    char* data = (char*)baseAddress;
    char* eof = data + size;

    for (  ; data < eof; ++data)
    {
        Token tk = CharToToken[*(uint8_t*)data];

        switch (state)
        {
        case HEADER:
            if (tk == T_OPEN_BRACKET)
                state = BRACKET;

            else if (tk == T_DIGIT)
                state = NEW_MOVE;

            else if (tk == T_OPEN_COMMENT)
            {
                prevState = state;
                state = COMMENT;
            }
            else if (tk != T_LF && tk != T_SPACE)
                error("Wrong header", data);
            break;

        case BRACKET:
            if (tk == T_CLOSE_BRACKET)
                state = HEADER;
            break;

        case COMMENT:
            if (tk == T_CLOSE_COMMENT)
                state = prevState;
            break;

        case NEW_MOVE:
            if (tk == T_DIGIT)
                continue;

            else if (tk == T_DOT)
                state = WHITE_MOVE;

            else if (tk == T_SPACE && end == curMove)
                continue;

            else if (tk == T_RESULT || *data == '-')
                state = RESULT;

            else if (tk == T_OPEN_COMMENT)
            {
                prevState = state;
                state = COMMENT;
            }
            else
                error("Wrong new move", data);
            break;

        case WHITE_MOVE:
            if (tk == T_MOVE || (tk == T_DIGIT && end != curMove))
                *end++ = *data;

            else if (tk == T_SPACE && end == curMove)
                continue;

            else if (tk == T_SPACE && end != curMove)
            {
                state = BLACK_MOVE;
                *end++ = 0; // Zero-terminating string
                curMove = end;
                moveCnt++;
            }

            else if (tk == T_OPEN_COMMENT)
            {
                prevState = state;
                state = COMMENT;
            }
            else
                error("Wrong white move", data);
            break;

        case BLACK_MOVE:
            if (tk == T_MOVE || (tk == T_DIGIT && end != curMove))
                *end++ = *data;

            else if (tk == T_SPACE && end == curMove)
                continue;

            else if (tk == T_SPACE && end != curMove)
            {
                state = NEW_MOVE;
                *end++ = 0; // Zero-terminating string
                curMove = end;
                moveCnt++;
            }
            else if (tk == T_DIGIT)
                state = RESULT;

            else if (tk == T_OPEN_COMMENT)
            {
                prevState = state;
                state = COMMENT;
            }
            else
                error("Wrong black move", data);
            break;

        case RESULT:
            if (tk == T_LF)
            {
                if (!dryRun)
                    parse_game(moves, end, &kt, &mt, mTable);

                gameCnt++;
                state = HEADER;
                end = curMove = moves;
            }
            break;

        default:
            break;
        }
    }

    stats.games = gameCnt;
    stats.moves = moveCnt;
}

} // namespace

namespace Parser {


void init() {

    static StateInfo st;
    const char* startFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    RootPos.set(startFEN, false, &st);

    CharToToken['\n'] = CharToToken['\r'] = T_LF;
    CharToToken[' '] = CharToToken['\t'] = T_SPACE;
    CharToToken['.'] = T_DOT;
    CharToToken['/'] = CharToToken['*'] = T_RESULT;
    CharToToken['['] = T_OPEN_BRACKET;
    CharToToken[']'] = T_CLOSE_BRACKET;
    CharToToken['{'] = T_OPEN_COMMENT;
    CharToToken['}'] = T_CLOSE_COMMENT;

    CharToToken['0'] = CharToToken['1'] = CharToToken['2'] = CharToToken['3'] =
    CharToToken['4'] = CharToToken['5'] = CharToToken['6'] = CharToToken['7'] =
    CharToToken['8'] = CharToToken['9'] = T_DIGIT;

    CharToToken['a'] = CharToToken['b'] = CharToToken['c'] = CharToToken['d'] =
    CharToToken['e'] = CharToToken['f'] = CharToToken['g'] = CharToToken['h'] =
    CharToToken['N'] = CharToToken['B'] = CharToToken['R'] = CharToToken['Q'] =
    CharToToken['K'] = CharToToken['x'] = CharToToken['+'] = CharToToken['#'] =
    CharToToken['='] = CharToToken['O'] = CharToToken['-'] = T_MOVE;
}

void process_pgn(const char* fname) {

    Stats stats;
    uint64_t size;
    void* baseAddress;

    map(fname, &baseAddress, &size);

    std::cerr << "\nAnalizing...";

    parse_pgn(baseAddress, size, stats, true, nullptr, nullptr);  // Dry run to get file stats

    std::cerr << "done\nProcessing...";

    // Use malloc() because we don't need to init, in the move table add
    // MOVE_NONE each game as game separator. We allocate one move more
    // for the header at the beginning.
    KeyEntry* kTable = (KeyEntry*) malloc((stats.moves + 1) * sizeof(KeyEntry));
    MoveEntry* mTable = (MoveEntry*) malloc((stats.moves + stats.games)  * sizeof(MoveEntry));

    // First entry is reserved for the header
    kTable->key = 0;
    kTable->moveOffset = (stats.moves + 1) * sizeof(KeyEntry);
    kTable++;

    TimePoint elapsed = now();

    parse_pgn(baseAddress, size, stats, false, kTable, mTable);

    elapsed = now() - elapsed + 1; // Ensure positivity to avoid a 'divide by zero'

    std::cerr << "done\nSorting...";

    std::sort(kTable, kTable + stats.moves);

    size_t uniqueKeys = 1;
    KeyEntry* last = kTable;
    for (KeyEntry* it = kTable + 1; it < kTable + stats.moves; ++it)
        if (it->key != (it - 1)->key)
        {
            if (it - last > 2)
               sort_by_frequency(last, it - last, mTable);

            last = it;
            uniqueKeys++;
        }

    std::cerr << "done\nWriting to files...";

    std::string posFile = std::string(fname) + ".idx";

    auto pFile = fopen(posFile.c_str(), "wb");

    // Write header and keys
    fwrite(--kTable, sizeof(KeyEntry), stats.moves + 1, pFile);

    // Write moves
    fwrite(mTable, sizeof(MoveEntry), stats.moves + stats.games, pFile);

    std::cerr << "done\n"
              << "\nGames: " << stats.games
              << "\nMoves: " << stats.moves
              << "\nUnique positions: " << 100 * uniqueKeys / stats.moves << "%"
              << "\nGames/second: " << 1000 * stats.games / elapsed
              << "\nMoves/second: " << 1000 * stats.moves / elapsed
              << "\nMBytes/second: " << float(size) / elapsed / 1000
              << "\nSize of index file (MB): " << ftell(pFile)
              << "\nIndex file: " << posFile
              << "\nProcessing time (ms): " << elapsed << "\n" << std::endl;

    fclose(pFile);
    free(kTable);
    free(mTable);
    unmap(baseAddress, size);
}

}
