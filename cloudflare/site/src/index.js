const PAGES = {
  "/pocketwiki": "pocketwiki",
  "/pocketwiki/": "pocketwiki",
};

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const page = PAGES[url.pathname];
    if (page) {
      url.pathname = `/${page}/index.html`;
      return env.ASSETS.fetch(new Request(url, request));
    }
    // The route claims the whole /pocketwiki* prefix, and an unknown path
    // cannot be re-fetched from the assets binding, which used to answer 500.
    // Assets themselves never reach here: they are served before the worker.
    return new Response("Not found\n", {
      status: 404,
      headers: { "content-type": "text/plain; charset=utf-8" },
    });
  },
};
