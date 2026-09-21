/**
 * Conformance: the TypeScript types must match what the C++ gateway emits.
 *
 * The failure this prevents is a field rename on one side. The app keeps
 * compiling, the stream keeps connecting, and the ladder silently stops
 * updating — because `parseMessage` rejects every frame and nothing throws.
 *
 * These fixtures are captured output, not invented examples. See fixtures.ts.
 */

import { describe, expect, it } from 'vitest';

import { Book } from '../book/book';
import {
  CAPTURED_DELTA_ADD,
  CAPTURED_DELTA_TRADE,
  CAPTURED_HEARTBEAT,
  CAPTURED_SNAPSHOT,
  CAPTURED_SSE_BODY,
} from './fixtures';
import { parseMessage } from './types';

describe('captured C++ frames parse into the declared types', () => {
  it('parses a snapshot', () => {
    const message = parseMessage(CAPTURED_SNAPSHOT);
    expect(message).not.toBeNull();
    expect(message?.type).toBe('snapshot');
    if (message?.type !== 'snapshot') throw new Error('narrowing failed');

    expect(message.seq).toBe(1);
    expect(message.symbol).toBe('MBK');
    expect(message.engine).toBe('v3');
    expect(message.bids).toEqual([{ price: 100_000, quantity: 50, orders: 1 }]);
    expect(message.asks).toEqual([{ price: 100_100, quantity: 40, orders: 1 }]);
    expect(message.restingOrders).toBe(2);
  });

  it('parses a delta that adds a level', () => {
    const message = parseMessage(CAPTURED_DELTA_ADD);
    if (message?.type !== 'delta') throw new Error('expected a delta');

    expect(message.levels).toEqual([
      { side: 'BID', price: 99_930, quantity: 30, orders: 1 },
    ]);
    expect(message.trades).toEqual([]);
  });

  it('parses a delta carrying a removal and a trade', () => {
    const message = parseMessage(CAPTURED_DELTA_TRADE);
    if (message?.type !== 'delta') throw new Error('expected a delta');

    expect(message.levels[0]?.quantity).toBe(0);
    expect(message.trades[0]).toEqual({
      price: 100_100,
      quantity: 40,
      takerSide: 'BUY',
    });
  });

  it('parses a heartbeat', () => {
    const message = parseMessage(CAPTURED_HEARTBEAT);
    expect(message?.type).toBe('heartbeat');
    expect(message?.seq).toBe(5);
  });
});

describe('the delta side vocabulary is BID/ASK and the trade one is BUY/SELL', () => {
  it('rejects a level using the trade vocabulary', () => {
    // The two really are different in the wire format, and conflating them is
    // the most likely drift between the C++ and the client.
    const wrong = CAPTURED_DELTA_ADD.replace('"side":"BID"', '"side":"BUY"');
    expect(parseMessage(wrong)).toBeNull();
  });

  it('rejects a trade using the level vocabulary', () => {
    const wrong = CAPTURED_DELTA_TRADE.replace('"takerSide":"BUY"', '"takerSide":"BID"');
    expect(parseMessage(wrong)).toBeNull();
  });
});

describe('malformed frames are rejected rather than half-parsed', () => {
  it('rejects a renamed field', () => {
    expect(parseMessage(CAPTURED_SNAPSHOT.replace('"quantity"', '"qty"'))).toBeNull();
  });

  it('rejects a price sent as a string', () => {
    // If the C++ ever emitted prices as strings, silently coercing them would
    // produce a ladder that sorts lexicographically.
    expect(parseMessage(CAPTURED_SNAPSHOT.replace('"price":100000', '"price":"100000"'))).toBeNull();
  });

  it('rejects an unknown message type', () => {
    expect(parseMessage('{"type":"wat","seq":1,"symbol":"MBK"}')).toBeNull();
  });

  it('rejects a missing sequence number', () => {
    expect(parseMessage('{"type":"heartbeat","symbol":"MBK"}')).toBeNull();
  });

  it('rejects invalid JSON without throwing', () => {
    expect(parseMessage('{not json')).toBeNull();
  });
});

describe('a captured session replays into the expected book', () => {
  it('produces the state the engine was actually in', () => {
    const payloads = CAPTURED_SSE_BODY.split('\n')
      .filter((line) => line.startsWith('data: '))
      .map((line) => line.slice('data: '.length));

    const book = new Book();
    for (const payload of payloads) {
      const message = parseMessage(payload);
      expect(message).not.toBeNull();
      if (message?.type === 'snapshot') book.applySnapshot(message);
      else if (message?.type === 'delta') book.applyDelta(message);
    }

    // Snapshot had one bid at 100000 and one ask at 100100. A delta added a
    // bid at 99930; another removed the ask entirely and printed the trade.
    expect(book.bestBid()).toBe(100_000);
    expect(book.bestAsk()).toBeNull();
    expect(book.bids.size).toBe(2);
    expect(book.asks.size).toBe(0);
    expect(book.seq).toBe(5);
    expect(book.trades).toHaveLength(1);
    expect(book.trades[0]).toMatchObject({
      price: 100_100,
      quantity: 40,
      takerSide: 'BUY',
    });
  });
});
