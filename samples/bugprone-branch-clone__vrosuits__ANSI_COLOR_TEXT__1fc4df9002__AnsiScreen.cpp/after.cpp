// Copyright 2026 Antony J Ingram, UNIVERSAL I.T SYSTEMS
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "AnsiScreen.h"
#include "AnsiSgr.h"

#include <map>
#include <utility>
#include <vector>

namespace ansi {
namespace {

constexpr char ESC = '\x1b';

// One screen cell. `written` distinguishes a real (possibly blank) glyph from
// an empty cell the cursor merely skipped over, which lets us trim trailing
// emptiness and render skipped cells as plain spaces.
struct Cell {
    char ch      = ' ';
    Attr attr;
    bool written = false;
};

// A dynamically growing 2D grid + cursor that replays an ANSI stream. All
// coordinates are 0-based.
class Screen {
public:
    explicit Screen(const ScreenConfig& cfg)
        : cfg_(cfg),
          bounded_(cfg.height > 0),
          top_(0),
          bot_(cfg.height > 0 ? cfg.height - 1 : 0) {}

    void setAttr(const Attr& a) { attr_ = a; }

    // --- simple cursor motion ----------------------------------------------
    void carriageReturn() { col_ = 0; }
    void backspace()      { if (col_ > 0) --col_; }

    void moveUp(int n)    { row_ = (static_cast<size_t>(n) > row_) ? 0 : row_ - n; }
    void moveDown(int n)  { row_ += n; clampRow(); }
    void moveLeft(int n)  { col_ = (static_cast<size_t>(n) > col_) ? 0 : col_ - n; }
    void moveRight(int n) { col_ += n; clampCol(); }

    void columnTo(int n1) { col_ = n1 > 0 ? static_cast<size_t>(n1 - 1) : 0; clampCol(); }
    void rowTo(int n1)    { row_ = n1 > 0 ? static_cast<size_t>(n1 - 1) : 0; clampRow(); }
    void moveTo(int r1, int c1) { rowTo(r1); columnTo(c1); }

    void saveCursor()    { savedRow_ = row_; savedCol_ = col_; }
    void restoreCursor() { row_ = savedRow_; col_ = savedCol_; clampRow(); clampCol(); }

    // --- line feeds (honor the scroll region when bounded) -----------------
    void index() {                       // LF: down, scrolling at bottom margin
        if (bounded_ && row_ >= bot_) scrollUp(1);
        else { ++row_; clampRow(); }
    }
    void reverseIndex() {                // RI: up, scrolling at top margin
        if (bounded_ && row_ <= top_) scrollDown(1);
        else if (row_ > 0) --row_;
    }
    void newLine() { carriageReturn(); index(); }

    // --- scrolling within [top_, bot_] -------------------------------------
    void scrollUp(int count) {
        if (count <= 0) return;
        size_t n = static_cast<size_t>(count);
        size_t region = bot_ - top_ + 1;
        if (n > region) n = region;
        ensureRow(bot_);
        for (size_t r = top_; r + n <= bot_; ++r) grid_[r] = std::move(grid_[r + n]);
        for (size_t r = bot_ - n + 1; r <= bot_; ++r) grid_[r].clear();
    }
    void scrollDown(int count) {
        if (count <= 0) return;
        size_t n = static_cast<size_t>(count);
        size_t region = bot_ - top_ + 1;
        if (n > region) n = region;
        ensureRow(bot_);
        for (size_t r = bot_ + 1; r-- > top_ + n;) grid_[r] = std::move(grid_[r - n]);
        for (size_t r = top_; r < top_ + n; ++r) grid_[r].clear();
    }

    void setScrollRegion(int t1, int b1) {
        if (!cfg_.scrollRegionEnabled || !bounded_) return;
        size_t t = t1 > 0 ? static_cast<size_t>(t1 - 1) : 0;
        size_t b = b1 > 0 ? static_cast<size_t>(b1 - 1) : cfg_.height - 1;
        if (b > cfg_.height - 1) b = cfg_.height - 1;
        if (t < b) { top_ = t; bot_ = b; }
        row_ = top_;
        col_ = 0;
    }

