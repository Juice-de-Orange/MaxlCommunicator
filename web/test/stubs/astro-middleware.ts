/*
 * `astro:middleware` is a virtual module that only exists inside an Astro build.
 * All the middleware takes from it is `defineMiddleware`, which is the identity
 * function with types -- so this is the whole of it, and it lets a test call
 * `onRequest` directly.
 */

export function defineMiddleware<T>(handler: T): T {
  return handler;
}
