/**
 * Wire types for the market data protocol.
 *
 * This file mirrors docs/protocol.md and must be changed with it.
 * `conformance.test.ts` parses fixtures captured from the real C++ gateway and
 * fails if the two have drifted — a type file that merely *looks* right is how
 * a renamed field ships and the ladder silently stops updating.
 */

export type MessageType = 'snapshot' | 'delta' | 'heartbeat' | 'error';

/** Which side of the book a level sits on. Deltas use BID/ASK. */
export type LevelSide = 'BID' | 'ASK';

/** The aggressor's side on a trade print. Trades use BUY/SELL. */
export type TakerSide = 'BUY' | 'SELL';

export interface WireLevel {
  price: number;
  quantity: number;
  orders: number;
}

/** A level entry inside a delta, which additionally names its side. */
export interface WireDeltaLevel extends WireLevel {
  side: LevelSide;
}

export interface WireTrade {
  price: number;
  quantity: number;
  takerSide: TakerSide;
}

export interface SnapshotMessage {
  type: 'snapshot';
  seq: number;
  symbol: string;
  engine: string;
  bids: WireLevel[];
  asks: WireLevel[];
  restingOrders: number;
}

export interface DeltaMessage {
  type: 'delta';
  seq: number;
  symbol: string;
  levels: WireDeltaLevel[];
  trades: WireTrade[];
}

export interface HeartbeatMessage {
  type: 'heartbeat';
  seq: number;
  symbol: string;
}

export interface ErrorMessage {
  type: 'error';
  seq: number;
  symbol: string;
  code: string;
  message: string;
}

export type WireMessage =
  | SnapshotMessage
  | DeltaMessage
  | HeartbeatMessage
  | ErrorMessage;

/**
 * Engine stats, from GET /stats/engine. All latencies are nanoseconds.
 */
export interface LatencyBucket {
  p50: number;
  p99: number;
  p999: number;
  count: number;
}

export interface EngineStats {
  engine: string;
  submit: LatencyBucket;
  cancel: LatencyBucket;
  counters: {
    ordersAccepted: number;
    ordersRejected: number;
    ordersResting: number;
    fills: number;
  };
  streams: {
    clients: number;
    messagesSent: number;
    dropped: number;
  };
}

// --- narrowing --------------------------------------------------------------
//
// Hand-written guards rather than a schema library. The surface is four message
// shapes that change only when the C++ changes, and the conformance test pins
// them against real captured frames — which is a stronger guarantee than a
// runtime validator derived from the same types that might be wrong.

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

function isWireLevel(value: unknown): value is WireLevel {
  return (
    isRecord(value) &&
    typeof value.price === 'number' &&
    typeof value.quantity === 'number' &&
    typeof value.orders === 'number'
  );
}

function isWireDeltaLevel(value: unknown): value is WireDeltaLevel {
  return isWireLevel(value) && ((value as WireDeltaLevel).side === 'BID' ||
    (value as WireDeltaLevel).side === 'ASK');
}

function isWireTrade(value: unknown): value is WireTrade {
  return (
    isRecord(value) &&
    typeof value.price === 'number' &&
    typeof value.quantity === 'number' &&
    (value.takerSide === 'BUY' || value.takerSide === 'SELL')
  );
}

export function parseMessage(raw: string): WireMessage | null {
  let value: unknown;
  try {
    value = JSON.parse(raw);
  } catch {
    return null;
  }
  if (!isRecord(value) || typeof value.seq !== 'number') return null;

  switch (value.type) {
    case 'snapshot':
      if (
        typeof value.symbol !== 'string' ||
        typeof value.engine !== 'string' ||
        !Array.isArray(value.bids) ||
        !Array.isArray(value.asks) ||
        !value.bids.every(isWireLevel) ||
        !value.asks.every(isWireLevel)
      ) {
        return null;
      }
      return value as unknown as SnapshotMessage;

    case 'delta':
      if (
        typeof value.symbol !== 'string' ||
        !Array.isArray(value.levels) ||
        !Array.isArray(value.trades) ||
        !value.levels.every(isWireDeltaLevel) ||
        !value.trades.every(isWireTrade)
      ) {
        return null;
      }
      return value as unknown as DeltaMessage;

    case 'heartbeat':
      if (typeof value.symbol !== 'string') return null;
      return value as unknown as HeartbeatMessage;

    case 'error':
      if (typeof value.code !== 'string' || typeof value.message !== 'string') {
        return null;
      }
      return value as unknown as ErrorMessage;

    default:
      return null;
  }
}
