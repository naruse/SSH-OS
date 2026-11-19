"""Terminal screen model for the SSH OS remote shell.

A small xterm/VT100 emulator.  It consumes the bytes a program writes to a
PTY and maintains a grid of character cells.  The target display is a 1-bit
screen with a 224-character font, so there are no colours: every cell holds a
SSH OS character code (charset.py: ASCII, Latin-1 and some line drawing) and
an attribute mask (inverse, bold, underline).  Background colours are reduced to "dark" or "light"; a dark
background shows as inverse video.

Only the Python standard library is used.

    screen = Screen(85, 34)
    reply = screen.feed(data_from_pty)      # reply goes back to the PTY
    screen.cells[row][col] -> (ch, attr)
"""

import re
import unicodedata

from . import charset

_CODE = charset.CODE          # character -> code
_CHAR = charset.char          # code -> character

ATTR_INVERSE = 1
ATTR_BOLD = 2
ATTR_UNDERLINE = 4

_BLANK = (32, 0)
_BLANK_INV = (32, ATTR_INVERSE)

# Parser states.
_GROUND = 0
_ESC = 1
_ESC_INTER = 2
_CSI = 3
_OSC = 4
_STR = 5            # DCS, APC, PM, SOS: consumed and ignored

_CSI_MAX = 2048     # longest parameter string accepted, in characters
_PARAM_MAX = 256    # most parameters looked at in one sequence
_VALUE_MAX = 65535  # numeric parameters are clamped to this
_OSC_MAX = 4096     # longest OSC payload kept (the rest is discarded)

_PRINTABLE = re.compile(br'[\x20-\x7e]+')
_PARAMS_OK = re.compile(r'[0-9;:]*\Z')


# ---------------------------------------------------------------------------
# Colours: only "is this background dark?" matters.

def _linear(v):
    v = v / 255.0
    if v <= 0.03928:
        return v / 12.92
    return ((v + 0.055) / 1.055) ** 2.4


def _rgb_is_dark(r, g, b):
    """True if the relative luminance (sRGB, linearised) is below 0.5."""
    lum = 0.2126 * _linear(r) + 0.7152 * _linear(g) + 0.0722 * _linear(b)
    return lum < 0.5


