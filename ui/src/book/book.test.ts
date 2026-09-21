import { describe, expect, it } from 'vitest';

import { Book, classifySeq } from './book';
import type { DeltaMessage, SnapshotMessage } from '../protocol/types';

function snapshot(partial: Partial<SnapshotMessage> = {}): SnapshotMessage {
  return {
    type: 'snapshot',
    seq: 1,
    symbol: 'MBK',
    engine: 'v3',
    bids: [
      { price: 100_000, quantity: 50, orders: 2 },
      { price: 99_990, quantity: 30, orders: 1 },
    ],
    asks: [
      { price: 100_100, quantity: 40, orders: 1 },
      { price: 100_110, quantity: 60, orders: 3 },
    ],
    restingOrders: 7,
    ...partial,
  };
}

function delta(seq: number, partial: Partial<DeltaMessage> = {}): DeltaMessage {
  return {
    type: 'delta',
    seq,
    symbol: 'MBK',
    levels: [],
    trades: [],
    ...partial,
  };
}

describe('applying a snapshot', () => {
  it('populates both sides', () => {
    const book = new Book();
    book.applySnapshot(snapshot());

    expect(book.bids.size).toBe(2);
    expect(book.asks.size).toBe(2);
    expect(book.bestBid()).toBe(100_000);
    expect(book.bestAsk()).toBe(100_100);
    expect(book.seq).toBe(1);
  });

  it('replaces rather than merges, so a resnapshot cannot leave stale levels', () => {
    const book = new Book();
    book.applySnapshot(snapshot());
    book.applySnapshot(
      snapshot({ seq: 9, bids: [{ price: 99_000, quantity: 5, orders: 1 }], asks: [] }),
    );

    expect(book.bids.size).toBe(1);
    expect(book.asks.size).toBe(0);
    expect(book.bestBid()).toBe(99_000);
  });

  it('drops zero-quantity levels arriving in a snapshot', () => {
    const book = new Book();
    book.applySnapshot(
      snapshot({ bids: [{ price: 100_000, quantity: 0, orders: 0 }], asks: [] }),
    );
    expect(book.bids.size).toBe(0);
  });
});

describe('applying deltas', () => {
  it('replaces a level absolutely rather than incrementing it', () => {
    const book = new Book();
    book.applySnapshot(snapshot());
    book.applyDelta(
      delta(2, { levels: [{ side: 'BID', price: 100_000, quantity: 25, orders: 1 }] }),
    );

    expect(book.bids.get(100_000)).toEqual({ price: 100_000, quantity: 25, orders: 1 });
  });

  it('is idempotent — replaying the same delta changes nothing', () => {
    // The property that makes absolute values the right choice: a duplicated
    // message cannot corrupt the book.
    const book = new Book();
    book.applySnapshot(snapshot());

    const message = delta(2, {
      levels: [{ side: 'ASK', price: 100_100, quantity: 15, orders: 1 }],
    });
    book.applyDelta(message);
    const afterFirst = { ...book.asks.get(100_100)! };
    book.applyDelta(message);

    expect(book.asks.get(100_100)).toEqual(afterFirst);
  });

  it('removes a level on quantity zero', () => {
    const book = new Book();
    book.applySnapshot(snapshot());
    book.applyDelta(
      delta(2, { levels: [{ side: 'ASK', price: 100_100, quantity: 0, orders: 0 }] }),
    );

    expect(book.asks.has(100_100)).toBe(false);
    expect(book.bestAsk()).toBe(100_110);
  });

  it('adds a level that was not in the snapshot', () => {
    const book = new Book();
    book.applySnapshot(snapshot());
    book.applyDelta(
      delta(2, { levels: [{ side: 'BID', price: 100_050, quantity: 12, orders: 1 }] }),
    );

    expect(book.bestBid()).toBe(100_050);
  });

  it('applies a full sequence and matches the expected final book', () => {
    const book = new Book();
    book.applySnapshot(snapshot());

    book.applyDelta(delta(2, { levels: [{ side: 'BID', price: 100_000, quantity: 20, orders: 1 }] }));
    book.applyDelta(delta(3, { levels: [{ side: 'BID', price: 99_990, quantity: 0, orders: 0 }] }));
    book.applyDelta(delta(4, { levels: [{ side: 'ASK', price: 100_090, quantity: 8, orders: 1 }] }));
    book.applyDelta(
      delta(5, {
        levels: [{ side: 'ASK', price: 100_100, quantity: 0, orders: 0 }],
        trades: [{ price: 100_100, quantity: 40, takerSide: 'BUY' }],
      }),
    );

    expect([...book.bids.entries()].sort((a, b) => b[0] - a[0])).toEqual([
      [100_000, { price: 100_000, quantity: 20, orders: 1 }],
    ]);
    expect([...book.asks.entries()].sort((a, b) => a[0] - b[0])).toEqual([
      [100_090, { price: 100_090, quantity: 8, orders: 1 }],
      [100_110, { price: 100_110, quantity: 60, orders: 3 }],
    ]);
    expect(book.seq).toBe(5);
    expect(book.trades).toHaveLength(1);
    expect(book.trades[0]?.takerSide).toBe('BUY');
  });
});

