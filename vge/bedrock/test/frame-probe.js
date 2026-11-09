(() => {
  let last = performance.now();
  let frames = 0;
  let elapsed = 0;
  let longest = 0;
  let over25 = 0;
  let over50 = 0;
  const tick = (now) => {
    const delta = now - last;
    last = now;
    frames++;
    elapsed += delta;
    longest = Math.max(longest, delta);
    if (delta > 25) {
      over25++;
      console.log("frameprobe spike at " + Math.round(now) + " ms length " + delta.toFixed(1));
    }
    if (delta > 50) over50++;
    if (elapsed >= 5000) {
      console.log("frameprobe fps " + Math.round(frames * 1000 / elapsed) +
                  " longest " + longest.toFixed(1) + " over25 " + over25 + " over50 " + over50 +
                  " canvas " + (document.querySelector("canvas")?.width ?? 0));
      frames = elapsed = longest = over25 = over50 = 0;
    }
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
  let lastSample = 0;
  const sample = (now) => {
    if (now - lastSample < 1000) return;
    lastSample = now;
    const canvas = document.querySelector("canvas");
    const gl = canvas && canvas.getContext("webgl2");
    if (!gl) return;
    const bound = gl.getParameter(gl.READ_FRAMEBUFFER_BINDING);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    const pixels = new Uint8Array(canvas.width * canvas.height * 4);
    gl.readPixels(0, 0, canvas.width, canvas.height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, bound);
    let hash = 0;
    for (const value of pixels) hash = (hash * 31 + value) | 0;
    console.log("frameprobe glhash " + Math.round(now) + " " + hash + " error " + gl.getError());
  };
  const nativeRaf = window.requestAnimationFrame.bind(window);
  window.requestAnimationFrame = (callback) => nativeRaf((now) => {
    callback(now);
    sample(now);
  });
  const starter = setInterval(() => {
    const play = document.querySelector("#play.art-ready");
    if (play && play.offsetParent) {
      clearInterval(starter);
      play.click();
      console.log("frameprobe started gameplay at " + Math.round(performance.now()));
      setInterval(() => {
        const skip = document.querySelector("#scene-skip");
        if (skip && skip.offsetParent) skip.click();
      }, 1000);
    }
  }, 500);
})();
