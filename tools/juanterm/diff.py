"""Turn "the screen the SE shows" and "the screen it should show" into the
drawing operations of the SSH OS terminal protocol (see kernel/term.c).

A screen here is a list of rows, each a list of (character code, attribute)
pairs, as kept by vt.Screen.
"""

OP_TEXT = 1
OP_ATTR = 2
OP_REPEAT = 3
OP_SCROLL_UP = 4
OP_SCROLL_DOWN = 5
OP_CURSOR = 6
OP_BELL = 7
OP_CLEAR = 8

MAX_MESSAGE = 7900      # the SE accepts 8000
BLANK = (32, 0)
GAP = 4                 # unchanged cells cheaper to redraw than to skip
MIN_REPEAT = 6          # shortest run worth a "repeat" operation


def blank_screen(cols, rows):
    return [[BLANK] * cols for _ in range(rows)]


def copy_screen(screen):
    return [list(row) for row in screen]


def _find_scroll(old, new):
    """Look for a block of rows that moved up or down as a whole, which the
    SE can do by moving memory instead of redrawing text. Returns
    (op, top, bottom, n) or None."""
    rows = len(new)
    old_t = [tuple(r) for r in old]
    new_t = [tuple(r) for r in new]
    blank = tuple([BLANK] * len(new[0]))
    best = None
    best_gain = 2           # must save at least three rows to be worth it
    for top in (0, 1, 2):
        for bottom in (rows - 1, rows - 2, rows - 3):
            if bottom - top < 3:
                continue
            fixed = sum(1 for i in range(top, bottom + 1)
                        if new_t[i] == old_t[i] and new_t[i] != blank)
            for n in range(1, bottom - top + 1):
                up = sum(1 for i in range(top, bottom + 1 - n)
                         if new_t[i] == old_t[i + n] and new_t[i] != blank)
                if up - fixed > best_gain:
                    best_gain, best = up - fixed, (OP_SCROLL_UP, top, bottom, n)
                down = sum(1 for i in range(top + n, bottom + 1)
                           if new_t[i] == old_t[i - n] and new_t[i] != blank)
                if down - fixed > best_gain:
                    best_gain, best = down - fixed, (OP_SCROLL_DOWN, top, bottom, n)
    return best


def apply_scroll(screen, op, top, bottom, n):
    """What a scroll operation does to a screen (the SE does the same)."""
    cols = len(screen[0])
    block = screen[top:bottom + 1]
    n = min(n, len(block))
    if op == OP_SCROLL_UP:
        block = block[n:] + [[BLANK] * cols for _ in range(n)]
    else:
        block = [[BLANK] * cols for _ in range(n)] + block[:len(block) - n]
    screen[top:bottom + 1] = block


def _row_ops(row_no, old_row, new_row):
    """Operations that turn old_row into new_row, as (attr, bytes) pairs."""
    cols = len(new_row)
    ops = []
    col = 0
    while col < cols:
        if new_row[col] == old_row[col]:
            col += 1
            continue
        # A changed stretch: runs on until GAP unchanged cells in a row.
        end = col + 1
        same = 0
        scan = end
        while scan < cols and same < GAP:
            if new_row[scan] == old_row[scan]:
                same += 1
            else:
                same = 0
                end = scan + 1
            scan += 1
        # Emit it in pieces of one attribute, using "repeat" for long runs.
        pos = col
        while pos < end:
            attr = new_row[pos][1]
            stop = pos
            while stop < end and new_row[stop][1] == attr:
                stop += 1
            text_start = pos
            while pos < stop:
                run = pos
                while run < stop and new_row[run][0] == new_row[pos][0]:
                    run += 1
                if run - pos >= MIN_REPEAT:
                    if pos > text_start:
                        chars = bytes(c for c, _ in new_row[text_start:pos])
                        ops.append((attr, bytes([OP_TEXT, row_no, text_start, len(chars)]) + chars))
                    ops.append((attr, bytes([OP_REPEAT, row_no, pos, run - pos, new_row[pos][0]])))
                    text_start = run
                pos = run
            if stop > text_start:
                chars = bytes(c for c, _ in new_row[text_start:stop])
                ops.append((attr, bytes([OP_TEXT, row_no, text_start, len(chars)]) + chars))
            pos = stop
        col = end
    return ops


def encode_update(old, new, cursor, bell=False):
    """Messages (the payloads of 'U' messages) that bring a client showing
    `old` to `new`. old may be None when nothing is known about the client's
    screen. cursor is (row, col, visible). Always returns at least one."""
    rows, cols = len(new), len(new[0])
    plain = []              # (attr or None, bytes): None = does not depend on the attribute
    if old is None:
        plain.append((None, bytes([OP_CLEAR])))
        old = blank_screen(cols, rows)
    else:
        old = copy_screen(old)
        scroll = _find_scroll(old, new)
        if scroll:
            plain.append((None, bytes(scroll)))
            apply_scroll(old, *scroll)
    for r in range(rows):
        if new[r] != old[r]:
            plain.extend(_row_ops(r, old[r], new[r]))
    if bell:
        plain.append((None, bytes([OP_BELL])))
    row, col, visible = cursor
    plain.append((None, bytes([OP_CURSOR, row, col, 1 if visible else 0])))

    # Pack into messages. The attribute starts at 0 in every message.
    messages = []
    current = bytearray()
    attr_now = 0
    for attr, data in plain:
        need = len(data) + (2 if attr is not None and attr != attr_now else 0)
        if len(current) + need > MAX_MESSAGE:
            messages.append(bytes(current))
            current = bytearray()
            attr_now = 0
        if attr is not None and attr != attr_now:
            current += bytes([OP_ATTR, attr])
            attr_now = attr
        current += data
    messages.append(bytes(current))
    return messages


def apply_update(screen, payload):
    """Carry out one message on a screen, exactly as kernel/term.c does.
    Returns (cursor or None, bell). Used by the tests."""
    cols = len(screen[0])
    rows = len(screen)
    attr = 0
    cursor = None
    bell = False
    p = 0
    while p < len(payload):
        op = payload[p]
        p += 1
        if op == OP_TEXT:
            row, col, n = payload[p], payload[p + 1], payload[p + 2]
            for i in range(n):
                if row < rows and col + i < cols:
                    screen[row][col + i] = (payload[p + 3 + i], attr)
            p += 3 + n
        elif op == OP_ATTR:
            attr = payload[p] & 7
            p += 1
        elif op == OP_REPEAT:
            row, col, n, ch = payload[p:p + 4]
            for i in range(n):
                if row < rows and col + i < cols:
                    screen[row][col + i] = (ch, attr)
            p += 4
        elif op in (OP_SCROLL_UP, OP_SCROLL_DOWN):
            top, bottom, n = payload[p:p + 3]
            if top <= bottom < rows and n:
                apply_scroll(screen, op, top, bottom, n)
            p += 3
        elif op == OP_CURSOR:
            cursor = (payload[p], payload[p + 1], bool(payload[p + 2]))
            p += 3
        elif op == OP_BELL:
            bell = True
        elif op == OP_CLEAR:
            for r in range(rows):
                screen[r] = [BLANK] * cols
        else:
            raise ValueError("unknown operation %d" % op)
    return cursor, bell
