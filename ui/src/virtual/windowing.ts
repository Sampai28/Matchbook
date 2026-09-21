/**
 * Virtualization windowing maths.
 *
 * A ladder can have hundreds of price levels; a screen shows perhaps thirty.
 * Rendering the rest costs DOM nodes, layout, and paint for pixels nobody sees,
 * and it is the difference between a ladder that keeps up and one that does not.
 *
 * Pure functions, no React, so the arithmetic is testable on its own. Off-by-one
 * errors here show up as a row flickering at the edge of the viewport during a
 * fast scroll, which is miserable to debug through a component.
 */

export interface Window {
  /** First index to render, inclusive. */
  start: number;
  /** Last index to render, exclusive. */
  end: number;
  /** Pixels of blank space to reserve above the rendered rows. */
  offsetTop: number;
  /** Total scrollable height, so the scrollbar is the right size. */
  totalHeight: number;
}

export interface WindowInput {
  scrollTop: number;
  viewportHeight: number;
  rowHeight: number;
  itemCount: number;
  /**
   * Rows rendered beyond each edge of the viewport. Without overscan, a fast
   * scroll paints blank space where rows have not been created yet.
   */
  overscan?: number;
}

export function computeWindow({
  scrollTop,
  viewportHeight,
  rowHeight,
  itemCount,
  overscan = 4,
}: WindowInput): Window {
  if (rowHeight <= 0 || itemCount <= 0 || viewportHeight <= 0) {
    return { start: 0, end: 0, offsetTop: 0, totalHeight: Math.max(0, itemCount * Math.max(0, rowHeight)) };
  }

  const clampedScroll = Math.max(0, Math.min(scrollTop, itemCount * rowHeight - 1));

  const firstVisible = Math.floor(clampedScroll / rowHeight);
  // ceil, not floor: a viewport 2.5 rows tall shows part of a third row, and
  // flooring would leave a sliver of blank space at the bottom edge.
  const visibleCount = Math.ceil(viewportHeight / rowHeight) + 1;

  const start = Math.max(0, firstVisible - overscan);
  const end = Math.min(itemCount, firstVisible + visibleCount + overscan);

  return {
    start,
    end,
    offsetTop: start * rowHeight,
    totalHeight: itemCount * rowHeight,
  };
}

/**
 * Scroll offset that centres a given row — used to keep the spread in view as
 * the book moves, rather than making the user chase it.
 */
export function scrollToCenter(
  index: number,
  rowHeight: number,
  viewportHeight: number,
  itemCount: number,
): number {
  const target = index * rowHeight - viewportHeight / 2 + rowHeight / 2;
  const max = Math.max(0, itemCount * rowHeight - viewportHeight);
  return Math.max(0, Math.min(target, max));
}