def _build_dark_table():
    palette = [
        (0, 0, 0), (205, 0, 0), (0, 205, 0), (205, 205, 0),
        (0, 0, 238), (205, 0, 205), (0, 205, 205), (229, 229, 229),
        (127, 127, 127), (255, 0, 0), (0, 255, 0), (255, 255, 0),
        (92, 92, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
    ]
    levels = (0, 95, 135, 175, 215, 255)
    for r in levels:
        for g in levels:
            for b in levels:
                palette.append((r, g, b))
    for i in range(24):
        v = 8 + 10 * i
        palette.append((v, v, v))
    return [_rgb_is_dark(r, g, b) for (r, g, b) in palette]


_DARK256 = _build_dark_table()


def _byte(v):
    if v is None or v < 0:
        return 0
    if v > 255:
        return 255
    return v


# ---------------------------------------------------------------------------
# Character mapping: every character becomes one of the SSH OS character
# codes (see charset.py): ASCII, Latin-1, or one of 32 line-drawing and
# symbol glyphs. Anything else gets the nearest of those.

# DEC special graphics, for the characters 0x5f..0x7e.
_DEC_GRAPHICS = {
    0x5f: ' ',   # blank
    0x60: '*',   # diamond
    0x61: '▒',   # checkerboard
    0x62: ' ',   # HT symbol
    0x63: ' ',   # FF symbol
    0x64: ' ',   # CR symbol
    0x65: ' ',   # LF symbol
    0x66: '°',   # degree
    0x67: '±',   # plus/minus
    0x68: ' ',   # NL symbol
    0x69: ' ',   # VT symbol
    0x6a: '┘',   # lower right corner
    0x6b: '┐',   # upper right corner
    0x6c: '┌',   # upper left corner
    0x6d: '└',   # lower left corner
    0x6e: '┼',   # crossing lines
    0x6f: '¯',   # scan line 1
    0x70: '─',   # scan line 3
    0x71: '─',   # horizontal line
    0x72: '─',   # scan line 7
    0x73: '_',   # scan line 9
    0x74: '├',   # left tee
    0x75: '┤',   # right tee
    0x76: '┴',   # bottom tee
    0x77: '┬',   # top tee
    0x78: '│',   # vertical line
    0x79: '<',   # less than or equal
    0x7a: '>',   # greater than or equal
    0x7b: '*',   # pi
    0x7c: '=',   # not equal
    0x7d: '£',   # pound sterling
    0x7e: '·',   # centred dot
}
_DEC_MAP = list(range(128))
for _k, _v in _DEC_GRAPHICS.items():
    _DEC_MAP[_k] = _CODE[_v]
del _k, _v

# Characters with a hand-picked replacement. (U+00A1 to U+00FF need none:
# they are in the character set.)
_EXPLICIT = {
    0x00A0: ' ',
    # letters that have no Unicode decomposition
    0x0110: 'Ð', 0x0111: 'd', 0x0131: 'i', 0x0141: 'L', 0x0142: 'l',
    0x0152: 'O', 0x0153: 'o',
    # general punctuation
    0x2010: '-', 0x2011: '-', 0x2012: '-', 0x2013: '-', 0x2014: '—',
    0x2015: '—', 0x2016: '|', 0x2017: '_', 0x2018: "'", 0x2019: "'",
    0x201A: ',', 0x201B: "'", 0x201C: '"', 0x201D: '"', 0x201E: '"',
    0x201F: '"', 0x2020: '+', 0x2021: '+', 0x2022: '•', 0x2023: '>',
    0x2024: '.', 0x2025: '.', 0x2026: '…', 0x2027: '·', 0x2030: '%',
    0x2032: "'", 0x2033: '"', 0x2035: '`', 0x2039: '<', 0x203A: '>',
    0x203C: '!', 0x2043: '-', 0x2044: '/', 0x20AC: 'E',
    # mathematical operators
    0x2212: '-', 0x2215: '/', 0x2216: '\\', 0x2217: '*', 0x2218: '°',
    0x2219: '·', 0x2223: '|', 0x2236: ':', 0x223C: '~', 0x2248: '~',
    0x2260: '=', 0x2264: '<', 0x2265: '>', 0x22C5: '·', 0x22EE: ':',
    0x22EF: '…',
    # the "record" symbol and geometric shapes used as bullets and markers
    0x23FA: '●',
    0x25A0: '█', 0x25A1: '#', 0x25AA: '•', 0x25AB: '·', 0x25C6: '*',
    0x25C7: '*', 0x25C9: '●', 0x25CB: 'o', 0x25CC: 'o', 0x25CF: '●',
    0x25E6: '°', 0x25EF: 'o', 0x2B24: '●',
    # stars, ballot boxes, check marks and crosses
    0x2605: '*', 0x2606: '*', 0x2610: 'o', 0x2611: '✓', 0x2612: '×',
    0x2705: '✓', 0x2713: '✓', 0x2714: '✓', 0x2715: '×', 0x2716: '×',
    0x2717: '×', 0x2718: '×', 0x274C: '×', 0x274E: '×', 0x2731: '*',
    0x2733: '*', 0x2734: '*',
    # ideographic space, prompt glyphs from patched fonts, replacement char
    0x3000: ' ', 0xE0B0: '>', 0xE0B1: '>', 0xE0B2: '<', 0xE0B3: '<',
    0xFFFD: '?',
}

# Box drawing, by which of the four arms a character has: (up, down, left,
# right). Heavy, double and dashed lines all become the one plain line.
_BOX = {
    (0, 0, 1, 1): '─', (0, 0, 1, 0): '─', (0, 0, 0, 1): '─',
    (1, 1, 0, 0): '│', (1, 0, 0, 0): '│', (0, 1, 0, 0): '│',
    (0, 1, 0, 1): '┌', (0, 1, 1, 0): '┐', (1, 0, 0, 1): '└', (1, 0, 1, 0): '┘',
    (1, 1, 0, 1): '├', (1, 1, 1, 0): '┤', (0, 1, 1, 1): '┬', (1, 0, 1, 1): '┴',
    (1, 1, 1, 1): '┼',
}
_BOX_ARC = {(0, 1, 0, 1): '╭', (0, 1, 1, 0): '╮', (1, 0, 0, 1): '╰', (1, 0, 1, 0): '╯'}

# Block elements U+2580..U+259F: halves, eighths, shades, quadrants.
_BLOCKS = ('▀_▄▄▄▄████' '█▌▌▌▌│'
           '▐░▒▓¯│▒▒▒▒' '▒▒▒▒▒▒')
assert len(_BLOCKS) == 32

_ARROWS = {'<': '←', '>': '→', '^': '↑', 'v': '↓'}


def _box_char(cp):
    """Our glyph for a box-drawing character U+2500..U+257F."""
    words = unicodedata.name(chr(cp)).split()
    if 'DIAGONAL' in words:
        if 'CROSS' in words:
            return '×'
        return '/' if words[-2:] == ['LOWER', 'LEFT'] else '\\'
    vertical, horizontal = 'VERTICAL' in words, 'HORIZONTAL' in words
    arms = (int('UP' in words or vertical), int('DOWN' in words or vertical),
            int('LEFT' in words or horizontal), int('RIGHT' in words or horizontal))
    if 'ARC' in words:
        return _BOX_ARC[arms]
    return _BOX[arms]


def _direction_char(name):
    """An arrow glyph for arrows, '<' '>' '^' 'v' for pointing shapes, from
    the Unicode name."""
    words = name.replace('-', ' ').split()
    arrow = 'ARROW' in words or 'ARROWS' in words
    if not arrow and 'ARROWHEAD' not in words and 'POINTING' not in words:
        return None
    right = 'RIGHT' in words or 'RIGHTWARDS' in words
    left = 'LEFT' in words or 'LEFTWARDS' in words
    up = 'UP' in words or 'UPWARDS' in words
    down = 'DOWN' in words or 'DOWNWARDS' in words
    if right and not left:
        m = '>'
    elif left and not right:
        m = '<'
    elif up and not down:
        m = '^'
    elif down and not up:
        m = 'v'
    elif left and right:
        return '-'
    elif up and down:
        return '|'
    else:
        return None
    return _ARROWS[m] if arrow else m


def _map_cp_slow(cp):
    """Map one code point to a tuple of 0, 1 or 2 character codes."""
    if cp < 0x80:
        if 0x20 <= cp < 0x7F:
            return (cp,)
        return ()
    if cp < 0xA0:
        return ()                       # C1 controls
    if cp > 0x10FFFF or 0xD800 <= cp <= 0xDFFF:
        return (63,)
    ch = chr(cp)
    cat = unicodedata.category(ch)
    wide = unicodedata.east_asian_width(ch) in ('W', 'F')
    if cat == 'Cn' and 0x1F000 <= cp <= 0x1FAFF:
        wide = True                     # pictographs newer than our tables
    m = _EXPLICIT.get(cp)
    if m is None:
        if 0x2500 <= cp <= 0x257F:
            m = _box_char(cp)
        elif 0x2580 <= cp <= 0x259F:
            m = _BLOCKS[cp - 0x2580]
        elif 0x2800 <= cp <= 0x28FF:    # braille (spinners, sparklines)
            m = ' ' if cp == 0x2800 else '·'
        elif cat in ('Mn', 'Me', 'Cf', 'Cc'):
            return ()                   # zero width: dropped
        elif ch in _CODE:
            m = ch                      # Latin-1, or one of our own glyphs
        else:
            try:
                name = unicodedata.name(ch)
            except ValueError:
                name = ''
            m = _direction_char(name)
            if m is None and 'CHECK MARK' in name:
                m = '✓'
            if m is None and 'BULLET' in name:
                m = '•'
            if m is None and not wide:
                base = [x for x in unicodedata.normalize('NFKD', ch)
                        if not unicodedata.category(x).startswith('M')]
                if len(base) == 1 and base[0] in _CODE:
                    m = base[0]
            if m is None:
                m = '?'
    if wide:
        return (_CODE[m], 32)
    return (_CODE[m],)


_MAP_CACHE = {}


def _map_cp(cp):
    r = _MAP_CACHE.get(cp)
    if r is None:
        r = _map_cp_slow(cp)
        if len(_MAP_CACHE) >= 8192:
            _MAP_CACHE.clear()
        _MAP_CACHE[cp] = r
    return r


def _plain_text(codes):
    """A sequence of code points as text limited to our character set (used
    for the title)."""
    out = []
    for cp in codes:
        m = _map_cp(cp)
        if m:
            out.append(_CHAR(m[0]))
    return ''.join(out)


# ---------------------------------------------------------------------------
# Parameter helpers.

def _num(text):
    if not text:
        return None
    if len(text) > 6:
        return _VALUE_MAX
    v = int(text)
    if v > _VALUE_MAX:
        return _VALUE_MAX
    return v


def _parse_params(text):
    """'1;38:5:2;' -> [[1], [38, 5, 2], [None]]"""
    if not text:
        return []
    out = []
    for part in text.split(';', _PARAM_MAX)[:_PARAM_MAX]:
        if ':' in part:
            out.append([_num(x) for x in part.split(':', 8)[:8]])
        else:
            out.append([_num(part)])
    return out


def _first(params, default=0):
    """First parameter, or default when missing."""
    if params:
        v = params[0][0]
        if v is not None:
            return v
    return default


def _count(params, index=0):
    """Parameter used as a count or 1-based position: missing or 0 means 1."""
    if index < len(params):
        v = params[index][0]
        if v:
            return v
    return 1


# ---------------------------------------------------------------------------

class Screen(object):
    """Model of the terminal screen.  See the module docstring."""

    # When true, internal errors propagate out of feed() instead of being
    # absorbed.  Meant for tests only.
    _strict = False

    def __init__(self, cols=85, rows=34):
        try:
            cols = int(cols)
        except (TypeError, ValueError):
            cols = 85
        try:
            rows = int(rows)
        except (TypeError, ValueError):
            rows = 34
        self.cols = max(1, cols)
        self.rows = max(1, rows)
        self.cells = []
        self.title = ''
        self.bell_count = 0
        # incremental UTF-8 decoder
        self._u_need = 0
        self._u_cp = 0
        self._u_lo = 0x80
        self._u_hi = 0xBF
        self._pos = 0
        self._reply = []
        self._reset()

    # -- state ------------------------------------------------------------

    def _reset(self):
        """Full reset (also the initial state).  Title and bell count stay."""
        cols = self.cols
        rows = self.rows
        self.cells[:] = [[_BLANK] * cols for _ in range(rows)]
        self._other = [[_BLANK] * cols for _ in range(rows)]
        self._alt = False
        self.cursor_row = 0
        self.cursor_col = 0
        self._wrap = False              # deferred wrap pending
        self.cursor_visible = True
        self.app_cursor_keys = False
        self.app_keypad = False
        self.bracketed_paste = False
        self._autowrap = True
        self._origin = False
        self._insert = False
        self._top = 0                   # scroll region, inclusive
        self._bottom = rows - 1
        self._bold = False
        self._underline = False
        self._inverse = False
        self._dark = False              # dark background colour selected
        self._attr = 0
        self._blank = _BLANK
        self._g = [False, False]        # G0/G1 are DEC special graphics?
        self._shift = 0
        self._graphics = False
        self._saved = [None, None]      # saved cursor: main, alternate
        self._last = None               # last printed character (for REP)
        self._state = _GROUND
        self._inter = ''
        self._csi = []
        self._osc = []
        self._str_esc = False

    def _update_attr(self):
        a = 0
        if self._inverse != self._dark:
            a = ATTR_INVERSE
        if self._bold:
            a |= ATTR_BOLD
        if self._underline:
            a |= ATTR_UNDERLINE
        self._attr = a
        self._blank = _BLANK_INV if self._dark else _BLANK

    def _recover(self):
        """Put everything back into a consistent state after an error."""
        cols = self.cols
        rows = self.rows
        self._state = _GROUND
        self._u_need = 0
        self._str_esc = False
        self._inter = ''
        self._csi = []
        self._osc = []
        self._last = None
        if not isinstance(self.cells, list):
            self.cells = []
        self.cells[:] = self._fix_grid(self.cells)
        self._other = self._fix_grid(getattr(self, '_other', None))
        r = self.cursor_row if isinstance(self.cursor_row, int) else 0
        c = self.cursor_col if isinstance(self.cursor_col, int) else 0
        self.cursor_row = min(max(r, 0), rows - 1)
        self.cursor_col = min(max(c, 0), cols - 1)
        self._wrap = False
        top = self._top
        bottom = self._bottom
        if not (isinstance(top, int) and isinstance(bottom, int)
                and 0 <= top <= bottom < rows) or (top == bottom and rows > 1):
            self._top = 0
            self._bottom = rows - 1
        self._shift = 1 if self._shift else 0
        self._graphics = bool(self._g[self._shift])
        self._update_attr()

    def _fix_grid(self, grid):
        cols = self.cols
        out = []
        for y in range(self.rows):
            row = None
            if isinstance(grid, list) and y < len(grid):
                row = grid[y]
            good = isinstance(row, list) and len(row) == cols
            if good:
                for cell in row:
                    if not (isinstance(cell, tuple) and len(cell) == 2
                            and isinstance(cell[0], int)
                            and isinstance(cell[1], int)
                            and 32 <= cell[0] <= 126 and 0 <= cell[1] <= 7):
                        good = False
                        break
            out.append(row if good else [_BLANK] * cols)
        return out

    # -- input ------------------------------------------------------------

    def feed(self, data):
        """Consume raw PTY output and return the bytes to write back.

        The data is UTF-8 and may be cut anywhere, including inside an escape
        sequence or a multi-byte character; state carries over to the next
        call.  The return value holds the answers to any queries (usually
        b'').
        """
        if isinstance(data, str):
            data = data.encode('utf-8', 'replace')
        elif not isinstance(data, bytes):
            try:
                data = bytes(data)
            except Exception:
                return b''
        self._reply = []
        n = len(data)
        pos = 0
        while pos < n:
            try:
                pos = self._feed(data, pos, n)
            except Exception:
                if self._strict:
                    raise
                self._recover()
                pos = max(pos, self._pos) + 1
        out = b''.join(self._reply)
        self._reply = []
        return out

    def _feed(self, data, i, n):
        handle = self._handle
        match = _PRINTABLE.match
        while i < n:
            self._pos = i
            b = data[i]
            if self._u_need:
                if self._u_lo <= b <= self._u_hi:
                    self._u_cp = (self._u_cp << 6) | (b & 0x3F)
                    self._u_lo = 0x80
                    self._u_hi = 0xBF
                    self._u_need -= 1
                    i += 1
                    if not self._u_need:
                        handle(self._u_cp)
                    continue
                # truncated character: report it, then look at b afresh
                self._u_need = 0
                handle(0xFFFD)
            if b < 0x80:
                if self._state == _GROUND and 0x20 <= b < 0x7F:
                    m = match(data, i)
                    i = m.end()
                    self._print_run(m.group())
                else:
                    i += 1
                    handle(b)
                continue
            i += 1
            if 0xC2 <= b <= 0xDF:
                self._u_need = 1
                self._u_cp = b & 0x1F
                self._u_lo = 0x80
                self._u_hi = 0xBF
            elif 0xE0 <= b <= 0xEF:
                self._u_need = 2
                self._u_cp = b & 0x0F
                self._u_lo = 0xA0 if b == 0xE0 else 0x80
                self._u_hi = 0x9F if b == 0xED else 0xBF
            elif 0xF0 <= b <= 0xF4:
                self._u_need = 3
                self._u_cp = b & 0x07
                self._u_lo = 0x90 if b == 0xF0 else 0x80
                self._u_hi = 0x8F if b == 0xF4 else 0xBF
            else:
                handle(0xFFFD)
        return n

    def _handle(self, c):
        """Run one code point through the escape sequence parser."""
        st = self._state
        if st == _GROUND:
            if c >= 0x20:
                if c != 0x7F:
                    self._print_cp(c)
            elif c == 0x1B:
                self._state = _ESC
                self._inter = ''
            else:
                self._control(c)
            return

        if st == _OSC or st == _STR:
            if self._str_esc:
                self._str_esc = False
                if c == 0x5C:                   # ESC \ : string terminator
                    self._state = _GROUND
                    if st == _OSC:
                        self._osc_dispatch()
                    return
                # ESC something else: the string is abandoned and the ESC
                # starts a new sequence.
                self._osc = []
                self._state = _ESC
                self._inter = ''
                st = _ESC
            elif c == 0x1B:
                self._str_esc = True
                return
            elif c == 0x18 or c == 0x1A:        # CAN, SUB abort
                self._osc = []
                self._state = _GROUND
                return
            elif c == 0x07 and st == _OSC:
                self._state = _GROUND
                self._osc_dispatch()
                return
            else:
                if st == _OSC and c >= 0x20 and c != 0x7F \
                        and len(self._osc) < _OSC_MAX:
                    self._osc.append(c)
                return

        # ESC, ESC with intermediates, or CSI
        if c < 0x20:
            if c == 0x1B:
                self._state = _ESC
                self._inter = ''
            elif c == 0x18 or c == 0x1A:
                self._state = _GROUND
            else:
                self._control(c)                # controls act mid-sequence
            return
        if c == 0x7F:
            return
        if c >= 0x80:
            if st != _CSI:
                self._state = _GROUND
                self._print_cp(c)
            return
        if st == _ESC:
            if c < 0x30:
                self._inter = chr(c)
                self._state = _ESC_INTER
            elif c == 0x5B:                     # [
                self._state = _CSI
                self._csi = []
            elif c == 0x5D:                     # ]
                self._state = _OSC
                self._osc = []
                self._str_esc = False
            elif c == 0x50 or c == 0x58 or c == 0x5E or c == 0x5F:  # P X ^ _
                self._state = _STR
                self._str_esc = False
            else:
                self._state = _GROUND
                self._esc_dispatch(c)
        elif st == _ESC_INTER:
            if c < 0x30:
                if len(self._inter) < 8:
                    self._inter += chr(c)
            else:
                self._state = _GROUND
                self._esc_inter_dispatch(self._inter, c)
        else:
            if c < 0x40:
                if len(self._csi) <= _CSI_MAX:
                    self._csi.append(c)
            else:
                self._state = _GROUND
                self._csi_dispatch(c)

    # -- printing ---------------------------------------------------------

    def _index(self):
        """Move down one line, scrolling the region at its bottom."""
        row = self.cursor_row
        if row == self._bottom:
            cells = self.cells
            del cells[self._top]
            cells.insert(row, [self._blank] * self.cols)
        elif row < self.rows - 1:
            self.cursor_row = row + 1

    def _reverse_index(self):
        row = self.cursor_row
        if row == self._top:
            cells = self.cells
            del cells[self._bottom]
            cells.insert(row, [self._blank] * self.cols)
        elif row > 0:
            self.cursor_row = row - 1

    def _put(self, ch):
        if self._wrap:
            self._wrap = False
            self.cursor_col = 0
            self._index()
        col = self.cursor_col
        row = self.cells[self.cursor_row]
        if self._insert:
            row.insert(col, (ch, self._attr))
            row.pop()
        else:
            row[col] = (ch, self._attr)
        if col + 1 < self.cols:
            self.cursor_col = col + 1
        elif self._autowrap:
            self._wrap = True

    def _emit(self, chars):
        """Print a mapped character: one cell, or two for a wide one."""
        if len(chars) == 2 and self.cols >= 2:
            if not self._wrap and self.cursor_col == self.cols - 1:
                if not self._autowrap:
                    self._put(chars[0])     # clipped at the right edge
                    return
                self._wrap = True           # does not fit: wrap first
            self._put(chars[0])
            self._put(chars[1])
        else:
            self._put(chars[0])

    def _print_cp(self, c):
        if c < 0x7F:
            if self._graphics:
                c = _DEC_MAP[c]
            chars = (c,)
        else:
            chars = _map_cp(c)
            if not chars:
                return
        self._last = chars
        self._emit(chars)

    def _print_run(self, run):
        """Print a run of printable ASCII bytes."""
        if self._graphics or self._insert:
            put = self._put
            if self._graphics:
                for ch in run:
                    put(_DEC_MAP[ch])
                self._last = (_DEC_MAP[run[-1]],)
            else:
                for ch in run:
                    put(ch)
                self._last = (run[-1],)
            return
        attr = self._attr
        cols = self.cols
        n = len(run)
        pos = 0
        while pos < n:
            if self._wrap:
                self._wrap = False
                self.cursor_col = 0
                self._index()
            col = self.cursor_col
            take = cols - col
            if take > n - pos:
                take = n - pos
            row = self.cells[self.cursor_row]
            row[col:col + take] = [(ch, attr) for ch in run[pos:pos + take]]
            pos += take
            col += take
            if col < cols:
                self.cursor_col = col
                continue
            self.cursor_col = cols - 1
            if self._autowrap:
                self._wrap = True
            else:
                if pos < n:                 # the rest piles up in the last column
                    row[cols - 1] = (run[n - 1], attr)
                break
        self._last = (run[-1],)

    # -- C0 controls ------------------------------------------------------

    def _control(self, c):
        if c == 0x0A or c == 0x0B or c == 0x0C:
            self._wrap = False
            self._index()
        elif c == 0x0D:
            self.cursor_col = 0
            self._wrap = False
        elif c == 0x08:
            if self.cursor_col > 0:
                self.cursor_col -= 1
            self._wrap = False
        elif c == 0x09:
            col = (self.cursor_col | 7) + 1
            self.cursor_col = min(col, self.cols - 1)
        elif c == 0x07:
            self.bell_count += 1
        elif c == 0x0E:
            self._shift = 1
            self._graphics = self._g[1]
        elif c == 0x0F:
            self._shift = 0
            self._graphics = self._g[0]

    # -- cursor -----------------------------------------------------------

    def _goto(self, row, col):
        """Absolute move (0-based), honouring origin mode."""
        if row < 0:
            row = 0
        if self._origin:
            row += self._top
            if row > self._bottom:
                row = self._bottom
        elif row > self.rows - 1:
            row = self.rows - 1
        if col < 0:
            col = 0
        elif col > self.cols - 1:
            col = self.cols - 1
        self.cursor_row = row
        self.cursor_col = col
        self._wrap = False

    def _up(self, n):
        row = self.cursor_row
        limit = self._top if row >= self._top else 0
        self.cursor_row = max(limit, row - n)
        self._wrap = False

    def _down(self, n):
        row = self.cursor_row
        limit = self._bottom if row <= self._bottom else self.rows - 1
        self.cursor_row = min(limit, row + n)
        self._wrap = False

    def _save_cursor(self):
        self._saved[1 if self._alt else 0] = (
            self.cursor_row, self.cursor_col, self._wrap,
            self._bold, self._underline, self._inverse, self._dark,
            self._g[0], self._g[1], self._shift, self._origin)

    def _restore_cursor(self):
        sv = self._saved[1 if self._alt else 0]
        if sv is None:
            sv = (0, 0, False, False, False, False, False,
                  False, False, 0, False)
        (row, col, wrap, self._bold, self._underline, self._inverse,
         self._dark, g0, g1, self._shift, self._origin) = sv
        self.cursor_row = min(max(row, 0), self.rows - 1)
        self.cursor_col = min(max(col, 0), self.cols - 1)
        self._wrap = bool(wrap) and self._autowrap
        self._g = [g0, g1]
        self._graphics = self._g[self._shift]
        self._update_attr()

    # -- erasing and scrolling -------------------------------------------

    def _blank_rows(self, n):
        blank = self._blank
        cols = self.cols
        return [[blank] * cols for _ in range(n)]

    def _scroll_up(self, top, bottom, n):
        size = bottom - top + 1
        if n > size:
            n = size
        cells = self.cells
        cells[top:bottom + 1] = cells[top + n:bottom + 1] + self._blank_rows(n)

    def _scroll_down(self, top, bottom, n):
        size = bottom - top + 1
        if n > size:
            n = size
        cells = self.cells
        cells[top:bottom + 1] = self._blank_rows(n) + cells[top:bottom + 1 - n]

    def _erase_display(self, mode):
        cells = self.cells
        cols = self.cols
        blank = self._blank
        r = self.cursor_row
        c = self.cursor_col
        if mode == 0:
            cells[r][c:] = [blank] * (cols - c)
            for y in range(r + 1, self.rows):
                cells[y] = [blank] * cols
        elif mode == 1:
            for y in range(r):
                cells[y] = [blank] * cols
            cells[r][:c + 1] = [blank] * (c + 1)
        elif mode == 2:
            cells[:] = self._blank_rows(self.rows)
        else:
            return                          # 3 = scrollback: there is none
        self._wrap = False

    def _erase_line(self, mode):
        cols = self.cols
        blank = self._blank
        row = self.cells[self.cursor_row]
        c = self.cursor_col
        if mode == 0:
            row[c:] = [blank] * (cols - c)
        elif mode == 1:
            row[:c + 1] = [blank] * (c + 1)
        elif mode == 2:
            row[:] = [blank] * cols
        else:
            return
        self._wrap = False

    def _switch_buffer(self, alt):
        """Select the main or alternate screen.  True if it changed."""
        if alt == self._alt:
            return False
        current = self.cells[:]
        self.cells[:] = self._other
        self._other = current
        self._alt = alt
        self._wrap = False
        return True

    # -- ESC --------------------------------------------------------------

    def _esc_dispatch(self, c):
        if c == 0x37:                       # 7  DECSC
            self._save_cursor()
        elif c == 0x38:                     # 8  DECRC
            self._restore_cursor()
        elif c == 0x44:                     # D  IND
            self._wrap = False
            self._index()
        elif c == 0x45:                     # E  NEL
            self._wrap = False
            self.cursor_col = 0
            self._index()
        elif c == 0x4D:                     # M  RI
            self._wrap = False
            self._reverse_index()
        elif c == 0x63:                     # c  RIS
            self._reset()
        elif c == 0x3D:                     # =  DECKPAM
            self.app_keypad = True
        elif c == 0x3E:                     # >  DECKPNM
            self.app_keypad = False

    def _esc_inter_dispatch(self, inter, c):
        if inter == '(':
            self._g[0] = (c == 0x30)
            self._graphics = self._g[self._shift]
        elif inter == ')':
            self._g[1] = (c == 0x30)
            self._graphics = self._g[self._shift]
        elif inter == '#' and c == 0x38:    # DECALN: screen alignment test
            cols = self.cols
            self.cells[:] = [[(69, 0)] * cols for _ in range(self.rows)]
            self._top = 0
            self._bottom = self.rows - 1
            self.cursor_row = 0
            self.cursor_col = 0
            self._wrap = False

    # -- OSC --------------------------------------------------------------

    def _osc_dispatch(self):
        codes = self._osc
        self._osc = []
        try:
            sep = codes.index(0x3B)
        except ValueError:
            return
        num = codes[:sep]
        if num == [0x30] or num == [0x32]:
            self.title = _plain_text(codes[sep + 1:])

    # -- CSI --------------------------------------------------------------

    def _csi_dispatch(self, final):
        buf = self._csi
        self._csi = []
        if len(buf) > _CSI_MAX:
            return
        text = ''.join(map(chr, buf))
        prefix = ''
        if text and text[0] in '<=>?':
            prefix = text[0]
            text = text[1:]
        k = len(text)
        while k > 0 and text[k - 1] < '0':
            k -= 1
        if k != len(text):
            return                          # intermediates: nothing we handle
        if not _PARAMS_OK.match(text):
            return                          # malformed
        f = chr(final)
        if prefix:
            if prefix == '?':
                if f == 'h':
                    self._dec_modes(_parse_params(text), True)
                elif f == 'l':
                    self._dec_modes(_parse_params(text), False)
            elif prefix == '>':
                if f == 'c' and _first(_parse_params(text)) == 0:
                    self._reply.append(b'\x1b[>0;0;0c')
            return

        params = _parse_params(text)
        cols = self.cols
        if f == 'm':
            self._sgr(params)
        elif f == 'H' or f == 'f':
            self._goto(_count(params, 0) - 1, _count(params, 1) - 1)
        elif f == 'K':
            self._erase_line(_first(params))
        elif f == 'J':
            self._erase_display(_first(params))
        elif f == 'A':
            self._up(_count(params))
        elif f == 'B' or f == 'e':
            self._down(_count(params))
        elif f == 'C' or f == 'a':
            self.cursor_col = min(cols - 1, self.cursor_col + _count(params))
            self._wrap = False
        elif f == 'D':
            self.cursor_col = max(0, self.cursor_col - _count(params))
            self._wrap = False
        elif f == 'E':
            self._down(_count(params))
            self.cursor_col = 0
        elif f == 'F':
            self._up(_count(params))
            self.cursor_col = 0
        elif f == 'G' or f == '`':
            self.cursor_col = min(cols - 1, _count(params) - 1)
            self._wrap = False
        elif f == 'd':
            self._goto(_count(params) - 1, self.cursor_col)
        elif f == 'L':
            row = self.cursor_row
            if self._top <= row <= self._bottom:
                self._scroll_down(row, self._bottom, _count(params))
                self.cursor_col = 0
            self._wrap = False
        elif f == 'M':
            row = self.cursor_row
            if self._top <= row <= self._bottom:
                self._scroll_up(row, self._bottom, _count(params))
                self.cursor_col = 0
            self._wrap = False
        elif f == '@':
            col = self.cursor_col
            n = min(_count(params), cols - col)
            row = self.cells[self.cursor_row]
            row[col:col] = [self._blank] * n
            del row[cols:]
            self._wrap = False
        elif f == 'P':
            col = self.cursor_col
            n = min(_count(params), cols - col)
            row = self.cells[self.cursor_row]
            del row[col:col + n]
            row.extend([self._blank] * n)
            self._wrap = False
        elif f == 'X':
            col = self.cursor_col
            n = min(_count(params), cols - col)
            row = self.cells[self.cursor_row]
            row[col:col + n] = [self._blank] * n
            self._wrap = False
        elif f == 'S':
            self._scroll_up(self._top, self._bottom, _count(params))
        elif f == 'T':
            if len(params) <= 1:            # more parameters: mouse tracking
                self._scroll_down(self._top, self._bottom, _count(params))
        elif f == 'r':
            top = _count(params, 0)
            bottom = self.rows
            if len(params) > 1 and params[1][0]:
                bottom = min(params[1][0], self.rows)
            if top < bottom:
                self._top = top - 1
                self._bottom = bottom - 1
                self._goto(0, 0)
        elif f == 'h' or f == 'l':
            for sub in params:
                if sub[0] == 4:             # IRM: insert mode
                    self._insert = (f == 'h')
        elif f == 'n':
            what = _first(params)
            if what == 6:
                row = self.cursor_row
                if self._origin:
                    row -= self._top
                self._reply.append(
                    ('\x1b[%d;%dR' % (row + 1, self.cursor_col + 1))
                    .encode('ascii'))
            elif what == 5:
                self._reply.append(b'\x1b[0n')
        elif f == 'c':
            if _first(params) == 0:
                self._reply.append(b'\x1b[?6c')
        elif f == 's':
            if not text:
                self._save_cursor()
        elif f == 'u':
            if not text:
                self._restore_cursor()
        elif f == 'b':
            if self._last is not None:
                chars = self._last
                emit = self._emit
                for _ in range(min(_count(params), cols * self.rows)):
                    emit(chars)
        elif f == 'I':
            col = self.cursor_col
            for _ in range(min(_count(params), cols)):
                col = (col | 7) + 1
            self.cursor_col = min(col, cols - 1)
        elif f == 'Z':
            col = self.cursor_col
            for _ in range(min(_count(params), cols)):
                if col <= 0:
                    break
                col = ((col - 1) // 8) * 8
            self.cursor_col = max(col, 0)
            self._wrap = False
        # anything else (including 't' window operations) is ignored

    def _dec_modes(self, params, on):
        for sub in params:
            mode = sub[0]
            if mode == 1:
                self.app_cursor_keys = on
            elif mode == 6:
                self._origin = on
                self._goto(0, 0)
            elif mode == 7:
                self._autowrap = on
                if not on:
                    self._wrap = False
            elif mode == 25:
                self.cursor_visible = on
            elif mode == 47 or mode == 1047:
                if self._switch_buffer(on) and on:
                    self.cells[:] = self._blank_rows(self.rows)
            elif mode == 1048:
                if on:
                    self._save_cursor()
                else:
                    self._restore_cursor()
            elif mode == 1049:
                if on:
                    if not self._alt:
                        self._save_cursor()
                    self._switch_buffer(True)
                    self.cells[:] = self._blank_rows(self.rows)
                else:
                    self._switch_buffer(False)
                    self._restore_cursor()
            elif mode == 2004:
                self.bracketed_paste = on

    # -- SGR --------------------------------------------------------------

    def _sgr(self, params):
        if not params:
            params = [[0]]
        n = len(params)
        i = 0
        while i < n:
            sub = params[i]
            i += 1
            p = sub[0] or 0
            if p == 0:
                self._bold = False
                self._underline = False
                self._inverse = False
                self._dark = False
            elif p == 1:
                self._bold = True
            elif p == 4:
                self._underline = not (len(sub) > 1 and sub[1] == 0)
            elif p == 7:
                self._inverse = True
            elif p == 21:
                self._underline = True      # double underline
            elif p == 22:
                self._bold = False
            elif p == 24:
                self._underline = False
            elif p == 27:
                self._inverse = False
            elif 40 <= p <= 47:
                self._dark = _DARK256[p - 40]
            elif p == 49:
                self._dark = False
            elif 100 <= p <= 107:
                self._dark = _DARK256[p - 100 + 8]
            elif p == 38 or p == 48 or p == 58:
                # extended colour: work out how many parameters it takes
                dark = None
                if len(sub) > 1:
                    spec = sub[1:]          # 48:5:n  48:2:r:g:b  48:2:cs:r:g:b
                elif i < n and len(params[i]) > 1:
                    spec = params[i]        # 48;2:r:g:b
                    i += 1
                else:
                    spec = None
                    if i < n:
                        kind = params[i][0]
                        if kind == 5:
                            idx = params[i + 1][0] if i + 1 < n else None
                            i += 2
                            if idx is not None and 0 <= idx <= 255:
                                dark = _DARK256[idx]
                        elif kind == 2:
                            rgb = [params[j][0] if j < n else None
                                   for j in range(i + 1, i + 4)]
                            i += 4
                            dark = _rgb_is_dark(_byte(rgb[0]), _byte(rgb[1]),
                                                _byte(rgb[2]))
                if spec is not None:
                    kind = spec[0]
                    if kind == 5:
                        idx = spec[1] if len(spec) > 1 else None
                        if idx is not None and 0 <= idx <= 255:
                            dark = _DARK256[idx]
                    elif kind == 2:
                        rgb = spec[2:5] if len(spec) >= 5 else spec[1:4]
                        rgb = list(rgb) + [None, None, None]
                        dark = _rgb_is_dark(_byte(rgb[0]), _byte(rgb[1]),
                                            _byte(rgb[2]))
                if p == 48 and dark is not None:
                    self._dark = dark
            # everything else (faint, italic, blink, foreground colours, ...)
            # has no effect on a 1-bit display
        self._update_attr()
