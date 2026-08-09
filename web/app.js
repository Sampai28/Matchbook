// Matchbook depth-of-book ladder viewer.
//
// Vanilla JS, no build step, no dependencies. Polls GET /book/{symbol} and
// redraws the ladder. Polling rather than a websocket because the server is
// single-threaded by design and a long-lived streaming connection would occupy
// its only worker.

(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const ui = {
    symbol: $('symbol'), depth: $('depth'), poll: $('poll'),
    toggle: $('toggle'), refresh: $('refresh'),
    engine: $('engine-badge'), seq: $('seq'), conn: $('conn'),
    bestBid: $('best-bid'), bestAsk: $('best-ask'),
    spread: $('spread'), resting: $('resting'),
    body: $('ladder-body'),
    side: $('o-side'), type: $('o-type'), price: $('o-price'),
    qty: $('o-qty'), part: $('o-part'), send: $('send'), response: $('response'),
  };

  let timer = null;
  let clientOrderId = Date.now() % 1_000_000;

  function setStatus(text, cls) {
    ui.conn.textContent = text;
    ui.conn.className = 'mono ' + cls;
  }

  async function fetchBook() {
    const symbol = ui.symbol.value.trim() || 'MBK';
    const depth = Math.max(1, parseInt(ui.depth.value, 10) || 12);
    try {
      const r = await fetch(`/book/${encodeURIComponent(symbol)}?depth=${depth}`);
      if (!r.ok) {
        const body = await r.text();
        setStatus(`HTTP ${r.status}`, 'status-error');
        renderEmpty(body.slice(0, 200));
        return;
      }
      render(await r.json());
      setStatus(timer ? 'live' : 'ok', 'status-live');
    } catch (err) {
      setStatus('server unreachable', 'status-error');
      renderEmpty('Is matchbook-server running? ' + err.message);
    }
  }

  function renderEmpty(message) {
    ui.body.innerHTML = '';
    const tr = document.createElement('tr');
    const td = document.createElement('td');
    td.colSpan = 5;
    td.className = 'empty';
    td.textContent = message;
    tr.appendChild(td);
    ui.body.appendChild(tr);
  }

  function render(book) {
    ui.engine.textContent = 'engine ' + (book.engine || '—');
    ui.seq.textContent = 'seq ' + (book.seq ?? '—');
    ui.resting.textContent = book.restingOrders ?? '—';

    const bids = book.bids || [];
    const asks = book.asks || [];

    ui.bestBid.textContent = book.bestBid ?? '—';
    ui.bestAsk.textContent = book.bestAsk ?? '—';
    ui.spread.textContent =
      (book.bestBid != null && book.bestAsk != null)
        ? (book.bestAsk - book.bestBid)
        : '—';

    // One scale across both sides so a bar's length is comparable between bid
    // and ask. Scaling each side independently would make a thin ask look as
    // deep as a heavy bid.
    let maxQty = 1;
    for (const l of bids) maxQty = Math.max(maxQty, l.quantity);
    for (const l of asks) maxQty = Math.max(maxQty, l.quantity);

    ui.body.innerHTML = '';

    // Asks are drawn highest price at the top, so the ladder reads the way a
    // trader expects: price descending down the page.
    for (let i = asks.length - 1; i >= 0; i--) {
      ui.body.appendChild(row(null, asks[i], maxQty));
    }

    const sep = document.createElement('tr');
    sep.className = 'spread-row';
    const sepTd = document.createElement('td');
    sepTd.colSpan = 5;
    sepTd.textContent =
      (book.bestBid != null && book.bestAsk != null)
        ? `spread ${book.bestAsk - book.bestBid}`
        : 'one side empty';
    sep.appendChild(sepTd);
    ui.body.appendChild(sep);

    for (const l of bids) {
      ui.body.appendChild(row(l, null, maxQty));
    }

    if (bids.length === 0 && asks.length === 0) {
      renderEmpty('Book is empty. Send an order.');
    }
  }

  function row(bid, ask, maxQty) {
    const tr = document.createElement('tr');

    const bidQty = document.createElement('td');
    const bidOrders = document.createElement('td');
    const price = document.createElement('td');
    const askOrders = document.createElement('td');
    const askQty = document.createElement('td');

    price.className = 'price';

    if (bid) {
      bidQty.className = 'bid-qty';
      bidQty.textContent = bid.quantity;
      bidOrders.textContent = bid.orders;
      price.textContent = bid.price;
      const pct = (bid.quantity / maxQty) * 100;
      bidQty.style.background =
        `linear-gradient(to left, rgba(63,185,80,0.22) ${pct}%, transparent ${pct}%)`;
    }
    if (ask) {
      askQty.className = 'ask-qty';
      askQty.textContent = ask.quantity;
      askOrders.textContent = ask.orders;
      price.textContent = ask.price;
      const pct = (ask.quantity / maxQty) * 100;
      askQty.style.background =
        `linear-gradient(to right, rgba(248,81,73,0.22) ${pct}%, transparent ${pct}%)`;
    }

    tr.append(bidQty, bidOrders, price, askOrders, askQty);
    return tr;
  }

  async function sendOrder() {
    const type = ui.type.value;
    const body = {
      symbol: ui.symbol.value.trim() || 'MBK',
      clientOrderId: ++clientOrderId,
      participant: parseInt(ui.part.value, 10) || 0,
      side: ui.side.value,
      type,
      quantity: parseInt(ui.qty.value, 10) || 0,
    };
    // A MARKET order must not carry a price; the validator rejects it if it does.
    if (type !== 'MARKET') body.price = parseInt(ui.price.value, 10) || 0;

    try {
      const r = await fetch('/order', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      const text = await r.text();
      try {
        ui.response.textContent = JSON.stringify(JSON.parse(text), null, 2);
      } catch {
        ui.response.textContent = text;
      }
      fetchBook();
    } catch (err) {
      ui.response.textContent = 'request failed: ' + err.message;
    }
  }

  function toggle() {
    if (timer) {
      clearInterval(timer);
      timer = null;
      ui.toggle.textContent = 'Start';
      setStatus('paused', 'status-idle');
      return;
    }
    const ms = Math.max(100, parseInt(ui.poll.value, 10) || 500);
    timer = setInterval(fetchBook, ms);
    ui.toggle.textContent = 'Stop';
    fetchBook();
  }

  ui.toggle.addEventListener('click', toggle);
  ui.refresh.addEventListener('click', fetchBook);
  ui.send.addEventListener('click', sendOrder);
  ui.symbol.addEventListener('keydown', (e) => { if (e.key === 'Enter') fetchBook(); });

  // Type drives whether the price field is meaningful.
  ui.type.addEventListener('change', () => {
    const isMarket = ui.type.value === 'MARKET';
    ui.price.disabled = isMarket;
    ui.price.style.opacity = isMarket ? 0.4 : 1;
  });

  fetchBook();
})();