    // --- tab stops ----------------------------------------------------------
    void setTabHere()   { tabStops_[col_] = true; }
    void clearTabHere() { tabStops_[col_] = false; }
    void clearAllTabs() { tabStops_.clear(); tabDefaultsCleared_ = true; }

    void horizontalTab(int count) {
        for (int k = 0; k < (count <= 0 ? 1 : count); ++k) col_ = nextTabStop(col_);
        clampCol();
    }
    void backTab(int count) {
        for (int k = 0; k < (count <= 0 ? 1 : count); ++k) col_ = prevTabStop(col_);
    }

    // --- writing & erasing --------------------------------------------------
    void put(char c) {
        clampRow();
        Cell& cell = at(row_, col_);
        cell.ch = c;
        cell.attr = attr_;
        cell.written = true;
        ++col_;
        if (cfg_.maxWidth && col_ >= cfg_.maxWidth) { col_ = 0; index(); }
    }

    void eraseChars(int count) {              // ECH: n cells from cursor, no move
        size_t n = count > 0 ? static_cast<size_t>(count) : 1;
        for (size_t c = col_; c < col_ + n; ++c) at(row_, c) = eraseBlank();
    }

    void eraseLine(int mode) {                // EL (operates on existing cells)
        if (row_ >= grid_.size()) return;
        std::vector<Cell>& ln = grid_[row_];
        size_t end = ln.size();
        size_t a = 0, b = 0;
        if (mode == 2)      { a = 0;                       b = end; }
        else if (mode == 0) { a = col_ < end ? col_ : end; b = end; }
        else if (mode == 1) { a = 0;                       b = (col_ + 1 < end ? col_ + 1 : end); }
        else return;
        for (size_t c = a; c < b; ++c) ln[c] = eraseBlank();
    }

    void eraseDisplay(int mode) {             // ED
        if (mode == 2 || mode == 3) { grid_.clear(); return; }
        if (mode == 0) {                       // cursor -> end of screen
            eraseLine(0);
            for (size_t r = row_ + 1; r < grid_.size(); ++r) grid_[r].clear();
        } else if (mode == 1) {                // start of screen -> cursor
            for (size_t r = 0; r < row_ && r < grid_.size(); ++r) grid_[r].clear();
            eraseLine(1);
        }
    }

    void reset() {
        grid_.clear();
        attr_ = Attr{};
        row_ = col_ = savedRow_ = savedCol_ = 0;
        top_ = 0;
        bot_ = bounded_ ? cfg_.height - 1 : 0;
        tabStops_.clear();
        tabDefaultsCleared_ = false;
    }

    // --- flatten into clean text + styled spans -----------------------------
    ParsedDocument flatten() const {
        ParsedDocument doc;

        size_t lastRow = 0;
        bool anyRow = false;
        for (size_t r = 0; r < grid_.size(); ++r)
            if (rowWidth(r) > 0) { lastRow = r; anyRow = true; }
        if (!anyRow) return doc;

        Attr curAttr;
        size_t spanStart = 0;
        auto pushSpan = [&]() {
            size_t end = doc.text.size();
            if (end > spanStart) doc.spans.push_back(Span{spanStart, end - spanStart, curAttr});
            spanStart = end;
        };
        auto emit = [&](char ch, const Attr& a) {
            if (a != curAttr) { pushSpan(); curAttr = a; }
            doc.text.push_back(ch);
        };

        for (size_t r = 0; r <= lastRow; ++r) {
            size_t w = rowWidth(r);
            for (size_t c = 0; c < w; ++c) {
                const Cell& cell = (c < grid_[r].size()) ? grid_[r][c] : kBlank;
                emit(cell.written ? cell.ch : ' ', cell.attr);
            }
            if (r != lastRow) emit('\n', Attr{});
        }
        pushSpan();
        return doc;
    }

private:
    static const Cell kBlank;

