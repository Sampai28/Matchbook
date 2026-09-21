import { describe, expect, it } from 'vitest';

import { computeWindow, scrollToCenter } from './windowing';

const base = { rowHeight: 20, viewportHeight: 200, itemCount: 100, overscan: 0 };

describe('computeWindow', () => {
  it('starts at the top when unscrolled', () => {
    const w = computeWindow({ ...base, scrollTop: 0 });
    expect(w.start).toBe(0);
    // ceil(200/20) + 1 = 11 rows: ten fully visible plus the partial row that
    // a non-aligned scroll would reveal at the bottom edge.
    expect(w.end).toBe(11);
    expect(w.offsetTop).toBe(0);
  });

  it('reserves the right spacer height so the scrollbar is correct', () => {
    expect(computeWindow({ ...base, scrollTop: 0 }).totalHeight).toBe(2000);
  });

  it('advances the window as it scrolls', () => {
    const w = computeWindow({ ...base, scrollTop: 400 });
    expect(w.start).toBe(20);
    expect(w.offsetTop).toBe(400);
  });

  it('keeps offsetTop aligned to the first rendered row', () => {
    // The spacer must land exactly on a row boundary, or every row shifts by a
    // fraction and the list appears to jitter while scrolling.
    const w = computeWindow({ ...base, scrollTop: 417 });
    expect(w.offsetTop).toBe(w.start * base.rowHeight);
    expect(w.offsetTop % base.rowHeight).toBe(0);
  });

  it('applies overscan on both edges', () => {
    const w = computeWindow({ ...base, scrollTop: 400, overscan: 3 });
    expect(w.start).toBe(17);
    expect(w.end).toBe(34);
  });

  it('never starts before zero even with overscan at the top', () => {
    const w = computeWindow({ ...base, scrollTop: 0, overscan: 10 });
    expect(w.start).toBe(0);
  });

  it('never runs past the end of the list', () => {
    const w = computeWindow({ ...base, scrollTop: 1900, overscan: 10 });
    expect(w.end).toBe(100);
    expect(w.start).toBeLessThan(w.end);
  });

  it('handles an empty list', () => {
    const w = computeWindow({ ...base, scrollTop: 0, itemCount: 0 });
    expect(w).toEqual({ start: 0, end: 0, offsetTop: 0, totalHeight: 0 });
  });

  it('handles a viewport taller than the content', () => {
    const w = computeWindow({ ...base, scrollTop: 0, itemCount: 3, viewportHeight: 800 });
    expect(w.start).toBe(0);
    expect(w.end).toBe(3);
  });

  it('is defensive about a zero row height rather than dividing by zero', () => {
    const w = computeWindow({ ...base, scrollTop: 0, rowHeight: 0 });
    expect(Number.isFinite(w.start)).toBe(true);
    expect(w.end).toBe(0);
  });

  it('clamps a scroll position beyond the content', () => {
    const w = computeWindow({ ...base, scrollTop: 99_999 });
    expect(w.end).toBeLessThanOrEqual(100);
    expect(w.start).toBeLessThan(100);
  });

  it('renders far fewer rows than exist, which is the whole point', () => {
    const w = computeWindow({
      scrollTop: 2000,
      viewportHeight: 400,
      rowHeight: 22,
      itemCount: 5000,
      overscan: 4,
    });
    expect(w.end - w.start).toBeLessThan(40);
  });
});

describe('scrollToCenter', () => {
  it('centres the requested row', () => {
    expect(scrollToCenter(50, 20, 200, 100)).toBe(50 * 20 - 100 + 10);
  });

  it('does not scroll above the top', () => {
    expect(scrollToCenter(0, 20, 200, 100)).toBe(0);
  });

  it('does not scroll past the bottom', () => {
    expect(scrollToCenter(99, 20, 200, 100)).toBe(100 * 20 - 200);
  });
});
