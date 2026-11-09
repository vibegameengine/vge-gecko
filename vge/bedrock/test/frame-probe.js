(() => {
  const calls = {};
  const watch = (object, names) => {
    for (const name of names) {
      const original = object[name];
      if (typeof original !== "function") continue;
      object[name] = function (...args) {
        const source = args.find((value) => value && typeof value === "object" && value.constructor);
        const numbers = args.filter((value) => typeof value === "number").join(",");
        const key = name + "(" + numbers + ")" + (source && name !== "createImageBitmap" ? ":" + source.constructor.name : "");
        calls[key] = (calls[key] ?? 0) + 1;
        return original.apply(this, args);
      };
    }
  };
  for (const proto of [WebGLRenderingContext.prototype, WebGL2RenderingContext.prototype]) {
    for (const name of Object.getOwnPropertyNames(proto)) {
      if (/^(create|delete|tex|copyTex|compressedTex|renderbuffer|framebuffer|readPixels|blit|invalidate|bufferData|bufferSubData|getBufferSubData|generateMipmap|fenceSync|clientWaitSync)/.test(name)) {
        watch(proto, [name]);
      }
    }
  }
  for (const dimension of ["width", "height"]) {
    const descriptor = Object.getOwnPropertyDescriptor(HTMLCanvasElement.prototype, dimension);
    Object.defineProperty(HTMLCanvasElement.prototype, dimension, {
      get() { return descriptor.get.call(this); },
      set(value) {
        calls["canvas." + dimension + "=" + value] = (calls["canvas." + dimension + "=" + value] ?? 0) + 1;
        descriptor.set.call(this, value);
      },
      configurable: true,
    });
  }
  watch(CanvasRenderingContext2D.prototype, ["drawImage", "getImageData", "createPattern"]);
  watch(window, ["createImageBitmap"]);
  watch(HTMLCanvasElement.prototype, ["getContext", "toBlob", "toDataURL"]);
  setInterval(() => {
    console.log("frameprobe calls " + JSON.stringify(calls));
    for (const key of Object.keys(calls)) delete calls[key];
  }, 5000);
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
