# Multi-stage: build the bundle with Node, serve it with nginx.
#
# The runtime image carries no Node at all. A production image that ships a
# dev server to serve static files is both larger and slower than one that
# ships the files, and `vite preview` is explicitly not a production server.

FROM node:20-alpine AS build

WORKDIR /app

# Manifest first so editing a component does not invalidate the npm install
# layer. package-lock.json is copied when present; `npm ci` needs it and falls
# back to `npm install` when it is absent, which is the case on a fresh clone.
COPY package.json package-lock.json* ./
RUN if [ -f package-lock.json ]; then npm ci; else npm install; fi

COPY tsconfig.json vite.config.ts index.html ./
COPY src/ ./src/

# tsc -b before vite build: Vite transpiles without type-checking, so a type
# error would otherwise ship silently. Strict mode is only worth having if the
# build enforces it.
RUN npx tsc --noEmit && npx vite build


FROM nginx:1.27-alpine AS runtime

COPY --from=build /app/dist /usr/share/nginx/html
COPY nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 80

# nginx's default entrypoint daemonises; `daemon off` keeps it in the
# foreground so the container has a process to supervise and stops cleanly.
CMD ["nginx", "-g", "daemon off;"]
