/**
 * Client-side order validation, mirroring the engine's inbound gates.
 *
 * The engine validates everything again and its answer is the one that counts —
 * a client-side check is a convenience, never a security boundary. The point is
 * latency of feedback: telling someone their price is off-tick as they type
 * beats a round trip that returns PRICE_NOT_ON_TICK.
 *
 * Every rule here corresponds to a RejectReason in include/matchbook/types.hpp.
 * When they disagree, the engine is right and this file is the bug.
 */

export type OrdType = 'LIMIT' | 'MARKET' | 'IOC' | 'FOK' | 'POST_ONLY';
export type Side = 'BUY' | 'SELL';

export interface OrderForm {
  symbol: string;
  clientOrderId: string;
  side: Side;
  type: OrdType;
  quantity: string;
  price: string;
  participant: string;
}

export interface BookRules {
  tickSize: number;
  referencePrice: number;
  priceBandTicks: number;
}

export const DEFAULT_RULES: BookRules = {
  tickSize: 1,
  referencePrice: 100_000,
  priceBandTicks: 10_000,
};

/** Field name -> message. Empty means the form is submittable. */
export type FieldErrors = Partial<Record<keyof OrderForm, string>>;

/** MARKET carries no price; everything else requires one. */
export function requiresPrice(type: OrdType): boolean {
  return type !== 'MARKET';
}

function parsePositiveInt(raw: string): number | null {
  if (!/^\d+$/.test(raw.trim())) return null;
  const value = Number(raw.trim());
  return Number.isSafeInteger(value) && value > 0 ? value : null;
}

function parseInteger(raw: string): number | null {
  if (!/^-?\d+$/.test(raw.trim())) return null;
  const value = Number(raw.trim());
  return Number.isSafeInteger(value) ? value : null;
}

export function validateOrder(form: OrderForm, rules: BookRules = DEFAULT_RULES): FieldErrors {
  const errors: FieldErrors = {};

  if (!form.symbol.trim()) {
    errors.symbol = 'symbol is required';
  }

  if (parsePositiveInt(form.clientOrderId) === null) {
    errors.clientOrderId = 'must be a positive integer';
  }

  const quantity = parsePositiveInt(form.quantity);
  if (quantity === null) {
    // Mirrors InvalidQuantity. Zero and negative are the same rejection in the
    // engine, so they get the same message here.
    errors.quantity = 'must be a positive integer';
  }

  if (parsePositiveInt(form.participant) === null) {
    errors.participant = 'must be a positive integer';
  }

  if (requiresPrice(form.type)) {
    const price = parseInteger(form.price);
    if (price === null) {
      errors.price = 'must be an integer';         // LimitOrderWithoutPrice
    } else if (price <= 0) {
      errors.price = 'must be greater than zero';  // InvalidPrice
    } else if (rules.tickSize > 0 && price % rules.tickSize !== 0) {
      errors.price = `must be a multiple of ${rules.tickSize}`;  // PriceNotOnTick
    } else {
      const span = rules.priceBandTicks * rules.tickSize;
      const low = rules.referencePrice - span;
      const high = rules.referencePrice + span;
      if (price < low || price > high) {
        errors.price = `outside the band ${low}–${high}`;  // PriceBandViolation
      }
    }
  } else if (form.price.trim() !== '') {
    // MarketOrderWithPrice. The engine rejects rather than ignoring the price,
    // because a client that sent one believes it means something.
    errors.price = 'a MARKET order must not carry a price';
  }

  return errors;
}

export function isValid(errors: FieldErrors): boolean {
  return Object.keys(errors).length === 0;
}

/** Shape the engine's POST /order expects. */
export interface OrderPayload {
  symbol: string;
  clientOrderId: number;
  side: Side;
  type: OrdType;
  quantity: number;
  participant: number;
  price?: number;
}

export function toPayload(form: OrderForm): OrderPayload {
  const payload: OrderPayload = {
    symbol: form.symbol.trim(),
    clientOrderId: Number(form.clientOrderId),
    side: form.side,
    type: form.type,
    quantity: Number(form.quantity),
    participant: Number(form.participant),
  };
  // The key is omitted entirely rather than sent as null: the engine treats a
  // present price on a MARKET order as a client bug.
  if (requiresPrice(form.type)) {
    payload.price = Number(form.price);
  }
  return payload;
}
