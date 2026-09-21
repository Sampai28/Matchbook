import { describe, expect, it } from 'vitest';

import {
  DEFAULT_RULES,
  isValid,
  requiresPrice,
  toPayload,
  validateOrder,
  type OrderForm,
} from './validation';

function form(partial: Partial<OrderForm> = {}): OrderForm {
  return {
    symbol: 'MBK',
    clientOrderId: '1',
    side: 'BUY',
    type: 'LIMIT',
    quantity: '10',
    price: '100000',
    participant: '1',
    ...partial,
  };
}

describe('a well-formed order', () => {
  it('passes', () => {
    expect(isValid(validateOrder(form()))).toBe(true);
  });
});

describe('quantity — mirrors InvalidQuantity', () => {
  it('rejects zero', () => {
    expect(validateOrder(form({ quantity: '0' })).quantity).toBeDefined();
  });
  it('rejects negatives', () => {
    expect(validateOrder(form({ quantity: '-5' })).quantity).toBeDefined();
  });
  it('rejects non-integers', () => {
    expect(validateOrder(form({ quantity: '1.5' })).quantity).toBeDefined();
  });
  it('rejects empty', () => {
    expect(validateOrder(form({ quantity: '' })).quantity).toBeDefined();
  });
});

describe('price — mirrors the engine price gates', () => {
  it('rejects a non-positive price (InvalidPrice)', () => {
    expect(validateOrder(form({ price: '0' })).price).toBeDefined();
    expect(validateOrder(form({ price: '-100' })).price).toBeDefined();
  });

  it('rejects a price off the tick grid (PriceNotOnTick)', () => {
    const rules = { ...DEFAULT_RULES, tickSize: 25 };
    expect(validateOrder(form({ price: '100010' }), rules).price).toMatch(/multiple of 25/);
    expect(validateOrder(form({ price: '100025' }), rules).price).toBeUndefined();
  });

  it('rejects a price outside the band (PriceBandViolation)', () => {
    const rules = { tickSize: 1, referencePrice: 100_000, priceBandTicks: 100 };
    expect(validateOrder(form({ price: '100101' }), rules).price).toMatch(/outside the band/);
    expect(validateOrder(form({ price: '99899' }), rules).price).toMatch(/outside the band/);
    expect(validateOrder(form({ price: '100100' }), rules).price).toBeUndefined();
  });

  it('requires a price on a LIMIT order (LimitOrderWithoutPrice)', () => {
    expect(validateOrder(form({ price: '' })).price).toBeDefined();
  });

  it('rejects a price on a MARKET order (MarketOrderWithPrice)', () => {
    expect(validateOrder(form({ type: 'MARKET', price: '100000' })).price).toMatch(
      /must not carry a price/,
    );
  });

  it('accepts a MARKET order with no price', () => {
    expect(isValid(validateOrder(form({ type: 'MARKET', price: '' })))).toBe(true);
  });

  it('requires a price for IOC, FOK and POST_ONLY', () => {
    // All three are limit-priced order types in this engine; only MARKET is
    // priceless, and treating IOC as priceless is a common mistake.
    for (const type of ['IOC', 'FOK', 'POST_ONLY'] as const) {
      expect(requiresPrice(type)).toBe(true);
      expect(validateOrder(form({ type, price: '' })).price).toBeDefined();
    }
  });
});

describe('identifiers', () => {
  it('requires a positive client order id', () => {
    expect(validateOrder(form({ clientOrderId: '0' })).clientOrderId).toBeDefined();
    expect(validateOrder(form({ clientOrderId: 'abc' })).clientOrderId).toBeDefined();
  });

  it('requires a positive participant', () => {
    expect(validateOrder(form({ participant: '0' })).participant).toBeDefined();
  });

  it('requires a symbol', () => {
    expect(validateOrder(form({ symbol: '   ' })).symbol).toBeDefined();
  });
});

describe('payload construction', () => {
  it('sends the price for a LIMIT order', () => {
    expect(toPayload(form())).toEqual({
      symbol: 'MBK',
      clientOrderId: 1,
      side: 'BUY',
      type: 'LIMIT',
      quantity: 10,
      participant: 1,
      price: 100_000,
    });
  });

  it('omits the price key entirely for MARKET', () => {
    const payload = toPayload(form({ type: 'MARKET', price: '' }));
    // Omitted, not null: the engine treats a present price on a MARKET order
    // as a client bug and rejects it.
    expect('price' in payload).toBe(false);
  });

  it('trims the symbol', () => {
    expect(toPayload(form({ symbol: '  MBK  ' })).symbol).toBe('MBK');
  });
});
