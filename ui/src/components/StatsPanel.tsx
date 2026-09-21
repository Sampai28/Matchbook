import { useEffect, useState } from 'react';

import type { PerfSnapshot } from '../perf/metrics';
import type { EngineStats } from '../protocol/types';

interface StatsProps {
  perf: PerfSnapshot;
  resnapshots: number;
  mode: string;
}

/** Nanoseconds from the engine; formatted so ns and µs are both readable. */
function ns(value: number): string {
  if (value === 0) return '—';
  if (value < 1000) return `${value} ns`;
  if (value < 1_000_000) return `${(value / 1000).toFixed(1)} µs`;
  return `${(value / 1_000_000).toFixed(2)} ms`;
}

function ms(value: number): string {
  return value === 0 ? '—' : `${value.toFixed(1)} ms`;
}

export function StatsPanel({ perf, resnapshots, mode }: StatsProps) {
  const [engine, setEngine] = useState<EngineStats | null>(null);

  useEffect(() => {
    let cancelled = false;
    const poll = async () => {
      try {
        const response = await fetch('/stats/engine');
        if (!response.ok) return;
        const body = (await response.json()) as EngineStats;
        if (!cancelled) setEngine(body);
      } catch {
        // The stats panel going stale is not worth surfacing as an error;
        // the connection indicator already shows if the server is gone.
      }
    };
    void poll();
    // 1Hz. The engine percentiles move slowly and polling faster would add
    // renders to the very thing being measured.
    const id = window.setInterval(poll, 1000);
    return () => {
      cancelled = true;
      window.clearInterval(id);
    };
  }, []);

  return (
    <div className="panel stats">
      <h2>stats</h2>

      <h3>engine ({engine?.engine ?? '—'})</h3>
      <dl>
        <div><dt>submit p50</dt><dd>{ns(engine?.submit.p50 ?? 0)}</dd></div>
        <div><dt>submit p99</dt><dd>{ns(engine?.submit.p99 ?? 0)}</dd></div>
        <div><dt>submit p99.9</dt><dd>{ns(engine?.submit.p999 ?? 0)}</dd></div>
        <div><dt>cancel p50</dt><dd>{ns(engine?.cancel.p50 ?? 0)}</dd></div>
        <div><dt>accepted</dt><dd>{engine?.counters.ordersAccepted ?? 0}</dd></div>
        <div><dt>rejected</dt><dd>{engine?.counters.ordersRejected ?? 0}</dd></div>
        <div><dt>resting</dt><dd>{engine?.counters.ordersResting ?? 0}</dd></div>
        <div><dt>fills</dt><dd>{engine?.counters.fills ?? 0}</dd></div>
      </dl>

      <h3>client ({mode})</h3>
      <dl>
        <div><dt>messages/sec</dt><dd>{perf.messagesPerSecond}</dd></div>
        <div><dt>messages total</dt><dd>{perf.messagesTotal}</dd></div>
        <div><dt>paint p50</dt><dd>{ms(perf.paintP50)}</dd></div>
        <div><dt>paint p95</dt><dd>{ms(perf.paintP95)}</dd></div>
        <div><dt>paint p99</dt><dd>{ms(perf.paintP99)}</dd></div>
        <div><dt>paint max</dt><dd>{ms(perf.paintMax)}</dd></div>
        <div><dt>commits</dt><dd>{perf.commits}</dd></div>
        <div><dt>dropped frames</dt><dd>{perf.droppedFrames}</dd></div>
        <div><dt>longest frame</dt><dd>{ms(perf.longestFrameMs)}</dd></div>
      </dl>

      <h3>stream</h3>
      <dl>
        <div><dt>clients</dt><dd>{engine?.streams.clients ?? 0}</dd></div>
        <div><dt>sent</dt><dd>{engine?.streams.messagesSent ?? 0}</dd></div>
        {/* Non-zero means a client fell behind and had to resnapshot. */}
        <div><dt>dropped</dt><dd>{engine?.streams.dropped ?? 0}</dd></div>
        <div><dt>resnapshots</dt><dd>{resnapshots}</dd></div>
      </dl>
    </div>
  );
}
