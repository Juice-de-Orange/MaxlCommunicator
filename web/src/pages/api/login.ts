import type { APIRoute } from "astro";

import { clearSessionCookie, createSessionCookie, safeEquals } from "../../lib/auth";

export const prerender = false;

export const POST: APIRoute = async ({ request, redirect }) => {
  const expected = process.env.DASHBOARD_PASSWORD;
  const secret = process.env.SESSION_SECRET;
  if (!expected || !secret) {
    return new Response("DASHBOARD_PASSWORD or SESSION_SECRET is not set.", { status: 500 });
  }

  const form = await request.formData();
  const supplied = String(form.get("password") ?? "");

  // Constant time, and a fixed delay on failure. Neither matters much against a
  // single password over TLS, and both cost nothing.
  if (!safeEquals(supplied, expected)) {
    await new Promise((resolve) => setTimeout(resolve, 400));
    return redirect("/login?failed=1", 303);
  }

  const response = redirect("/", 303);
  response.headers.append("set-cookie", createSessionCookie(secret));
  return response;
};

export const DELETE: APIRoute = async ({ redirect }) => {
  const response = redirect("/login", 303);
  response.headers.append("set-cookie", clearSessionCookie());
  return response;
};
