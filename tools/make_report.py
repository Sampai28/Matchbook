#!/usr/bin/env python3
"""Turns bench/results/*.txt into bench/report.html.

Produces latency CDFs for V0-V3 overlaid, a percentile table, and a throughput
bar chart.

THIS HAS NEVER BEEN RUN. bench/results/ is empty, and running it now would
produce an empty report. It degrades gracefully and says so rather than
crashing or, worse, emitting a plausible-looking chart of nothing.

    python3 -m venv .venv && . .venv/bin/activate
    pip install -r tools/requirements.txt
    python3 tools/make_report.py
"""

from __future__ import annotations

import argparse
import glob
import os
import sys
from collections import defaultdict

VERSION_ORDER = ["v0", "v1", "v2", "v3"]
VERSION_LABEL = {
    "v0": "V0 — naive (std::map + std::list)",
    "v1": "V1 — pooled + intrusive lists",
    "v2": "V2 — flat price array + bitmap",
    "v3": "V3 — hot-path tuned",
}
VERSION_COLOR = {"v0": "#8b949e", "v1": "#58a6ff", "v2": "#a371f7", "v3": "#3fb950"}
OPERATIONS = ["insert", "cancel", "match"]


def parse_results(path: str) -> tuple[dict, dict]:
    """Returns (metrics, fingerprint).

    metrics[version][operation][metric] = float
    """
    metrics: dict = defaultdict(lambda: defaultdict(dict))
    fingerprint: dict = {}

    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            if line.startswith("#"):
                body = line.lstrip("#").strip()
                if ":" in body:
                    k, _, v = body.partition(":")
                    fingerprint[k.strip()] = v.strip()
                continue
            parts = line.split()
            if len(parts) != 4:
                continue
            version, operation, metric, value = parts
            try:
                metrics[version][operation][metric] = float(value)
            except ValueError:
                continue
    return metrics, fingerprint


def newest_results(results_dir: str) -> str | None:
    files = sorted(glob.glob(os.path.join(results_dir, "*.txt")))
    return files[-1] if files else None


def write_placeholder(out_path: str, results_dir: str) -> None:
    """Emitted when there is nothing to report. Says so unmistakably."""
    html = f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<title>Matchbook — no benchmark results</title>
<style>
  body {{ background:#0d1117; color:#e6edf3; font:15px/1.6 system-ui,sans-serif;
          max-width:760px; margin:60px auto; padding:0 20px; }}
  h1 {{ font-size:22px; }}
  code {{ background:#1c232d; padding:2px 6px; border-radius:4px;
          font-family:ui-monospace,Menlo,Consolas,monospace; font-size:13px; }}
  .warn {{ border:1px solid #f85149; background:rgba(248,81,73,0.08);
           border-radius:8px; padding:16px; margin:24px 0; }}
  pre {{ background:#161b22; border:1px solid #262d38; border-radius:8px;
         padding:14px; overflow-x:auto; font-size:13px; }}
</style></head><body>
<h1>Matchbook — no benchmark results</h1>
<div class="warn">
  <strong>Nothing has been measured.</strong>
  <code>{results_dir}/</code> contains no result files, so there is no report to
  generate. This page exists so that an empty result set produces an obvious
  message rather than an empty chart that could be mistaken for data.
