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
    if (delta > 25) over25++;
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
