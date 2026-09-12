export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/pocketwiki" || url.pathname === "/pocketwiki/" || url.pathname === "/pocketwiki2" || url.pathname === "/pocketwiki2/") {
      const route = url.pathname.startsWith("/pocketwiki2") ? "pocketwiki2" : "pocketwiki";
      url.pathname = `/${route}/index.html`;
      return env.ASSETS.fetch(new Request(url, request));
    }
    return env.ASSETS.fetch(request);
  },
};
