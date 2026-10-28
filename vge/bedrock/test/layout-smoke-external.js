const fromFile = document.createElement("div");
fromFile.id = "external";
fromFile.className = "cell";
document.getElementById("bar").appendChild(fromFile);
console.log("external script ran, cells:", document.getElementsByClassName("cell").length);
