/**
 * Client-side performance instrumentation.
 *
 * Measures the three things that actually distinguish the naive and optimized
 * render paths:
 *
 *   - **messages/sec sustained** — how much feed the client can absorb.
 *   - **update-to-paint latency** — from a message arriving to the frame that
 *     shows it. This is the number a trader would feel, and it is the one that
 *     diverges most between the two modes.
 *   - **dropped frames** — frames the browser should have painted at 60Hz and
 *     did not, because the main thread was busy rendering.
 *
 * `performance.now()` rather than `Date.now()`: monotonic, unaffected by clock
 * adjustments, and sub-millisecond. `Date.now()` has millisecond granularity,
 * which is coarser than the effect being measured.
 */

const FRAME_BUDGET_MS = 1000 / 60;

export interface PerfSnapshot {
  messagesPerSecond: number;
  messagesTotal: number;
  paintP50: number;
  paintP95: number;
  paintP99: number;
  paintMax: number;
  commits: number;
  droppedFrames: number;
  longestFrameMs: number;
}

export class PerfCollector {
  private messageTimes: number[] = [];
  private paintLatencies: number[] = [];
  private lastFrameAt: number | null = null;

  messagesTotal = 0;
  commits = 0;
  droppedFrames = 0;
  longestFrameMs = 0;

  /** Arrival timestamps of messages not yet reflected on screen. */
  private pending: number[] = [];

  reset(): void {
    this.messageTimes = [];
    this.paintLatencies = [];
    this.pending = [];
    this.lastFrameAt = null;
    this.messagesTotal = 0;
    this.commits = 0;
    this.droppedFrames = 0;
    this.longestFrameMs = 0;
  }

  onMessage(at: number = performance.now()): void {
    this.messagesTotal++;
    this.messageTimes.push(at);
    this.pending.push(at);

    // Keep only the last second for the rate calculation. An all-time average
    // would hide the fact that throughput collapsed thirty seconds ago.
    const cutoff = at - 1000;
    while (this.messageTimes.length > 0 && (this.messageTimes[0] ?? 0) < cutoff) {
      this.messageTimes.shift();
    }

    // Bound the pending list. Under the naive path at high rates this can grow
    // faster than frames retire it, and an unbounded array here would be
    // measuring memory pressure rather than latency.
    if (this.pending.length > 5000) {
      this.pending.splice(0, this.pending.length - 5000);
    }
  }

  /**
   * Called from a requestAnimationFrame callback scheduled *after* React has
   * committed. That is the closest a page can get to "the user saw it" without
   * the Element Timing API, and it is consistent between both modes, which is
   * what matters for a comparison.
   */
  onPainted(at: number = performance.now()): void {
    this.commits++;

    for (const queuedAt of this.pending) {
      this.paintLatencies.push(at - queuedAt);
    }
    this.pending.length = 0;

    if (this.paintLatencies.length > 20_000) {
      this.paintLatencies.splice(0, this.paintLatencies.length - 20_000);
    }

    if (this.lastFrameAt !== null) {
      const gap = at - this.lastFrameAt;
      if (gap > this.longestFrameMs) this.longestFrameMs = gap;
      // A gap of more than roughly 1.5 frames means at least one refresh went
      // by unpainted. Using 1.5 rather than 1.0 avoids counting ordinary
      // scheduling jitter as a dropped frame.
      if (gap > FRAME_BUDGET_MS * 1.5) {
        this.droppedFrames += Math.floor(gap / FRAME_BUDGET_MS) - 1;
      }
    }
    this.lastFrameAt = at;
  }

  private percentile(p: number): number {
    if (this.paintLatencies.length === 0) return 0;
    const sorted = [...this.paintLatencies].sort((a, b) => a - b);
    const index = Math.min(
      sorted.length - 1,
      Math.max(0, Math.ceil((p / 100) * sorted.length) - 1),
    );
    return sorted[index] ?? 0;
  }

  snapshot(): PerfSnapshot {
    return {
      messagesPerSecond: this.messageTimes.length,
      messagesTotal: this.messagesTotal,
      paintP50: this.percentile(50),
      paintP95: this.percentile(95),
      paintP99: this.percentile(99),
      paintMax: this.paintLatencies.length
        ? Math.max(...this.paintLatencies)
        : 0,
      commits: this.commits,
      droppedFrames: this.droppedFrames,
      longestFrameMs: this.longestFrameMs,
    };
  }
}
