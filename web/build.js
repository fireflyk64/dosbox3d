// This page's build, and another page's against it (the user, 2026-11-17: "assuming we have older retrovega players
// with un-updated browsers who haven't reloaded paired with newer players ... Can we detect it somehow in the lobby
// and advise the older players to reload").  Two pages fly together only as the same build: the game's data and
// logic step in lockstep on every machine, and a page left open across a deploy keeps its old ones until it is
// reloaded.  scripts/build-web.sh writes the line below into the copy it deploys: the commit ("+" for a tree with
// changes) and the moment of the build, in seconds.  A page served from the tree says "dev" and is never compared.
export const PAGE_BUILD = { id: "dev", at: 0 };

// The site within the origin this page is (/dosbox/, /terrain/: the terrain branch's playtest copy is another game
// for now, on the same lobby).
export const PAGE_SITE = typeof location !== "undefined" ? location.pathname.replace(/[^/]*$/, "") : "/";   // (no location: node's tests)

// What a hello says of its page: put into the lobby's and the room's hellos.
export const buildFields = () => ({ build: PAGE_BUILD.id, built: PAGE_BUILD.at, site: PAGE_SITE });

// Another page's build, from its hello, against this one: "same"; "newer" or "older" (theirs, the same site);
// "site" (another site's page); "none" (it said nothing: a page from before builds were compared, of this site and
// older, or another site's that does not compare yet; it hears nothing of ours but the chat).  A "dev" page on either
// side is "same".
export function compareBuild(m) {
  if (!m || typeof m.build !== "string") return PAGE_BUILD.id === "dev" ? "same" : "none";
  if (typeof m.site === "string" && m.site !== PAGE_SITE) return "site";
  if (m.build === PAGE_BUILD.id || m.build === "dev" || PAGE_BUILD.id === "dev") return "same";
  const at = Number(m.built) || 0;
  return at > PAGE_BUILD.at ? "newer" : "older";
}

// When a build was made, as the pilot reads it.
export const builtWhen = (at) => (at ? new Date(at * 1000).toLocaleString([], { dateStyle: "medium", timeStyle: "short" }) : "earlier");

// How to reload for certain (a plain reload may keep a page's files from the cache a while).
export const RELOAD_HOW = "reload the page with Ctrl+Shift+R (Cmd+Shift+R on a Mac)";
