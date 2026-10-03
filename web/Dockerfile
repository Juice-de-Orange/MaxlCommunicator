# =============================================================================
# MaxlCommunicator dashboard + bridge.
#
# Build context is the repository root, not web/. Two reasons, and both are
# load-bearing:
#
#   * The dashboard imports the bridge's protocol decoder (`@protocol`) rather
#     than keeping a second copy of it, so the bridge sources have to be here.
#   * The PWA is served as static assets from the dashboard's own origin. Web
#     Bluetooth needs a secure context, and splitting the two across origins
#     would mean a second certificate and a cross-origin POST to the ingest
#     endpoint for no gain at all.
#
#   docker build -f web/Dockerfile -t maxl-web .
# =============================================================================

# node:22-slim (Debian, glibc) rather than -alpine (musl), and this is not a
# preference.
#
# package-lock.json is generated on a glibc machine, so the optional native
# packages it pins are the *-linux-x64-gnu ones. `npm ci` installs exactly what
# the lock says, and on musl those cannot load:
#
#   Error: Cannot find native binding.
#     cause: Cannot find module '@rolldown/binding-wasm32-wasi'
#
# The wasm fallback is not in the lock either, because it was never needed where
# the lock was written. Regenerating the lock inside the image would fix the
# symptom and throw away the reproducibility that `npm ci` is for.
FROM node:22-slim AS bridge-build
WORKDIR /src/bridge
# npm's defaults give up quickly on a slow registry, and the failure it reports
# is ETIMEDOUT with a paragraph about proxy settings -- which sends you looking
# in entirely the wrong place. This build has failed that way once already.
RUN npm config set fetch-retries 6 \
 && npm config set fetch-retry-maxtimeout 120000 \
 && npm config set fetch-timeout 300000
COPY bridge/package.json bridge/package-lock.json ./
# --ignore-scripts, and not for tidiness.
#
# esbuild's postinstall runs its own freshly written binary to check it, and on
# Docker's overlayfs that races the write:
#
#   npm error command sh -c node install.js
#   Error: spawnSync /src/bridge/node_modules/esbuild/bin/esbuild ETXTBSY
#
# The binary itself arrives from the @esbuild/* platform package, which is data
# rather than a script, so skipping the check costs nothing -- and the vite
# build in the next step is what proves the binary works.
RUN npm ci --no-audit --no-fund --ignore-scripts
COPY bridge/ ./
RUN npm run build


FROM node:22-slim AS web-build
WORKDIR /src/web
# npm's defaults give up quickly on a slow registry, and the failure it reports
# is ETIMEDOUT with a paragraph about proxy settings -- which sends you looking
# in entirely the wrong place. This build has failed that way once already.
RUN npm config set fetch-retries 6 \
 && npm config set fetch-retry-maxtimeout 120000 \
 && npm config set fetch-timeout 300000
COPY web/package.json web/package-lock.json ./
# --ignore-scripts, and not for tidiness.
#
# esbuild's postinstall runs its own freshly written binary to check it, and on
# Docker's overlayfs that races the write:
#
#   npm error command sh -c node install.js
#   Error: spawnSync /src/bridge/node_modules/esbuild/bin/esbuild ETXTBSY
#
# The binary itself arrives from the @esbuild/* platform package, which is data
# rather than a script, so skipping the check costs nothing -- and the vite
# build in the next step is what proves the binary works.
RUN npm ci --no-audit --no-fund --ignore-scripts
# The alias resolves outside this package's root, so the bridge's sources have
# to sit where tsconfig and astro.config expect them: one level up.
COPY bridge/ /src/bridge/
COPY web/ ./
# The PWA lands under /app/ on the finished server.
COPY --from=bridge-build /src/bridge/dist ./public/app
# The public host name behind the TLS proxy (docs/DEPLOYMENT.md). Astro writes
# it into the server at build time -- it is what the cross-site check compares
# the browser's Origin with -- so it is a build argument, declared this late so
# that changing it does not invalidate the `npm ci` layer above.
ARG DASHBOARD_HOST=""
RUN DASHBOARD_HOST="$DASHBOARD_HOST" npm run build

# Reduce the very same tree to what the server needs, rather than installing a
# second one in the runtime stage.
#
# That third `npm ci` was a third trip to the registry, and on a flaky night it
# is the one that fails -- 990 seconds in before ETIMEDOUT on a transitive
# dependency, with two perfectly good build stages already behind it. Pruning
# what is already on disk cannot time out.
RUN npm prune --omit=dev


FROM node:22-slim AS runtime
WORKDIR /app
ENV NODE_ENV=production

COPY web/package.json ./
COPY --from=web-build /src/web/node_modules ./node_modules
COPY --from=web-build /src/web/dist ./dist
COPY --from=web-build /src/web/drizzle ./drizzle
COPY --from=web-build /src/web/src/db ./src/db
COPY --from=web-build /src/web/src/lib ./src/lib
# docs/DEPLOYMENT.md has the operator register a node with this, so it has to be
# in the image. Without it `device.ts add` fails with ERR_MODULE_NOT_FOUND and
# there is no way to issue an ingest token at all.
COPY --from=web-build /src/web/scripts ./scripts
COPY web/docker-entrypoint.sh /usr/local/bin/entrypoint
RUN chmod +x /usr/local/bin/entrypoint

# What the server above was built for, so the entrypoint can tell a changed
# DASHBOARD_HOST from one that is in effect.
ARG DASHBOARD_HOST=""
ENV MAXL_BUILT_FOR_HOST=$DASHBOARD_HOST

# Never root. The process needs to read its own files and nothing else.
USER node

ENV HOST=0.0.0.0 PORT=4321
EXPOSE 4321

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD node -e "fetch('http://127.0.0.1:4321/login').then(r=>process.exit(r.status<500?0:1)).catch(()=>process.exit(1))"

ENTRYPOINT ["entrypoint"]
