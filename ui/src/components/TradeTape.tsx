import { memo } from 'react';

import type { Trade } from '../book/book';

interface TapeProps {
  trades: Trade[];
}

const TapeRow = memo(function TapeRow({ trade }: { trade: Trade }) {
  return (
    <div className={`tape-row ${trade.takerSide === 'BUY' ? 'taker-buy' : 'taker-sell'}`}>
      <span className="tape-price">{trade.price}</span>
      <span className="tape-qty">{trade.quantity}</span>
      <span className="tape-side">{trade.takerSide}</span>
    </div>
  );
});

export function TradeTape({ trades }: TapeProps) {
  return (
    <div className="panel tape">
      <h2>tape</h2>
      <div className="tape-rows">
        {trades.length === 0 ? (
          <div className="empty">no trades yet</div>
        ) : (
          // Keyed by the client-assigned trade id rather than array index.
          // Trades are prepended, so an index key would change the key of every
          // existing row on each print and re-render the whole tape.
          trades.map((trade) => <TapeRow key={trade.id} trade={trade} />)
        )}
      </div>
    </div>
  );
}
