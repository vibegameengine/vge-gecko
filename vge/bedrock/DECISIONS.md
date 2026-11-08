# VgeBedrockRuntime decisions

Each entry records what was decided and why, and what would make us revisit it.

## 2026-11-09: `target=_blank` links open in the same window

**Decision.** A link with `target=_blank`, and `window.open`, loads in the current window. The host sets
`browser.link.open_newwindow` to `OPEN_CURRENTWINDOW` and `browser.link.open_newwindow.restriction` to 0, and installs
`SingleWindowOpener`, an `nsIBrowserDOMWindow` that returns the content browsing context of the window
(`runtime/VgeBedrockRuntime.cpp`).

**Why.** Firefox opens these links through its own browser UI: `nsContentTreeOwner::ProvideWindow` asks the chrome
window's `nsIBrowserDOMWindow`, which browser.xhtml implements with tabs. The host has no browser UI and no tabs.

- With no window creator set, `nsWindowWatcher::OpenWindowInternal` hit a diagnostic assertion and the process ended
  when a user clicked such a link.
- With a window creator alone, a new top-level window appeared with nothing in it, because no code puts a content
  frame into that window.

**What this costs.** A page cannot open a second window. A page that expects `window.open` to give it a separate
window, such as a sign-in popup, gets its own window navigated away instead.

**Revisit when** the host needs real popups or tabs. The final form is a window creator that builds the same chrome
shell and content frame as `CreateContentDocShell` for every new window, or tabs in the host's UI.