    void clampRow() { if (bounded_ && row_ > cfg_.height - 1) row_ = cfg_.height - 1; }
    void clampCol() { if (cfg_.maxWidth && col_ > cfg_.maxWidth - 1) col_ = cfg_.maxWidth - 1; }

    Cell& at(size_t r, size_t c) {
        if (r >= grid_.size()) grid_.resize(r + 1);
        std::vector<Cell>& line = grid_[r];
        if (c >= line.size()) line.resize(c + 1);
        return line[c];
    }

    void ensureRow(size_t r) { if (r >= grid_.size()) grid_.resize(r + 1); }

    // An erased cell: painted with the current background when faithful erase is
    // on and a background is set, otherwise fully transparent.
    Cell eraseBlank() const {
        if (cfg_.eraseUsesBackground && attr_.back.set) {
            Cell cell;
            cell.ch = ' ';
            cell.attr = Attr{};
            cell.attr.back = attr_.back;
            cell.written = true;
            return cell;
        }
        return Cell{};
    }

    bool isStop(size_t c) const {
        auto it = tabStops_.find(c);
        if (it != tabStops_.end()) return it->second;
        return !tabDefaultsCleared_ && cfg_.tabWidth > 0 &&
               c != 0 && (c % static_cast<size_t>(cfg_.tabWidth)) == 0;
    }

    size_t nextTabStop(size_t c) const {
        size_t cap = cfg_.maxWidth ? cfg_.maxWidth - 1 : c + static_cast<size_t>(cfg_.tabWidth) * 256;
        for (size_t x = c + 1; x <= cap; ++x)
            if (isStop(x)) return x;
        return cap;
    }
    size_t prevTabStop(size_t c) const {
        for (size_t x = c; x-- > 0;)
            if (isStop(x)) return x;
        return 0;
    }

    size_t rowWidth(size_t r) const {
        if (r >= grid_.size()) return 0;
        const std::vector<Cell>& line = grid_[r];
        size_t w = 0;
        for (size_t c = 0; c < line.size(); ++c)
            if (line[c].written) w = c + 1;
        return w;
    }

