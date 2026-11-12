/*
 * Dirty rectangle list (dirty.h).
 *
 * Invariant: the rectangles in the list are non-empty, inside the screen and
 * pairwise disjoint, so a frame never paints a pixel twice. dirty_add()
 * keeps it: only the new rectangle is cut (into the bands outside any
 * rectangle it overlaps), and a piece is merged with a neighbor only when
 * the box around both wastes little and overlaps nothing else.
 */

#include "dirty.h"

static int64_t area(gfx_rect_t r) {
    return (int64_t)r.w * r.h;
}

bool rect_empty(gfx_rect_t r) {
    return r.w <= 0 || r.h <= 0;
}

bool rect_overlaps(gfx_rect_t a, gfx_rect_t b) {
    return !rect_empty(a) && !rect_empty(b) &&
           a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

gfx_rect_t rect_union(gfx_rect_t a, gfx_rect_t b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int32_t x0 = a.x < b.x ? a.x : b.x;
    int32_t y0 = a.y < b.y ? a.y : b.y;
    int32_t x1 = (a.x + a.w > b.x + b.w) ? a.x + a.w : b.x + b.w;
    int32_t y1 = (a.y + a.h > b.y + b.h) ? a.y + a.h : b.y + b.h;
    return gfx_rect(x0, y0, x1 - x0, y1 - y0);
}

gfx_rect_t rect_intersect(gfx_rect_t a, gfx_rect_t b) {
    int32_t x0 = a.x > b.x ? a.x : b.x;
    int32_t y0 = a.y > b.y ? a.y : b.y;
    int32_t x1 = (a.x + a.w < b.x + b.w) ? a.x + a.w : b.x + b.w;
    int32_t y1 = (a.y + a.h < b.y + b.h) ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0)
        return gfx_rect(0, 0, 0, 0);
    return gfx_rect(x0, y0, x1 - x0, y1 - y0);
}

static int64_t overlap_area(gfx_rect_t a, gfx_rect_t b) {
    return area(rect_intersect(a, b));
}

static bool contains(gfx_rect_t outer, gfx_rect_t inner) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h;
}

// Worth painting as one: the box around both paints at most a quarter more
// than the two cover together (side by side, nearly the same, one in the other)
static bool cheap_merge(gfx_rect_t a, gfx_rect_t b) {
    int64_t covered = area(a) + area(b) - overlap_area(a, b);
    int64_t waste = area(rect_union(a, b)) - covered;
    return waste <= covered / 4;
}

static void remove_at(dirty_t* d, size_t i) {
    d->rects[i] = d->rects[--d->count];
}

// Pending rectangles of one dirty_add (pieces of a split wait here)
#define PENDING_MAX (4 * DIRTY_MAX)

typedef struct {
    gfx_rect_t rects[PENDING_MAX];
    size_t     count;
} pending_t;

static void push(pending_t* p, gfx_rect_t r) {
    if (rect_empty(r))
        return;
    if (p->count < PENDING_MAX) {
        p->rects[p->count++] = r;
        return;
    }
    // Out of room: fold it into the last one (bigger, never wrong)
    p->rects[p->count - 1] = rect_union(p->rects[p->count - 1], r);
}

// The parts of r outside e (they overlap): up to four bands
static void push_outside(pending_t* p, gfx_rect_t r, gfx_rect_t e) {
    int32_t top    = e.y - r.y;
    int32_t bottom = (r.y + r.h) - (e.y + e.h);
    if (top > 0)
        push(p, gfx_rect(r.x, r.y, r.w, top));
    if (bottom > 0)
        push(p, gfx_rect(r.x, e.y + e.h, r.w, bottom));

    int32_t y0 = r.y > e.y ? r.y : e.y;
    int32_t y1 = (r.y + r.h < e.y + e.h) ? r.y + r.h : e.y + e.h;
    int32_t left  = e.x - r.x;
    int32_t right = (r.x + r.w) - (e.x + e.w);
    if (left > 0)
        push(p, gfx_rect(r.x, y0, left, y1 - y0));
    if (right > 0)
        push(p, gfx_rect(e.x + e.w, y0, right, y1 - y0));
}

// True if r overlaps any rectangle in the list except index `skip`
static bool overlaps_others(const dirty_t* d, gfx_rect_t r, size_t skip) {
    for (size_t i = 0; i < d->count; i++) {
        if (i != skip && rect_overlaps(r, d->rects[i]))
            return true;
    }
    return false;
}

// Add r, which overlaps nothing in the list. Merge it with a neighbor while
// that is cheap and the merged box stays clear of everything else; each
// merge shortens the list, so this ends.
static void place(dirty_t* d, gfx_rect_t r) {
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t i = 0; i < d->count; i++) {
            gfx_rect_t u = rect_union(r, d->rects[i]);
            if (cheap_merge(r, d->rects[i]) && !overlaps_others(d, u, i)) {
                r = u;
                remove_at(d, i);
                merged = true;
                break;
            }
        }
    }

    if (d->count < DIRTY_MAX) {
        d->rects[d->count++] = r;
        return;
    }

    // Full: fold it into the rectangle it grows least, and let the result
    // swallow whatever it now overlaps. The list only shrinks here.
    size_t  best = 0;
    int64_t best_growth = INT64_MAX;
    for (size_t i = 0; i < d->count; i++) {
        int64_t growth = area(rect_union(r, d->rects[i])) - area(d->rects[i]);
        if (growth < best_growth) {
            best_growth = growth;
            best = i;
        }
    }
    r = rect_union(r, d->rects[best]);
    remove_at(d, best);

    bool grew = true;
    while (grew) {
        grew = false;
        for (size_t i = 0; i < d->count; i++) {
            if (rect_overlaps(r, d->rects[i])) {
                r = rect_union(r, d->rects[i]);
                remove_at(d, i);
                grew = true;
                break;
            }
        }
    }
    d->rects[d->count++] = r;
}

void dirty_init(dirty_t* d, int32_t width, int32_t height) {
    d->width  = width;
    d->height = height;
    d->count  = 0;
}

// Only the new rectangle is ever cut, never the ones already listed: each
// piece is smaller than what it came from and misses the rectangle it was
// cut against, so the cutting ends. Then each piece is placed (see place()).
void dirty_add(dirty_t* d, gfx_rect_t r) {
    r = rect_intersect(r, gfx_rect(0, 0, d->width, d->height));
    if (rect_empty(r))
        return;

    pending_t pending;
    pending.count = 0;
    push(&pending, r);

    while (pending.count > 0) {
        r = pending.rects[--pending.count];

        bool done = false;
        for (size_t i = 0; i < d->count && !done; i++) {
            gfx_rect_t e = d->rects[i];
            if (contains(e, r)) {
                done = true;                // Already covered
            } else if (contains(r, e)) {
                remove_at(d, i);            // r covers it: drop it, look again
                i = (size_t)-1;
            } else if (rect_overlaps(r, e)) {
                push_outside(&pending, r, e);
                done = true;                // Its pieces carry on
            }
        }
        if (!done)
            place(d, r);
    }
}

void dirty_add_all(dirty_t* d) {
    d->count = 0;
    dirty_add(d, gfx_rect(0, 0, d->width, d->height));
}

bool dirty_empty(const dirty_t* d) {
    return d->count == 0;
}

void dirty_clear(dirty_t* d) {
    d->count = 0;
}
