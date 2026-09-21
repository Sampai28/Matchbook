/**
 * The SSE subscription, gap recovery, and the two render paths being compared.
 *
 * ## naive vs optimized
 *
 * **naive** commits to React state on every single message, and rebuilds the
 * whole ladder from scratch each time. This is what an app written the obvious
 * way does, and it is not a straw man — it is the default shape of
 * `setState(newBook)` inside an `onmessage` handler.
 *
 * **optimized** applies deltas to a mutable Book outside React entirely, and
 * commits once per animation frame. The intermediate states are never rendered
 * because they could never have been seen: the display refreshes 60 times a
 * second regardless of how many messages arrive.
 *
 * Both paths consume the identical message stream and produce the identical
 * final book, so the comparison isolates render strategy and nothing else.
 */

import { useCallback, useEffect, useRef, useState } from 'react';

import { Book, classifySeq, type LadderRow, type Trade } from './book';
import { PerfCollector, type PerfSnapshot } from '../perf/metrics';
import { parseMessage, type WireMessage } from '../protocol/types';

export type RenderMode = 'naive' | 'optimized';
export type ConnectionState = 'idle' | 'connecting' | 'live' | 'error';

export interface BookView {
  rows: LadderRow[];
  maxQuantity: number;
  bestBid: number | null;
  bestAsk: number | null;
  trades: Trade[];
  seq: number;
  engine: string;
  restingOrders: number;
}

const EMPTY_VIEW: BookView = {
  rows: [],
  maxQuantity: 0,
  bestBid: null,
  bestAsk: null,
  trades: [],
  seq: 0,
  engine: '',
  restingOrders: 0,
};

const MAX_LADDER_ROWS = 400;

export interface StreamOptions {
  symbol: string;
  mode: RenderMode;
  enabled: boolean;
}

export interface StreamResult {
  view: BookView;
  connection: ConnectionState;
  perf: PerfSnapshot;
  resnapshots: number;
  resetPerf: () => void;
}

