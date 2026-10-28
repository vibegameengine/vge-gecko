const html = `<!doctype html>
<html><head><style>
  body { margin: 0; font: 16px sans-serif; }
  #bar { display: flex; gap: 10px; padding: 8px; width: 400px; background: #222; }
  .cell { flex: 1; height: 40px; background: #4a4; }
  #wide { flex: 2; }
  p { margin: 12px 8px; width: 200px; }
</style></head>
<body>
  <div id="bar"><div class="cell"></div><div class="cell" id="wide"></div><div class="cell"></div></div>
  <p id="text">Gecko layout inside VgeBedrockRuntime, wrapping onto more than one line.</p>
</body></html>`;

const browser = Services.appShell.createWindowlessBrowser(false);
browser.docShell.QueryInterface(Ci.nsIWebNavigation).loadURI(
  Services.io.newURI("data:text/html;charset=utf-8," + encodeURIComponent(html)),
  { triggeringPrincipal: Services.scriptSecurityManager.getSystemPrincipal() }
);

Services.tm.spinEventLoopUntil(
  "vge-bedrock-layout-smoke",
  () =>
    browser.document &&
    browser.document.documentURI.startsWith("data:") &&
    browser.document.readyState === "complete"
);

const doc = browser.document;
for (const element of doc.querySelectorAll("*")) {
  const box = element.getBoundingClientRect();
  const id = element.id ? "#" + element.id : element.className ? "." + element.className : "";
  print(`${element.localName}${id} x=${box.x} y=${box.y} w=${box.width} h=${box.height}`);
}

browser.close();
