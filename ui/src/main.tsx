import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import { App } from './App';
import './styles.css';

const container = document.getElementById('root');
if (!container) {
  throw new Error('#root is missing from index.html');
}

// StrictMode double-invokes effects in development, which for an EventSource
// means connect, disconnect, reconnect on mount. That is intentional on React's
// part — it surfaces effects that do not clean up properly — and the stream
// hook handles it because its cleanup closes the source. Worth knowing when
// the network tab shows two connections in dev and one in the build.
createRoot(container).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
