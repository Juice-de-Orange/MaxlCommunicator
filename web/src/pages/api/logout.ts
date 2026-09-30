import type { APIRoute } from "astro";

import { clearSessionCookie } from "../../lib/auth";

export const prerender = false;

export const POST: APIRoute = async ({ redirect }) => {
  const response = redirect("/login", 303);
  response.headers.append("set-cookie", clearSessionCookie());
  return response;
};
