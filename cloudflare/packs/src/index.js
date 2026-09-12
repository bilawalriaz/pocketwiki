const CORS = {
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "GET, HEAD, OPTIONS",
  "access-control-allow-headers": "content-type, if-none-match",
  "access-control-expose-headers": "content-length, etag",
};

export default {
  async fetch(request, env) {
    if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });
    if (request.method !== "GET" && request.method !== "HEAD") {
      return new Response("Method not allowed", { status: 405, headers: { ...CORS, allow: "GET, HEAD, OPTIONS" } });
    }

    const url = new URL(request.url);
    const key = url.pathname === "/" ? "index.json" : decodeURIComponent(url.pathname.slice(1));
    if (!key || key.includes("..") || key.startsWith("/")) {
      return new Response("Invalid path", { status: 400, headers: CORS });
    }

    const onlyIf = request.headers.get("if-none-match");
    const object = await env.PACKS.get(key, onlyIf ? { onlyIf: { etagDoesNotMatch: onlyIf.replaceAll('"', "") } } : undefined);
    if (!object) return new Response("Not found", { status: 404, headers: CORS });

    const headers = new Headers(CORS);
    object.writeHttpMetadata(headers);
    headers.set("etag", object.httpEtag);
    headers.set("x-content-type-options", "nosniff");
    headers.set("cache-control", key === "index.json"
      ? "public, max-age=300, stale-while-revalidate=3600"
      : "public, max-age=31536000, immutable");
    if (!object.body) return new Response(null, { status: 304, headers });
    return new Response(request.method === "HEAD" ? null : object.body, { headers });
  },
};