</div>
<p>To produce real results, build natively under WSL2 and run the harness:</p>
<pre>cmake --preset release
cmake --build --preset release
./bench/run_bench.sh
python3 tools/make_report.py</pre>
<p>Benchmarks must run natively rather than in Docker: containerised timing on a
Windows host adds jitter that swamps nanosecond-resolution measurement.</p>
<p>Until this page shows real numbers, treat every performance figure in
<code>README.md</code> and <code>docs/expected-performance.md</code> as an
unverified projection.</p>
</body></html>
"""
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(html)


def build_report(metrics: dict, fingerprint: dict, source: str, out_path: str) -> None:
    import plotly.graph_objects as go
    from plotly.subplots import make_subplots

    present = [v for v in VERSION_ORDER if v in metrics]

    # --- percentile "CDF" ---------------------------------------------------
    # The harness records full HdrHistograms but writes only the headline
    # percentiles, so this is a percentile curve through the points that exist,
    # not a true CDF. Labelled honestly rather than dressed up.
    cdf = make_subplots(
        rows=1, cols=len(OPERATIONS),
        subplot_titles=[o.capitalize() for o in OPERATIONS],
        shared_yaxes=False,
    )
    pct_points = [50, 99, 99.9]
    pct_keys = ["p50_ns", "p99_ns", "p999_ns"]

    for col, op in enumerate(OPERATIONS, start=1):
        for v in present:
            data = metrics[v].get(op, {})
            ys = [data.get(k) for k in pct_keys]
            if all(y is None for y in ys):
                continue
            cdf.add_trace(
                go.Scatter(
                    x=pct_points, y=ys, mode="lines+markers",
                    name=VERSION_LABEL[v], legendgroup=v,
                    showlegend=(col == 1),
                    line=dict(color=VERSION_COLOR[v], width=2),
                ),
                row=1, col=col,
            )
        cdf.update_xaxes(title_text="percentile", row=1, col=col)
        cdf.update_yaxes(title_text="latency (ns)" if col == 1 else None,
                         type="log", row=1, col=col)

    cdf.update_layout(
        template="plotly_dark", height=420,
        margin=dict(l=60, r=20, t=60, b=50),
        legend=dict(orientation="h", y=-0.22),
        title="Latency by percentile (log scale), median of repetitions",
    )

    # --- throughput ---------------------------------------------------------
    bar = go.Figure()
    for op in OPERATIONS:
        bar.add_trace(go.Bar(
            name=op.capitalize(),
            x=[VERSION_LABEL[v] for v in present],
            y=[metrics[v].get(op, {}).get("throughput_ops_per_sec", 0) for v in present],
        ))
    bar.update_layout(
        template="plotly_dark", barmode="group", height=380,
        margin=dict(l=60, r=20, t=60, b=80),
        title="Throughput (operations/sec, median of repetitions)",
        yaxis_title="ops/sec",
    )

    # --- percentile table ---------------------------------------------------
    rows = []
    for v in present:
        for op in OPERATIONS:
            d = metrics[v].get(op, {})
            if not d:
                continue
            rows.append([
                VERSION_LABEL[v], op,
                f"{d.get('p50_ns', 0):,.0f}",
                f"{d.get('p99_ns', 0):,.0f}",
                f"{d.get('p999_ns', 0):,.0f}",
                f"{d.get('throughput_ops_per_sec', 0):,.0f}",
                f"{d.get('p50_spread_pct', 0):.1f}%",
            ])

    header = ["Version", "Operation", "p50 (ns)", "p99 (ns)", "p99.9 (ns)",
              "ops/sec", "run spread"]
    table_html = ["<table><thead><tr>"]
    table_html += [f"<th>{h}</th>" for h in header]
    table_html.append("</tr></thead><tbody>")
    for r in rows:
        table_html.append("<tr>" + "".join(f"<td>{c}</td>" for c in r) + "</tr>")
    table_html.append("</tbody></table>")

    fp_html = "".join(
        f"<div><span class='k'>{k}</span><span class='v'>{val}</span></div>"
        for k, val in fingerprint.items()
    )

    html = f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<title>Matchbook benchmark report</title>
<style>
  body {{ background:#0d1117; color:#e6edf3; font:15px/1.6 system-ui,sans-serif;
          max-width:1100px; margin:40px auto; padding:0 20px; }}
  h1 {{ font-size:24px; margin-bottom:4px; }}
  h2 {{ font-size:13px; text-transform:uppercase; letter-spacing:.08em;
        color:#8b949e; margin-top:34px; }}
  .src {{ color:#8b949e; font-size:13px; }}
  .fp {{ background:#151b23; border:1px solid #262d38; border-radius:8px;
         padding:14px; margin:18px 0; font-size:13px;
         display:grid; grid-template-columns:repeat(auto-fit,minmax(300px,1fr)); gap:4px 20px; }}
  .fp .k {{ color:#8b949e; display:inline-block; min-width:130px; }}
  .fp .v {{ font-family:ui-monospace,Menlo,Consolas,monospace; }}
  table {{ width:100%; border-collapse:collapse; font-size:13px;
           font-family:ui-monospace,Menlo,Consolas,monospace; }}
  th {{ text-align:right; color:#8b949e; font-weight:500; padding:8px;
        border-bottom:1px solid #262d38; font-size:11px; text-transform:uppercase; }}
  th:first-child, td:first-child {{ text-align:left; }}
  td {{ text-align:right; padding:6px 8px; border-bottom:1px solid #1c232d; }}
  .caveat {{ border:1px solid #d29922; background:rgba(210,153,34,0.08);
             border-radius:8px; padding:14px; margin:24px 0; font-size:14px; }}
</style>
<script src="https://cdn.plot.ly/plotly-2.27.0.min.js"></script>
</head><body>
<h1>Matchbook benchmark report</h1>
<div class="src">source: {os.path.basename(source)}</div>

<div class="caveat">
  <strong>Read the deltas, not the absolutes.</strong> These figures come from a
  laptop whose frequency scaling is governed by the host. Absolute nanosecond
  values will drift between runs and between machines. What is trustworthy is
  the relative difference between V0-V3 measured in the same run, on the same
  workload, on the same core. The <em>run spread</em> column is the error bar:
  a difference between versions smaller than that column is not a result.
</div>

<h2>Machine</h2>
<div class="fp">{fp_html}</div>

<h2>Latency</h2>
<div id="cdf"></div>

<h2>Throughput</h2>
<div id="bar"></div>

<h2>Percentiles</h2>
{''.join(table_html)}

<script>
  Plotly.newPlot("cdf", {cdf.to_json()}.data, {cdf.to_json()}.layout, {{responsive:true}});
  Plotly.newPlot("bar", {bar.to_json()}.data, {bar.to_json()}.layout, {{responsive:true}});
</script>
</body></html>
"""
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(html)


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate the Matchbook benchmark report.")
    ap.add_argument("--results", default="bench/results", help="results directory")
    ap.add_argument("--out", default="bench/report.html", help="output HTML path")
    ap.add_argument("--input", default=None, help="a specific results file to use")
    args = ap.parse_args()

    source = args.input or newest_results(args.results)

    if source is None:
        print(f"No benchmark results found in {args.results}/.")
        print()
        print("This is the expected state of a freshly generated repository:")
        print("nothing has been compiled or run, so there is nothing to plot.")
        print()
        print("To produce results (natively under WSL2, not in Docker):")
        print("  cmake --preset release")
        print("  cmake --build --preset release")
        print("  ./bench/run_bench.sh")
        print()
        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
        write_placeholder(args.out, args.results)
        print(f"Wrote a placeholder page to {args.out} explaining the empty state.")
        return 0

    metrics, fingerprint = parse_results(source)
    if not metrics:
        print(f"{source} contains no parseable metric lines.", file=sys.stderr)
        print("Expected lines of the form: <version> <operation> <metric> <value>",
              file=sys.stderr)
        return 1

    try:
        import plotly  # noqa: F401
    except ImportError:
        print("plotly is not installed. Install the tooling requirements first:",
              file=sys.stderr)
        print("  python3 -m venv .venv && . .venv/bin/activate", file=sys.stderr)
        print("  pip install -r tools/requirements.txt", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    build_report(metrics, fingerprint, source, args.out)
    print(f"Wrote {args.out} from {source}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
