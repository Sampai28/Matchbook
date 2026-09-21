/**
 * The mutable order book, deliberately outside React.
 *
 * This is the core of the performance story. React state is immutable by
 * convention: every update allocates a new object, and every component reading
 * it re-renders. At a few messages a second that is invisible; at the rates a
 * market data feed produces it is the whole cost of the application.
 *
 * So deltas are applied to plain Maps here, with no allocation beyond what the
 * change itself requires, and React is told about it once per animation frame.
 * The component tree never sees the intermediate states, because nobody could
 * have seen them anyway — the screen only refreshes 60 times a second.
 *
 * Nothing in this file imports React, which is what makes it testable in a
 * plain node environment and what keeps the hot path honest.
 */

import type {
  DeltaMessage,
  SnapshotMessage,
  TakerSide,
  WireTrade,
} from '../protocol/types';

export interface Level {
  price: number;
  quantity: number;
  orders: number;
}

export interface Trade {
  price: number;
  quantity: number;
  takerSide: TakerSide;
  /** Client-assigned, monotonic. Used as a stable React key. */
  id: number;
}

export interface LadderRow {
  price: number;
  bidQty: number;
  bidOrders: number;
  askQty: number;
  askOrders: number;
}

/** Recent fills are capped and recycled; an unbounded tape is a memory leak. */
export const MAX_TAPE = 200;

export class Book {
  /** price -> level. Sorting happens at read time, not on every write. */
  readonly bids = new Map<number, Level>();
  readonly asks = new Map<number, Level>();

  trades: Trade[] = [];

  seq = 0;
  symbol = '';
  engine = '';
  restingOrders = 0;

  /** Incremented on every mutation so a consumer can tell whether to re-read. */
  revision = 0;

  private nextTradeId = 1;

  reset(): void {
    this.bids.clear();
    this.asks.clear();
    this.trades = [];
    this.seq = 0;
    this.restingOrders = 0;
    this.revision++;
  }

  applySnapshot(msg: SnapshotMessage): void {
    this.bids.clear();
    this.asks.clear();
    for (const level of msg.bids) {
      if (level.quantity > 0) {
        this.bids.set(level.price, { ...level });
      }
    }
    for (const level of msg.asks) {
      if (level.quantity > 0) {
        this.asks.set(level.price, { ...level });
      }
    }
    this.seq = msg.seq;
    this.symbol = msg.symbol;
    this.engine = msg.engine;
    this.restingOrders = msg.restingOrders;
    this.revision++;
  }

  applyDelta(msg: DeltaMessage): void {
    for (const level of msg.levels) {
      const side = level.side === 'BID' ? this.bids : this.asks;
      if (level.quantity === 0) {
        // A level at zero is a removal, not a level with no size. Keeping it
        // would leave phantom liquidity on the ladder forever.
        side.delete(level.price);
      } else {
        // Absolute replacement, never an increment — see docs/protocol.md.
        // Idempotent, so a duplicated delta is harmless.
        side.set(level.price, {
          price: level.price,
          quantity: level.quantity,
          orders: level.orders,
        });
      }
    }
    for (const trade of msg.trades) {
      this.pushTrade(trade);
    }
    this.seq = msg.seq;
    this.revision++;
  }

  private pushTrade(trade: WireTrade): void {
    this.trades.unshift({ ...trade, id: this.nextTradeId++ });
    if (this.trades.length > MAX_TAPE) {
      this.trades.length = MAX_TAPE;
    }
  }

  bestBid(): number | null {
    let best: number | null = null;
    for (const price of this.bids.keys()) {
      if (best === null || price > best) best = price;
    }
    return best;
  }

  bestAsk(): number | null {
    let best: number | null = null;
    for (const price of this.asks.keys()) {
      if (best === null || price < best) best = price;
    }
    return best;
  }

  /**
   * Merge both sides into price-descending ladder rows.
   *
   * O(n log n) on the number of occupied levels, run once per committed frame
   * rather than once per message. That ordering is the entire optimisation:
   * the same work done per message would run hundreds of times to produce one
   * painted frame.
   */
  ladder(maxRows: number): LadderRow[] {
    const prices = new Set<number>();
    for (const price of this.bids.keys()) prices.add(price);
    for (const price of this.asks.keys()) prices.add(price);

    const sorted = Array.from(prices).sort((a, b) => b - a);

    const rows: LadderRow[] = [];
    for (const price of sorted) {
      const bid = this.bids.get(price);
      const ask = this.asks.get(price);
      rows.push({
        price,
        bidQty: bid?.quantity ?? 0,
        bidOrders: bid?.orders ?? 0,
        askQty: ask?.quantity ?? 0,
        askOrders: ask?.orders ?? 0,
      });
      if (rows.length >= maxRows) break;
    }
    return rows;
  }

  /** Largest resting quantity on either side, for size-bar scaling. */
  maxQuantity(rows: readonly LadderRow[]): number {
    let max = 0;
    for (const row of rows) {
      if (row.bidQty > max) max = row.bidQty;
      if (row.askQty > max) max = row.askQty;
    }
    return max;
  }
}

/** Outcome of feeding a message to the sequencer. */
export type SequenceAction = 'applied' | 'ignored' | 'gap';

/**
 * Gap detection.
 *
 * Kept separate from Book so it can be tested without constructing one, and so
 * the recovery decision is a pure function of two numbers rather than something
 * buried in a stream callback.
 */
export function classifySeq(lastSeq: number, incoming: number): SequenceAction {
  if (lastSeq === 0) return 'applied';       // nothing established yet
  if (incoming === lastSeq) return 'ignored'; // heartbeat
  if (incoming === lastSeq + 1) return 'applied';
  // Both a forward jump and a backward one mean the local book cannot be
  // trusted: forward because messages were missed, backward because the server
  // restarted and its sequence began again.
  return 'gap';
}
