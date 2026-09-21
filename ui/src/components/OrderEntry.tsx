import { useCallback, useMemo, useState } from 'react';

import {
  DEFAULT_RULES,
  isValid,
  requiresPrice,
  toPayload,
  validateOrder,
  type OrderForm,
  type OrdType,
  type Side,
} from '../orders/validation';

const TYPES: OrdType[] = ['LIMIT', 'MARKET', 'IOC', 'FOK', 'POST_ONLY'];

interface Outcome {
  ok: boolean;
  text: string;
}

export function OrderEntry({ symbol }: { symbol: string }) {
  const [form, setForm] = useState<OrderForm>({
    symbol,
    clientOrderId: '1001',
    side: 'BUY',
    type: 'LIMIT',
    quantity: '25',
    price: '100000',
    participant: '1',
  });
  const [outcome, setOutcome] = useState<Outcome | null>(null);
  const [inFlight, setInFlight] = useState(false);

  const errors = useMemo(() => validateOrder(form, DEFAULT_RULES), [form]);
  const submittable = isValid(errors) && !inFlight;

  const set = useCallback(
    <K extends keyof OrderForm>(key: K, value: OrderForm[K]) => {
      setForm((prev) => ({ ...prev, [key]: value }));
    },
    [],
  );

  const submit = useCallback(
    async (event: React.FormEvent) => {
      event.preventDefault();
      if (!submittable) return;
      setInFlight(true);
      setOutcome(null);
      try {
        const response = await fetch('/order', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(toPayload(form)),
        });
        const body = (await response.json()) as {
          ok?: boolean;
          reason?: string;
          filledQuantity?: number;
          remainingQuantity?: number;
          error?: string;
          message?: string;
        };

        if (response.ok && body.ok) {
          setOutcome({
            ok: true,
            text: `accepted · filled ${body.filledQuantity ?? 0} · resting ${body.remainingQuantity ?? 0}`,
          });
          // Advance the id so the next submit does not collide with this one —
          // the engine rejects a duplicate client order id.
          set('clientOrderId', String(Number(form.clientOrderId) + 1));
        } else {
          // The engine's reason is shown verbatim. Translating it into
          // friendlier prose would hide which gate actually fired, and those
          // names are the vocabulary the rest of the system uses.
          setOutcome({
            ok: false,
            text: body.reason ?? body.message ?? body.error ?? `HTTP ${response.status}`,
          });
        }
      } catch (error) {
        setOutcome({ ok: false, text: error instanceof Error ? error.message : 'request failed' });
      } finally {
        setInFlight(false);
      }
    },
    [form, submittable, set],
  );

  return (
    <form className="panel order-entry" onSubmit={submit}>
      <h2>order entry</h2>

      <label>
        symbol
        <input value={form.symbol} onChange={(e) => set('symbol', e.target.value)} />
      </label>
      {errors.symbol && <p className="field-error">{errors.symbol}</p>}

      <div className="side-toggle">
        {(['BUY', 'SELL'] as Side[]).map((side) => (
          <button
            key={side}
            type="button"
            className={form.side === side ? `active ${side.toLowerCase()}` : ''}
            onClick={() => set('side', side)}
          >
            {side}
          </button>
        ))}
      </div>

      <label>
        type
        <select value={form.type} onChange={(e) => set('type', e.target.value as OrdType)}>
          {TYPES.map((type) => (
            <option key={type} value={type}>
              {type}
            </option>
          ))}
        </select>
      </label>

      <label>
        quantity
        <input value={form.quantity} onChange={(e) => set('quantity', e.target.value)} />
      </label>
      {errors.quantity && <p className="field-error">{errors.quantity}</p>}

      <label>
        price
        <input
          value={form.price}
          disabled={!requiresPrice(form.type)}
          placeholder={requiresPrice(form.type) ? '' : 'n/a for MARKET'}
          onChange={(e) => set('price', e.target.value)}
        />
      </label>
      {errors.price && <p className="field-error">{errors.price}</p>}

      <div className="row-2">
        <label>
          client id
          <input
            value={form.clientOrderId}
            onChange={(e) => set('clientOrderId', e.target.value)}
          />
        </label>
        <label>
          participant
          <input
            value={form.participant}
            onChange={(e) => set('participant', e.target.value)}
          />
        </label>
      </div>
      {errors.clientOrderId && <p className="field-error">{errors.clientOrderId}</p>}
      {errors.participant && <p className="field-error">{errors.participant}</p>}

      <button type="submit" className={`submit ${form.side.toLowerCase()}`} disabled={!submittable}>
        {inFlight ? 'sending…' : `${form.side} ${form.quantity}`}
      </button>

      {outcome && (
        <p className={outcome.ok ? 'outcome ok' : 'outcome bad'}>{outcome.text}</p>
      )}
    </form>
  );
}