describe('the tape', () => {
  it('is newest-first', () => {
    const book = new Book();
    book.applySnapshot(snapshot());
    book.applyDelta(delta(2, { trades: [{ price: 1, quantity: 1, takerSide: 'BUY' }] }));
    book.applyDelta(delta(3, { trades: [{ price: 2, quantity: 1, takerSide: 'SELL' }] }));

    expect(book.trades[0]?.price).toBe(2);
    expect(book.trades[1]?.price).toBe(1);
  });

  it('is capped, so a long session cannot grow without bound', () => {
    const book = new Book();
    book.applySnapshot(snapshot());
    for (let i = 0; i < 500; i++) {
      book.applyDelta(delta(2 + i, { trades: [{ price: i, quantity: 1, takerSide: 'BUY' }] }));
    }
    expect(book.trades.length).toBe(200);
    expect(book.trades[0]?.price).toBe(499);
  });

  it('gives every trade a unique key', () => {
    const book = new Book();
    book.applySnapshot(snapshot());
    for (let i = 0; i < 20; i++) {
      book.applyDelta(delta(2 + i, { trades: [{ price: 100, quantity: 1, takerSide: 'BUY' }] }));
    }
    // Identical prices and sizes, so anything deriving a key from content
    // would collide and React would reuse the wrong DOM node.
    expect(new Set(book.trades.map((t) => t.id)).size).toBe(20);
  });
});

describe('ladder projection', () => {
  it('merges both sides into price-descending rows', () => {
    const book = new Book();
    book.applySnapshot(snapshot());
    const rows = book.ladder(50);

    expect(rows.map((r) => r.price)).toEqual([100_110, 100_100, 100_000, 99_990]);
    expect(rows[2]).toMatchObject({ price: 100_000, bidQty: 50, askQty: 0 });
    expect(rows[1]).toMatchObject({ price: 100_100, bidQty: 0, askQty: 40 });
  });

  it('caps the row count', () => {
    const book = new Book();
    book.applySnapshot(snapshot());
    for (let i = 0; i < 100; i++) {
      book.applyDelta(delta(2 + i, { levels: [{ side: 'BID', price: 90_000 + i, quantity: 1, orders: 1 }] }));
    }
    expect(book.ladder(10)).toHaveLength(10);
  });

  it('reports the largest quantity for bar scaling', () => {
    const book = new Book();
    book.applySnapshot(snapshot());
    expect(book.maxQuantity(book.ladder(50))).toBe(60);
  });
});

describe('sequence gap detection', () => {
  it('accepts the next number in order', () => {
    expect(classifySeq(10, 11)).toBe('applied');
  });

  it('ignores a repeat, which is what a heartbeat sends', () => {
    expect(classifySeq(10, 10)).toBe('ignored');
  });

  it('reports a forward jump as a gap', () => {
    expect(classifySeq(10, 14)).toBe('gap');
  });

  it('reports a backward jump as a gap, because the server restarted', () => {
    expect(classifySeq(10, 3)).toBe('gap');
  });

  it('accepts anything when nothing has been established yet', () => {
    expect(classifySeq(0, 9_999)).toBe('applied');
  });
});
