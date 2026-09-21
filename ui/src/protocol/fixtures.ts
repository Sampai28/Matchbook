/**
 * Frames captured verbatim from the running C++ gateway.
 *
 * Not hand-written. These were copied out of an actual
 * `curl -N http://localhost:8080/stream/MBK` session, which is the entire
 * point: a fixture invented alongside the TypeScript types would agree with
 * them by construction and prove nothing. These agree with the C++ or the test
 * fails.
 *
 * Regenerate with:
 *   docker compose -f docker/docker-compose.yml up -d server
 *   curl -sN --max-time 6 http://localhost:8080/stream/MBK
 */

export const CAPTURED_SNAPSHOT =
  '{"type":"snapshot","seq":1,"symbol":"MBK","engine":"v3",' +
  '"bids":[{"price":100000,"quantity":50,"orders":1}],' +
  '"asks":[{"price":100100,"quantity":40,"orders":1}],"restingOrders":2}';

export const CAPTURED_DELTA_ADD =
  '{"type":"delta","seq":2,"symbol":"MBK",' +
  '"levels":[{"side":"BID","price":99930,"quantity":30,"orders":1}],"trades":[]}';

export const CAPTURED_DELTA_TRADE =
  '{"type":"delta","seq":5,"symbol":"MBK",' +
  '"levels":[{"side":"ASK","price":100100,"quantity":0,"orders":0}],' +
  '"trades":[{"price":100100,"quantity":40,"takerSide":"BUY"}]}';

export const CAPTURED_HEARTBEAT =
  '{"type":"heartbeat","seq":5,"symbol":"MBK"}';

/** A full SSE body, framing included, exactly as the browser receives it. */
export const CAPTURED_SSE_BODY = [
  'event: snapshot',
  `data: ${CAPTURED_SNAPSHOT}`,
  '',
  'event: delta',
  `data: ${CAPTURED_DELTA_ADD}`,
  '',
  'event: delta',
  `data: ${CAPTURED_DELTA_TRADE}`,
  '',
  'event: heartbeat',
  `data: ${CAPTURED_HEARTBEAT}`,
  '',
  '',
].join('\n');