export function useBookStream({ symbol, mode, enabled }: StreamOptions): StreamResult {
  const bookRef = useRef(new Book());
  const perfRef = useRef(new PerfCollector());
  const frameRef = useRef<number | null>(null);
  const dirtyRef = useRef(false);
  const paintPendingRef = useRef(false);

  const [view, setView] = useState<BookView>(EMPTY_VIEW);
  const [connection, setConnection] = useState<ConnectionState>('idle');
  const [perf, setPerf] = useState<PerfSnapshot>(() => perfRef.current.snapshot());
  const [resnapshots, setResnapshots] = useState(0);

  // Read inside callbacks that must not be re-created when the mode flips —
  // re-creating them would tear down and rebuild the EventSource, which would
  // resnapshot and reset the very measurement being compared.
  const modeRef = useRef(mode);
  modeRef.current = mode;

  const buildView = useCallback((): BookView => {
    const book = bookRef.current;
    const rows = book.ladder(MAX_LADDER_ROWS);
    return {
      rows,
      maxQuantity: book.maxQuantity(rows),
      bestBid: book.bestBid(),
      bestAsk: book.bestAsk(),
      // Copied so React sees a new reference; the Book mutates its array in
      // place and a shared reference would make memo comparisons wrong.
      trades: book.trades.slice(0, 60),
      seq: book.seq,
      engine: book.engine,
      restingOrders: book.restingOrders,
    };
  }, []);

  const commit = useCallback(() => {
    setView(buildView());
    paintPendingRef.current = true;
  }, [buildView]);

  const scheduleCommit = useCallback(() => {
    dirtyRef.current = true;
    if (frameRef.current !== null) return;   // a frame is already queued
    frameRef.current = requestAnimationFrame(() => {
      frameRef.current = null;
      if (!dirtyRef.current) return;
      dirtyRef.current = false;
      commit();
    });
  }, [commit]);

  // Measure paint after React has committed. The nested rAF is deliberate:
  // useEffect runs after commit but before the browser paints, so scheduling
  // one frame out is the closest approximation to "on screen" available
  // without the Element Timing API.
  useEffect(() => {
    if (!paintPendingRef.current) return;
    paintPendingRef.current = false;
    const handle = requestAnimationFrame(() => {
      perfRef.current.onPainted();
    });
    return () => cancelAnimationFrame(handle);
  }, [view]);

  const resetPerf = useCallback(() => {
    perfRef.current.reset();
    setPerf(perfRef.current.snapshot());
  }, []);

  const resnapshot = useCallback(async (reason: string) => {
    try {
      const response = await fetch(
        `/snapshot/${encodeURIComponent(symbol)}?depth=500`,
      );
      if (!response.ok) return;
      const message = parseMessage(await response.text());
      if (message && message.type === 'snapshot') {
        bookRef.current.applySnapshot(message);
        commit();
        setResnapshots((n) => n + 1);
      }
    } catch {
      // A failed resnapshot is not fatal: the stream is still open and the
      // next gap will trigger another attempt. Logging every failure would
      // spam the console during a disconnect.
      void reason;
    }
  }, [symbol, commit]);

  const handleMessage = useCallback(
    (raw: string) => {
      const message: WireMessage | null = parseMessage(raw);
      if (message === null) return;

      perfRef.current.onMessage();

      const book = bookRef.current;

      if (message.type === 'error') {
        setConnection('error');
        return;
      }

      if (message.type === 'snapshot') {
        book.applySnapshot(message);
      } else {
        const action = classifySeq(book.seq, message.seq);
        if (action === 'ignored') {
          // Heartbeat. Counted as a message for the rate, but it changes
          // nothing, so committing would be a wasted render in naive mode and
          // a wasted frame in optimized mode.
          return;
        }
        if (action === 'gap') {
          void resnapshot(`gap ${book.seq} -> ${message.seq}`);
          return;
        }
        if (message.type === 'delta') {
          book.applyDelta(message);
        }
      }

      if (modeRef.current === 'naive') {
        // Commit immediately, once per message. Under load this is many
        // setState calls per frame, each rebuilding the full ladder array.
        commit();
      } else {
        scheduleCommit();
      }
    },
    [commit, scheduleCommit, resnapshot],
  );

  useEffect(() => {
    if (!enabled) {
      setConnection('idle');
      return;
    }

    bookRef.current.reset();
    setView(EMPTY_VIEW);
    setConnection('connecting');

    const source = new EventSource(`/stream/${encodeURIComponent(symbol)}?depth=500`);

    const onAny = (event: MessageEvent<string>) => {
      setConnection('live');
      handleMessage(event.data);
    };

    // Named events, because the gateway sets `event:` on every frame and a
    // plain `onmessage` handler only receives frames with no event name.
    source.addEventListener('snapshot', onAny as EventListener);
    source.addEventListener('delta', onAny as EventListener);
    source.addEventListener('heartbeat', onAny as EventListener);
    source.onmessage = onAny;
    source.onerror = () => {
      // EventSource reconnects on its own. The sequence gap that reconnection
      // creates is detected by classifySeq and recovered by resnapshot, so
      // there is nothing to do here but reflect the state.
      setConnection('connecting');
    };

    return () => {
      source.close();
      if (frameRef.current !== null) {
        cancelAnimationFrame(frameRef.current);
        frameRef.current = null;
      }
    };
  }, [symbol, enabled, handleMessage]);

  // Poll the collector for display. Reading it into React state on every
  // message would itself be a per-message render — instrumentation that
  // changed the thing it measures.
  useEffect(() => {
    const id = window.setInterval(() => {
      setPerf(perfRef.current.snapshot());
    }, 250);
    return () => window.clearInterval(id);
  }, []);

  return { view, connection, perf, resnapshots, resetPerf };
}