    ScreenConfig cfg_;
    bool   bounded_;
    std::vector<std::vector<Cell>> grid_;
    Attr   attr_;
    size_t row_ = 0, col_ = 0;
    size_t savedRow_ = 0, savedCol_ = 0;
    size_t top_, bot_;
    std::map<size_t, bool> tabStops_;   // explicit set(true)/cleared(false) stops
    bool   tabDefaultsCleared_ = false;
};

const Cell Screen::kBlank{};

// First CSI parameter with a per-op default.
int firstParam(const std::string& body, int dflt) {
    if (body.empty()) return dflt;
    return sgrParams(body)[0];
}

} // namespace

ParsedDocument renderScreen(const std::string& input, const ScreenConfig& cfg) {
    Screen scr(cfg);
    Attr   attr;

    const size_t n = input.size();
    for (size_t i = 0; i < n;) {
        char c = input[i];

        if (c == ESC && i + 1 < n) {
            char next = input[i + 1];
            if (next == '[') {
                // CSI: ESC [ <body> <final 0x40-0x7E>
                size_t j = i + 2;
                while (j < n) {
                    unsigned char fb = static_cast<unsigned char>(input[j]);
                    if (fb >= 0x40 && fb <= 0x7E) break;
                    ++j;
                }
                if (j < n) {
                    char final = input[j];
                    std::string body = input.substr(i + 2, j - (i + 2));
                    switch (final) {
                        case 'm': applySgr(sgrParams(body), attr); scr.setAttr(attr); break;
                        case 'A': scr.moveUp(firstParam(body, 1));    break;
                        case 'B': scr.moveDown(firstParam(body, 1));  break;
                        case 'C':                                     // CUF
                        case 'a': scr.moveRight(firstParam(body, 1)); break; // a = HPR
                        case 'D': scr.moveLeft(firstParam(body, 1));  break;
                        case 'E': scr.carriageReturn(); scr.moveDown(firstParam(body, 1)); break;
                        case 'F': scr.carriageReturn(); scr.moveUp(firstParam(body, 1));   break;
                        case 'G': scr.columnTo(firstParam(body, 1));  break;
                        case 'd': scr.rowTo(firstParam(body, 1));     break;
                        case 'e': scr.moveDown(firstParam(body, 1));  break; // VPR
                        case 'H':
                        case 'f': {
                            std::vector<int> p = sgrParams(body);
                            int r  = p.size() > 0 && p[0] > 0 ? p[0] : 1;
                            int cc = p.size() > 1 && p[1] > 0 ? p[1] : 1;
                            scr.moveTo(r, cc);
                            break;
                        }
                        case 'I': scr.horizontalTab(firstParam(body, 1)); break; // CHT
                        case 'Z': scr.backTab(firstParam(body, 1));       break; // CBT
                        case 'S': scr.scrollUp(firstParam(body, 1));      break; // SU
                        case 'T': scr.scrollDown(firstParam(body, 1));    break; // SD
                        case 'J': scr.eraseDisplay(firstParam(body, 0));  break;
                        case 'K': scr.eraseLine(firstParam(body, 0));     break;
                        case 'X': scr.eraseChars(firstParam(body, 1));    break; // ECH
                        case 'g':                                              // TBC
                            if (firstParam(body, 0) == 3) scr.clearAllTabs();
                            else scr.clearTabHere();
                            break;
                        case 'r': {                                            // DECSTBM
                            std::vector<int> p = sgrParams(body);
                            int t = p.size() > 0 ? p[0] : 0;
                            int b = p.size() > 1 ? p[1] : 0;
                            scr.setScrollRegion(t, b);
                            break;
                        }
                        case 's': scr.saveCursor();    break;
                        case 'u': scr.restoreCursor(); break;
                        default: break;
                    }
                    i = j + 1;
                    continue;
                }
                // Unterminated CSI: treat ESC as literal below.
            } else {
                // Non-CSI ESC sequences (two bytes).
                bool handled = true;
                switch (next) {
                    case 'D': scr.index();         break; // IND
                    case 'M': scr.reverseIndex();  break; // RI
                    case 'E': scr.newLine();       break; // NEL
                    case 'H': scr.setTabHere();    break; // HTS
                    case '7': scr.saveCursor();    break; // DECSC
                    case '8': scr.restoreCursor(); break; // DECRC
                    case 'c': scr.reset();         break; // RIS
                    default:  handled = false;     break;
                }
                if (handled || next != '[') { i += 2; continue; }
            }
        }

        if (c == '\n')      scr.newLine();
        else if (c == '\r') scr.carriageReturn();
        else if (c == '\t') scr.horizontalTab(1);
        else if (c == '\b') scr.backspace();
        else                scr.put(c);
        ++i;
    }

    return scr.flatten();
}

size_t nextFrameBoundary(const std::string& input, size_t pos, size_t minChunk) {
    const size_t n = input.size();
    if (pos >= n) return n;
    size_t target = pos + (minChunk ? minChunk : 1);

    size_t i = pos;
    while (i < n) {
        if (input[i] == ESC && i + 1 < n && input[i + 1] == '[') {
            // Skip a whole CSI sequence (never a stopping point).
            size_t j = i + 2;
            while (j < n) {
                unsigned char fb = static_cast<unsigned char>(input[j]);
                if (fb >= 0x40 && fb <= 0x7E) { ++j; break; }
                ++j;
            }
            i = j;
        } else if (input[i] == ESC && i + 1 < n) {
            i += 2;                       // two-byte ESC sequence
        } else if (input[i] == ESC) {
            i += 1;                       // dangling ESC at end
        } else {
            ++i;                          // a printable / control byte
            if (i >= target) return i;    // end the frame right after it
        }
    }
    return n;
}

} // namespace ansi
