import { memo, useCallback, useLayoutEffect, useRef, useState } from 'react';

import type { LadderRow } from '../book/book';
import { computeWindow } from '../virtual/windowing';

const ROW_HEIGHT = 22;

interface RowProps {
  row: LadderRow;
  maxQuantity: number;
  isBestBid: boolean;
  isBestAsk: boolean;
}

/**
 * memo with an explicit comparator.
 *
 * The default shallow compare would still re-render every row on every commit,
 * because `row` is a fresh object each time the ladder is rebuilt. Comparing
 * the four fields that are actually rendered means a row only re-renders when
 * its own numbers changed — which, in a book where one level ticks, is one row
 * out of thirty rather than thirty out of thirty.
 */
const Row = memo(
  function Row({ row, maxQuantity, isBestBid, isBestAsk }: RowProps) {
    const bidWidth = maxQuantity > 0 ? (row.bidQty / maxQuantity) * 100 : 0;
    const askWidth = maxQuantity > 0 ? (row.askQty / maxQuantity) * 100 : 0;

    return (
      <div
        className="row"
        style={{ height: ROW_HEIGHT }}
        data-best-bid={isBestBid || undefined}
        data-best-ask={isBestAsk || undefined}
      >
        <div className="cell bid">
          <div className="bar bid-bar" style={{ width: `${bidWidth}%` }} />
          <span className="qty">{row.bidQty > 0 ? row.bidQty : ''}</span>
        </div>
        <div className="cell price">{row.price}</div>
        <div className="cell ask">
          <div className="bar ask-bar" style={{ width: `${askWidth}%` }} />
          <span className="qty">{row.askQty > 0 ? row.askQty : ''}</span>
        </div>
      </div>
    );
  },
  (prev, next) =>
    prev.row.price === next.row.price &&
    prev.row.bidQty === next.row.bidQty &&
    prev.row.askQty === next.row.askQty &&
    prev.maxQuantity === next.maxQuantity &&
    prev.isBestBid === next.isBestBid &&
    prev.isBestAsk === next.isBestAsk,
);

interface LadderProps {
  rows: LadderRow[];
  maxQuantity: number;
  bestBid: number | null;
  bestAsk: number | null;
  /** When false, every row is rendered — the naive path, for comparison. */
  virtualized: boolean;
}

export function Ladder({ rows, maxQuantity, bestBid, bestAsk, virtualized }: LadderProps) {
  const viewportRef = useRef<HTMLDivElement>(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [viewportHeight, setViewportHeight] = useState(480);

  useLayoutEffect(() => {
    const element = viewportRef.current;
    if (!element) return;
    const observer = new ResizeObserver(() => {
      setViewportHeight(element.clientHeight);
    });
    observer.observe(element);
    setViewportHeight(element.clientHeight);
    return () => observer.disconnect();
  }, []);

  const onScroll = useCallback((event: React.UIEvent<HTMLDivElement>) => {
    setScrollTop(event.currentTarget.scrollTop);
  }, []);

  const window_ = virtualized
    ? computeWindow({
        scrollTop,
        viewportHeight,
        rowHeight: ROW_HEIGHT,
        itemCount: rows.length,
      })
    : { start: 0, end: rows.length, offsetTop: 0, totalHeight: rows.length * ROW_HEIGHT };

  const visible = rows.slice(window_.start, window_.end);

  return (
    <div className="ladder">
      <div className="ladder-head">
        <span>bid</span>
        <span>price</span>
        <span>ask</span>
      </div>
      <div className="ladder-viewport" ref={viewportRef} onScroll={onScroll}>
        <div style={{ height: window_.totalHeight, position: 'relative' }}>
          <div style={{ transform: `translateY(${window_.offsetTop}px)` }}>
            {visible.map((row) => (
              // Keyed by price, not index. An index key would make React reuse
              // a DOM node for a different price when levels are inserted or
              // removed, so the wrong row would animate and memo would compare
              // against the wrong previous value.
              <Row
                key={row.price}
                row={row}
                maxQuantity={maxQuantity}
                isBestBid={row.price === bestBid}
                isBestAsk={row.price === bestAsk}
              />
            ))}
          </div>
        </div>
      </div>
      <div className="ladder-foot">
        {rows.length} levels · rendering {visible.length}
        {virtualized ? '' : ' (not virtualized)'}
      </div>
    </div>
  );
}
