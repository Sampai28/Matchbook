import { useCallback, useState } from 'react';

import { Ladder } from './components/Ladder';
import { OrderEntry } from './components/OrderEntry';
import { StatsPanel } from './components/StatsPanel';
import { TradeTape } from './components/TradeTape';
import { useBookStream, type RenderMode } from './book/useBookStream';

export function App() {
  const [symbol, setSymbol] = useState('MBK');
  const [mode, setMode] = useState<RenderMode>('optimized');
  const [connected, setConnected] = useState(true);

  const { view, connection, perf, resnapshots, resetPerf } = useBookStream({
    symbol,
    mode,
    enabled: connected,
  });

  // Flipping mode resets the measurement. Carrying counters across the switch
  // would average the two paths together and make the comparison meaningless.
  const switchMode = useCallback(
    (next: RenderMode) => {
      if (next === mode) return;
      setMode(next);
      resetPerf();
    },
    [mode, resetPerf],
  );

  const spread =
    view.bestBid !== null && view.bestAsk !== null ? view.bestAsk - view.bestBid : null;

  return (
    <div className="app">
      <header>
        <h1>matchbook</h1>

        <label className="symbol">
          symbol
          <input value={symbol} onChange={(e) => setSymbol(e.target.value.toUpperCase())} />
        </label>

        <div className="mode-toggle">
          <span>render</span>
          {(['naive', 'optimized'] as RenderMode[]).map((candidate) => (
            <button
              key={candidate}
              className={mode === candidate ? 'active' : ''}
              onClick={() => switchMode(candidate)}
            >
              {candidate}
            </button>
          ))}
        </div>

        <button className="secondary" onClick={() => setConnected((on) => !on)}>
          {connected ? 'disconnect' : 'connect'}
        </button>
        <button className="secondary" onClick={resetPerf}>
          reset metrics
        </button>

        <div className={`conn ${connection}`}>{connection}</div>
      </header>

      <div className="top-of-book">
        <span className="bid">bid {view.bestBid ?? '—'}</span>
        <span className="spread">spread {spread ?? '—'}</span>
        <span className="ask">ask {view.bestAsk ?? '—'}</span>
        <span className="seq">seq {view.seq}</span>
        <span className="engine">{view.engine || '—'}</span>
      </div>

      <main>
        <Ladder
          rows={view.rows}
          maxQuantity={view.maxQuantity}
          bestBid={view.bestBid}
          bestAsk={view.bestAsk}
          // The naive path renders every row, which is the other half of what
          // makes it slow: no virtualization and a commit per message.
          virtualized={mode === 'optimized'}
        />
        <div className="side-column">
          <OrderEntry symbol={symbol} />
          <TradeTape trades={view.trades} />
        </div>
        <StatsPanel perf={perf} resnapshots={resnapshots} mode={mode} />
      </main>
    </div>
  );
}
